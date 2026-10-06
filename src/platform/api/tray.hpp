#pragma once

// System tray contract.
//
// Evidence: docs/migration/cpp/feature-parity.md rows "Tray" and "Tray
// theme/recording glyph"; .NET reference VoiceTyper.App/Tray/TrayIcon.cs and
// VoiceTyper.App/Tray/ToastWindow.cs.
//
// Frozen observable contract:
//   * The tray is the application's home: closing the settings window hides it
//     and the process stays resident, so a dead-looking background app is a bug.
//   * The tray menu offers exactly these actions: open settings, record or
//     cancel (whichever the current state allows), quit.
//   * The tooltip reports engine/model readiness, so a user can tell from the
//     tray whether dictation is possible at all.
//
// Intent-parity decision recorded here: `ApplyTheme` and `SetRecording` on the
// .NET tray icon are no-ops today, and feature-parity.md classifies the tray
// theme/recording glyph as non-blocking pending an explicit product decision.
// The C++ interface therefore *declares* the capability and marks it as awaiting
// that decision rather than pretending the .NET no-op is a behavior to copy. The
// readiness tooltip is blocking and is implemented regardless.
//
// Thread affinity: every method must be called on the UI thread. A worker thread
// that needs to change the tray posts to the UI thread. Exceptions cannot
// escape. Ownership: the tray is owned by the UI layer; callbacks it invokes are
// owned by the caller and must not be called after quit().

#include "domain/error.hpp"
#include "domain/settings.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace voicetyper::platform {

using domain::AppSettings;
using domain::AppTheme;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// Menu and notification actions a tray can raise.
enum class TrayAction : std::uint8_t {
    /// "Open settings" was chosen.
    open_settings = 0,
    /// "Record" was chosen (push-to-talk/toggle start).
    record = 1,
    /// "Cancel" was chosen.
    cancel = 2,
    /// "Quit" was chosen. The only action that ends the process.
    quit = 3,
};

[[nodiscard]] constexpr std::string_view tray_action_name(TrayAction action) noexcept
{
    switch (action) {
    case TrayAction::open_settings: return "open_settings";
    case TrayAction::record: return "record";
    case TrayAction::cancel: return "cancel";
    case TrayAction::quit: return "quit";
    }
    return "unknown";
}

/// Which of record/cancel the menu should currently offer. The tray decides this
/// from the recording state so the menu can never show a dead action.
enum class TrayRecordingState : std::uint8_t {
    /// Idle: offer "record".
    idle = 0,
    /// Recording: offer "cancel".
    recording = 1,
    /// Transcribing/processing: offer "cancel".
    processing = 2,
};

[[nodiscard]] constexpr std::string_view tray_recording_state_name(TrayRecordingState state) noexcept
{
    switch (state) {
    case TrayRecordingState::idle: return "idle";
    case TrayRecordingState::recording: return "recording";
    case TrayRecordingState::processing: return "processing";
    }
    return "unknown";
}

/// Engine/model readiness, surfaced in the tooltip.
struct TrayReadiness {
    /// The model file selected in settings exists on disk.
    bool model_present = false;
    /// The selected engine finished loading and can transcribe.
    bool engine_ready = false;
    /// Reason the engine is not ready, for the tooltip's detail line.
    std::optional<std::string> detail;
};

/// Receives tray actions on the UI thread.
using TrayActionSink = std::function<void(TrayAction)>;

class Tray {
public:
    virtual ~Tray() = default;

    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;
    Tray(Tray&&) = delete;
    Tray& operator=(Tray&&) = delete;

    /// Installs the action sink. Must be called before show().
    virtual Status set_action_sink(TrayActionSink sink) = 0;

    /// Creates the tray icon and makes it visible.
    /// Failure codes: unavailable (no system tray on this session),
    /// unsupported (platform has no tray concept).
    virtual Status show() = 0;

    /// Hides the icon without tearing the sink down.
    virtual Status hide() = 0;

    /// Sets the hover text. Must not contain recognized audio text or secrets.
    virtual Status set_tooltip(std::string_view text) = 0;

    /// Replaces the tooltip with a readiness-derived string.
    virtual Status set_readiness(const TrayReadiness& readiness) = 0;

    /// Switches the menu between "record" and "cancel".
    virtual Status set_recording_state(TrayRecordingState state) = 0;

    /// Updates the icon/overlay glyph for the current state.
    ///
    /// Intent-parity note: this has no counterpart in the current .NET build
    /// (its SetRecording is a no-op) and is pending the explicit product
    /// decision feature-parity.md calls for. Declared, not specified: what the
    /// glyph looks like is a UI decision, and a backend may legitimately return
    /// ErrorCode::unsupported until that decision is made.
    virtual Status set_recording_glyph(TrayRecordingState state) = 0;

    /// Applies the application theme to the tray menu/tooltip colors.
    ///
    /// Same intent-parity caveat as set_recording_glyph(): the .NET ApplyTheme is
    /// a no-op and the visual result is an open product decision.
    virtual Status apply_theme(AppTheme theme) = 0;

    /// Shows a transient notification (the .NET "toast window").
    virtual Status notify(std::string_view title, std::string_view message) = 0;

    /// Removes the icon and releases the sink. Idempotent; called during
    /// shutdown.
    virtual Status quit() = 0;

protected:
    Tray() = default;
};

} // namespace voicetyper::platform
