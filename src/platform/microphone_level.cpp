#include "platform/api/microphone_level.hpp"

#include <algorithm>
#include <cmath>

#if defined(_WIN32)
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#endif

namespace voicetyper::platform {

double clamp_microphone_level(double level) noexcept
{
    if (std::isnan(level)) {
        return 0.0;
    }
    return std::clamp(level, 0.0, 1.0);
}

int microphone_level_percent(double level) noexcept
{
    return static_cast<int>(std::lround(clamp_microphone_level(level) * 100.0));
}

double microphone_level_from_percent(int percent) noexcept
{
    const int clamped = std::clamp(percent, 0, 100);
    return static_cast<double>(clamped) / 100.0;
}

MicrophoneLevelController::MicrophoneLevelController(std::unique_ptr<MicrophoneLevelPort> port)
    : port_(std::move(port))
{
}

bool MicrophoneLevelController::available() const noexcept
{
    return port_ != nullptr && state_.available;
}

MicrophoneLevelState MicrophoneLevelController::current() const noexcept
{
    return state_;
}

MicrophoneLevelState MicrophoneLevelController::refresh()
{
    if (port_ == nullptr) {
        state_ = MicrophoneLevelState{};
        return state_;
    }
    state_ = port_->read();
    return state_;
}

Status MicrophoneLevelController::set_percent(int percent)
{
    if (port_ == nullptr) {
        return Status::failure(ErrorCode::unavailable,
            "this platform has no microphone level control");
    }
    // The mute flag is the platform's, not the slider's: a sensitivity change must
    // not un-mute a microphone the user muted in Windows.
    const bool muted = state_.muted;
    const Status status = port_->write(microphone_level_from_percent(percent), muted);
    if (status.is_error()) {
        return status;
    }
    state_.level = microphone_level_from_percent(percent);
    state_.available = true;
    state_.muted = muted;
    return Status::success();
}

#if defined(_WIN32)

namespace {

template <typename T>
void release(T*& pointer)
{
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

/// The Windows control: IAudioEndpointVolume on the default capture endpoint, which
/// is the very value the Sound control panel edits, so the slider and Windows never
/// disagree.
class WindowsMicrophoneLevel final : public MicrophoneLevelPort {
public:
    [[nodiscard]] MicrophoneLevelState read() override
    {
        IAudioEndpointVolume* volume = open();
        if (volume == nullptr) {
            return {};
        }
        BOOL muted = FALSE;
        float level = 0.0F;
        const HRESULT mute_status = volume->GetMute(&muted);
        const HRESULT level_status = volume->GetMasterVolumeLevelScalar(&level);
        volume->Release();
        if (FAILED(mute_status) || FAILED(level_status)) {
            return {};
        }
        return MicrophoneLevelState{true, muted != FALSE, static_cast<double>(level)};
    }

    [[nodiscard]] Status write(double level, bool muted) override
    {
        IAudioEndpointVolume* volume = open();
        if (volume == nullptr) {
            return Status::failure(ErrorCode::unavailable,
                "the default recording endpoint has no level control");
        }
        const HRESULT mute_status = volume->SetMute(muted ? TRUE : FALSE, nullptr);
        const HRESULT level_status = volume->SetMasterVolumeLevelScalar(
            static_cast<float>(clamp_microphone_level(level)), nullptr);
        volume->Release();
        if (FAILED(mute_status) || FAILED(level_status)) {
            return Status::failure(ErrorCode::permission_denied,
                "the device refused the input level");
        }
        return Status::success();
    }

private:
    static IAudioEndpointVolume* open()
    {
        // COM is initialised by the application (Qt does it for the GUI thread); if it
        // is not, CoCreateInstance says so and the caller reports "unavailable" rather
        // than crashing.
        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || enumerator == nullptr) {
            return nullptr;
        }
        IMMDevice* device = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        IAudioEndpointVolume* volume = nullptr;
        if (SUCCEEDED(hr) && device != nullptr) {
            hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&volume));
        }
        release(device);
        release(enumerator);
        if (FAILED(hr)) {
            release(volume);
            return nullptr;
        }
        return volume;
    }
};

} // namespace

std::unique_ptr<MicrophoneLevelPort> create_microphone_level()
{
    return std::make_unique<WindowsMicrophoneLevel>();
}

#else

std::unique_ptr<MicrophoneLevelPort> create_microphone_level()
{
    // No level control outside Windows: the UI disables the slider and says why.
    return nullptr;
}

#endif

} // namespace voicetyper::platform
