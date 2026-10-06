#pragma once

// Auto-paste (synthetic Ctrl+V) contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §3 "Clipboard and
// paste"; .NET reference VoiceTyper.Core/Services/TextOutputService.cs and
// VoiceTyper.App/Services/InputSimulatorPaster.cs.
//
// Frozen observable contract:
//   * Clipboard first, paste second. The transcript is already on the clipboard
//     before paste() is ever called; a paste failure never removes it.
//   * The paste happens 80 ms after the clipboard write, so the clipboard has
//     settled. That delay is part of the compatibility contract, not a tuning
//     knob.
//   * Injection is best effort. A UAC integrity mismatch (the app runs lower
//     integrity than the focused window) makes SendInput fail; the correct
//     outcome is an explicit clipboard-only state, not an error the user cannot
//     act on, and never a silent success.
//
// Intent-parity note: the Linux branch of the .NET project inherited the same
// rule — clipboard mandatory, injection best effort with an explicit
// `clipboard-only` state. That state is expressed here as
// TextOutputOutcome::clipboard_only so both platforms report it identically.
//
// Thread affinity: paste() synthesizes input for whatever window currently has
// focus, so it must run on the thread that owns input handling for the target
// platform (the UI thread on both planned backends) and must not run while the
// settings window is capturing a hotkey.
//
// Ownership: stateless; the backend holds no text.

#include "domain/error.hpp"

#include <chrono>
#include <string_view>

namespace voicetyper::platform {

using domain::ErrorCode;
using domain::Status;

/// Delay between the clipboard write and the synthetic Ctrl+V, milliseconds.
/// Frozen by compatibility-contracts.md §3.
inline constexpr std::chrono::milliseconds kPasteDelay{80};

/// The composite key chord sent to paste. Frozen: the product only ever
/// simulates plain Ctrl+V.
inline constexpr std::string_view kPasteChord = "Ctrl+V";

/// Outcome of one "put the text where the user is typing" operation.
///
/// The caller (text output) decides the outcome from the clipboard status and
/// the paste status, so the states are reported separately and never merged.
enum class TextOutputOutcome : std::uint8_t {
    /// Nothing was written: the transcript was empty or whitespace-only.
    /// The clipboard was not touched at all.
    skipped_empty = 0,
    /// The clipboard was written and the paste was injected.
    pasted = 1,
    /// The clipboard was written but the paste could not be injected
    /// (UAC integrity mismatch, no input backend, target rejected the event).
    /// This is a normal, expected state and not a failure.
    clipboard_only = 2,
    /// The clipboard write itself failed; the transcript was not delivered.
    clipboard_failed = 3,
};

[[nodiscard]] constexpr std::string_view text_output_outcome_name(TextOutputOutcome outcome) noexcept
{
    switch (outcome) {
    case TextOutputOutcome::skipped_empty: return "skipped_empty";
    case TextOutputOutcome::pasted: return "pasted";
    case TextOutputOutcome::clipboard_only: return "clipboard_only";
    case TextOutputOutcome::clipboard_failed: return "clipboard_failed";
    }
    return "unknown";
}

/// True when the transcript reached the user through the clipboard, which is
/// the condition that makes the operation a success.
[[nodiscard]] constexpr bool is_delivered(TextOutputOutcome outcome) noexcept
{
    return outcome == TextOutputOutcome::pasted || outcome == TextOutputOutcome::clipboard_only;
}

/// Injects a synthetic paste chord into the focused window.
///
/// Implementations must not clear or otherwise disturb the clipboard.
class PasteSimulator {
public:
    virtual ~PasteSimulator() = default;

    PasteSimulator(const PasteSimulator&) = delete;
    PasteSimulator& operator=(const PasteSimulator&) = delete;
    PasteSimulator(PasteSimulator&&) = delete;
    PasteSimulator& operator=(PasteSimulator&&) = delete;

    /// Sends the paste chord once.
    ///
    /// Failure codes: permission_denied (integrity level below the focused
    /// window, i.e. the UAC case), unavailable (no input-injection backend on
    /// this platform or the window does not accept synthetic input).
    /// Both are reported so the caller can raise the clipboard_only state; the
    /// clipboard content is unaffected either way.
    virtual Status paste() = 0;

    /// True when injection is expected to work in this session. A false value is
    /// a diagnostic hint, not a guarantee: UAC and window policies can still
    /// refuse the injection.
    [[nodiscard]] virtual bool is_injection_supported() const noexcept = 0;

protected:
    PasteSimulator() = default;
};

} // namespace voicetyper::platform
