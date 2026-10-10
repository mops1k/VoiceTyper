#include "platform/linux/linux_microphone.hpp"

#include "platform/linux/pulse_support.hpp"

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace voicetyper::platform::linuxos {
namespace {

constexpr auto kPulseTimeout = std::chrono::milliseconds(3000);

/// One enumeration pass: the server's default source plus every non-monitor
/// capture source.
struct SourceListRequest {
    std::string default_name;
    std::vector<MicrophoneDevice> devices;
    bool done = false;
    std::string error;
};

void source_info_callback(pa_context*, const pa_source_info* info, int eol, void* userdata)
{
    auto* request = static_cast<SourceListRequest*>(userdata);
    if (eol > 0) {
        request->done = true;
        return;
    }
    if (eol < 0) {
        request->error = "the sound server refused the source list";
        request->done = true;
        return;
    }
    if (info == nullptr) {
        return;
    }
    // A monitor source is a loopback of an output device: it would show up in
    // the microphone dropdown as a device that hears the speakers.
    if (info->monitor_of_sink != PA_INVALID_INDEX) {
        return;
    }
    MicrophoneDevice device;
    device.id = info->name != nullptr ? info->name : "";
    if (device.id.empty()) {
        return;
    }
    device.name = info->description != nullptr && *info->description != '\0'
        ? info->description
        : device.id;
    device.is_default = !request->default_name.empty() && request->default_name == device.id;
    if (info->sample_spec.rate > 0 && info->sample_spec.channels > 0) {
        device.native_format = domain::AudioFormat(static_cast<std::uint32_t>(info->sample_spec.rate),
            static_cast<std::uint16_t>(info->sample_spec.channels), domain::SampleFormat::pcm_s16);
    }
    request->devices.push_back(std::move(device));
}

} // namespace

LinuxMicrophoneService::LinuxMicrophoneService() = default;

LinuxMicrophoneService::~LinuxMicrophoneService() = default;

std::vector<MicrophoneDevice> LinuxMicrophoneService::list_devices()
{
    diagnostics_.clear();

    PulseConnection connection;
    std::string error;
    if (!connection.connect(kPulseTimeout, error)) {
        diagnostics_ = error;
        return {};
    }

    SourceListRequest request;
    request.default_name = connection.default_source_name(kPulseTimeout, error);

    pa_operation* operation = pa_context_get_source_info_list(
        connection.context(), &source_info_callback, &request);
    if (!connection.wait(operation, kPulseTimeout, error)) {
        diagnostics_ = error;
        return {};
    }
    if (!request.error.empty()) {
        diagnostics_ = request.error;
        return {};
    }
    // "No capture device attached" is a success with an empty list; the
    // diagnostic is only set when the enumeration itself failed.
    return request.devices;
}

std::string LinuxMicrophoneService::default_device_id()
{
    diagnostics_.clear();

    PulseConnection connection;
    std::string error;
    if (!connection.connect(kPulseTimeout, error)) {
        diagnostics_ = error;
        return {};
    }
    const std::string name = connection.default_source_name(kPulseTimeout, error);
    if (name.empty() && !error.empty()) {
        diagnostics_ = error;
    }
    return name;
}

bool LinuxMicrophoneService::is_device_available(const std::string& device_id)
{
    if (device_id.empty()) {
        return !default_device_id().empty();
    }
    const auto devices = list_devices();
    return std::any_of(devices.begin(), devices.end(),
        [&device_id](const MicrophoneDevice& device) { return device.id == device_id; });
}

std::string LinuxMicrophoneService::diagnostics() const
{
    return diagnostics_;
}

} // namespace voicetyper::platform::linuxos
