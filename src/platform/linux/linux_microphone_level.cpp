#include "platform/linux/linux_microphone_level.hpp"

#include "platform/linux/pulse_support.hpp"

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <chrono>
#include <memory>

namespace voicetyper::platform::linuxos {
namespace {

constexpr auto kPulseTimeout = std::chrono::milliseconds(3000);

/// One read of the default source's volume state.
struct SourceVolumeRequest {
    bool done = false;
    bool available = false;
    double level = 0.0;
    bool muted = false;
    std::string name;
    std::uint8_t channels = 2;
    std::string error;
};

void source_volume_callback(pa_context*, const pa_source_info* info, int eol, void* userdata)
{
    auto* request = static_cast<SourceVolumeRequest*>(userdata);
    if (eol > 0) {
        request->done = true;
        return;
    }
    if (eol < 0) {
        request->error = "the sound server refused the source volume";
        request->done = true;
        return;
    }
    if (info == nullptr) {
        return;
    }
    request->available = true;
    request->muted = info->mute != 0;
    request->channels = info->volume.channels > 0 ? info->volume.channels : 2;
    request->name = info->name != nullptr ? info->name : "";
    // The server stores a cubic volume; the UI works with the linear scalar that
    // 100 % in pavucontrol also shows.
    const double linear = pa_sw_volume_to_linear(pa_cvolume_avg(&info->volume));
    request->level = std::clamp(linear, 0.0, 1.0);
}

/// Reads the default source's state, reporting why it could not.
bool read_default_source(PulseConnection& connection, SourceVolumeRequest& request, std::string& error)
{
    const std::string name = connection.default_source_name(kPulseTimeout, error);
    if (name.empty()) {
        if (error.empty()) {
            error = "the sound server reported no default source";
        }
        return false;
    }
    pa_operation* operation = pa_context_get_source_info_by_name(
        connection.context(), name.c_str(), &source_volume_callback, &request);
    if (!connection.wait(operation, kPulseTimeout, error)) {
        return false;
    }
    if (!request.error.empty()) {
        error = request.error;
        return false;
    }
    return request.available;
}

} // namespace

LinuxMicrophoneLevelPort::LinuxMicrophoneLevelPort() = default;

LinuxMicrophoneLevelPort::~LinuxMicrophoneLevelPort() = default;

MicrophoneLevelState LinuxMicrophoneLevelPort::read()
{
    diagnostics_.clear();
    MicrophoneLevelState state;

    PulseConnection connection;
    std::string error;
    if (!connection.connect(kPulseTimeout, error)) {
        diagnostics_ = error;
        return state;
    }

    SourceVolumeRequest request;
    if (!read_default_source(connection, request, error)) {
        diagnostics_ = error;
        return state;
    }

    state.available = true;
    state.muted = request.muted;
    state.level = request.level;
    return state;
}

domain::Status LinuxMicrophoneLevelPort::write(double level, bool muted)
{
    diagnostics_.clear();

    PulseConnection connection;
    std::string error;
    if (!connection.connect(kPulseTimeout, error)) {
        diagnostics_ = error;
        return domain::Status::failure(domain::ErrorCode::unavailable, error);
    }

    SourceVolumeRequest request;
    if (!read_default_source(connection, request, error)) {
        diagnostics_ = error;
        return domain::Status::failure(domain::ErrorCode::unavailable, error);
    }

    const double clamped = platform::clamp_microphone_level(level);
    pa_cvolume volume;
    pa_cvolume_init(&volume);
    pa_cvolume_set(&volume, request.channels, pa_sw_volume_from_linear(clamped));

    pa_operation* volume_operation = pa_context_set_source_volume_by_name(
        connection.context(), request.name.c_str(), &volume, nullptr, nullptr);
    if (!connection.wait(volume_operation, kPulseTimeout, error)) {
        diagnostics_ = error;
        return domain::Status::failure(domain::ErrorCode::io_failure, error);
    }

    pa_operation* mute_operation = pa_context_set_source_mute_by_name(
        connection.context(), request.name.c_str(), muted ? 1 : 0, nullptr, nullptr);
    if (!connection.wait(mute_operation, kPulseTimeout, error)) {
        diagnostics_ = error;
        return domain::Status::failure(domain::ErrorCode::io_failure, error);
    }
    return domain::Status::success();
}

std::string LinuxMicrophoneLevelPort::diagnostics() const
{
    return diagnostics_;
}

std::unique_ptr<MicrophoneLevelPort> create_linux_microphone_level()
{
    return std::make_unique<LinuxMicrophoneLevelPort>();
}

} // namespace voicetyper::platform::linuxos
