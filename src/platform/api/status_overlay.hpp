#pragma once

// Frameless status overlay contract.
//
// Evidence: docs/migration/cpp/feature-parity.md row "Status overlay"; .NET
// reference VoiceTyper.App/Overlay/StatusOverlayWindow.* and
// VoiceTyper.App/App.axaml.cs.
//
// Frozen observable contract:
//   * A frameless, always-on-top pill at the bottom center of the screen that
//     shows the current voice activity while dictating. It must not appear in
//     the taskbar, must not take focus and must not steal keystrokes from the
//     window the user is typing into.
//   * It shows at least: idle (hidden), recording, processing and error.
//   * The recording state pulses with a 350 ms period; that period is frozen
//     because it is part of the current look and timing behavior.
//
// Thread affinity: every method must run on the UI thread. Worker threads post to
// it. DPI/multi-monitor placement is a backend concern, but the overlay must be
// created on the same display the user last interacted with, and its geometry
// must be recomputed when that changes. Exceptions cannot escape.
//
// Ownership: the overlay is owned by the UI layer. Hiding it must not destroy the
// backend resources, because it is shown and hidden on every recording.

#include "domain/error.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace voicetyper::platform {

using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// Overlay state machine. The transitions match the recording state machine, so
/// the overlay can never show a state the recorder is not in.
enum class OverlayState : std::uint8_t {
    /// Not dictating: the overlay is hidden.
    idle = 0,
    /// Capturing audio.
    recording = 1,
    /// Transcribing or pasting.
    processing = 2,
    /// A recoverable problem (no model, device lost, engine unavailable).
    error = 3,
};

[[nodiscard]] constexpr std::string_view overlay_state_name(OverlayState state) noexcept
{
    switch (state) {
    case OverlayState::idle: return "idle";
    case OverlayState::recording: return "recording";
    case OverlayState::processing: return "processing";
    case OverlayState::error: return "error";
    }
    return "unknown";
}

/// Pulse period of the recording indicator, milliseconds. Frozen from the .NET
/// overlay (350 ms).
inline constexpr std::chrono::milliseconds kOverlayPulsePeriod{350};

class StatusOverlay {
public:
    virtual ~StatusOverlay() = default;

    StatusOverlay(const StatusOverlay&) = delete;
    StatusOverlay& operator=(const StatusOverlay&) = delete;
    StatusOverlay(StatusOverlay&&) = delete;
    StatusOverlay& operator=(StatusOverlay&&) = delete;

    /// Creates the overlay window in the hidden idle state. Idempotent.
    /// Failure codes: unsupported (no GUI session), unavailable (no display).
    virtual Status create() = 0;

    /// Shows the overlay and switches it to `state`. `detail` is optional
    /// secondary text, e.g. a stop reason or an error message. It must never
    /// contain recognized text or secrets.
    virtual Status show(OverlayState state, std::string_view detail = {}) = 0;

    /// Switches the visible state without recreating the window.
    virtual Status set_state(OverlayState state, std::string_view detail = {}) = 0;

    /// Hides the overlay. The next show() reuses the same window.
    virtual Status hide() = 0;

    /// The state the overlay is currently displaying, for tests and for the
    /// settings Log page.
    [[nodiscard]] virtual OverlayState current_state() const noexcept = 0;

    /// Destroys the overlay window. Idempotent; called during shutdown.
    virtual Status destroy() = 0;

protected:
    StatusOverlay() = default;
};

} // namespace voicetyper::platform
