#pragma once

// The microphone's own input level and mute state (device I/O only).
//
// Evidence: the .NET build never exposed this - it asked the user to "check the
// microphone level" in a warning and left them to the Windows control panel
// (VoiceTyper.Core/Audio/NativeWasapiCapture.cs and its "запись не дала данных
// (проверьте уровень микрофона)" message, which Alexander hit on 2026-10-06 while
// the app itself said nothing at all). Applications that use a microphone offer the
// two things he asked for instead: a sensitivity slider and a test with a level
// indicator, in the microphone page of their own settings.
//
// Platform mapping: Windows exposes this through IAudioEndpointVolume on the default
// capture endpoint - the same value the Sound control panel edits, so the slider and
// Windows stay in step. A platform without such a control reports unavailable and the
// UI disables the slider instead of pretending to work.
//
// Thread affinity: read/write are called from the application (UI) thread and may
// block on a COM call; they are never called from an audio callback.

#include "domain/error.hpp"

#include <memory>

namespace voicetyper::platform {

using domain::ErrorCode;
using domain::Status;

/// The endpoint's input level.
struct MicrophoneLevelState {
    /// False when the platform has no level control, or the endpoint could not be
    /// opened: the caller must then disable its controls rather than show a zero.
    bool available = false;
    bool muted = false;
    /// The platform's scalar, 0.0 - 1.0.
    double level = 0.0;
};

/// The seam: one implementation per platform, plus a portable fallback.
class MicrophoneLevelPort {
public:
    virtual ~MicrophoneLevelPort() = default;

    [[nodiscard]] virtual MicrophoneLevelState read() = 0;
    /// Applies the level (clamped to 0..1) and the mute flag. An implementation
    /// without a control returns unavailable rather than silently doing nothing.
    [[nodiscard]] virtual Status write(double level, bool muted) = 0;
};

/// Clamps to the range the platform understands.
[[nodiscard]] double clamp_microphone_level(double level) noexcept;
/// 0.0 - 1.0 to 0 - 100, rounded to the nearest whole percent.
[[nodiscard]] int microphone_level_percent(double level) noexcept;
/// 0 - 100 back to the platform scalar; anything outside the range is clamped.
[[nodiscard]] double microphone_level_from_percent(int percent) noexcept;

/// The app-side controller. It owns the port, remembers the last state it saw, and
/// keeps the platform's mute flag when the slider moves - the two are separate
/// controls in Windows and a sensitivity slider must not unmute a muted microphone
/// by itself.
class MicrophoneLevelController {
public:
    explicit MicrophoneLevelController(std::unique_ptr<MicrophoneLevelPort> port);

    MicrophoneLevelController(const MicrophoneLevelController&) = delete;
    MicrophoneLevelController& operator=(const MicrophoneLevelController&) = delete;

    [[nodiscard]] bool available() const noexcept;
    /// The last known state; a fresh read() is taken by refresh().
    [[nodiscard]] MicrophoneLevelState current() const noexcept;
    /// Re-reads the platform.
    [[nodiscard]] MicrophoneLevelState refresh();
    /// Applies a slider value. A missing or refusing port leaves the state as it was
    /// and returns the reason.
    [[nodiscard]] Status set_percent(int percent);

private:
    std::unique_ptr<MicrophoneLevelPort> port_;
    MicrophoneLevelState state_;
};

/// The platform's level control, or nullptr where the platform has none (the UI
/// then disables the slider instead of pretending to work).
[[nodiscard]] std::unique_ptr<MicrophoneLevelPort> create_microphone_level();

} // namespace voicetyper::platform
