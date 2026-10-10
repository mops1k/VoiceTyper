#pragma once

// Linux implementation of the frozen platform::PasteSimulator contract.
//
// Evidence and contract:
//   * src/platform/api/paste.hpp - one synthetic Ctrl+V (kPasteChord), the
//     clipboard is never touched, injection is best effort and a refusal is
//     reported so the caller can raise the explicit clipboard_only state.
//   * docs/migration/cpp/release-gates.md risk R8 - "Portal-first, X11/evdev
//     fallback, clipboard-first, explicit capability state": on Wayland the
//     compositor does not let a client inject input directly, so the injection
//     goes through the uinput-backed ydotool daemon, which is the only such
//     helper installed on the target machine (verified 2026-10-08: ydotool is
//     present, xdotool and wtype are not).
//
// Why the availability probe is honest rather than optimistic: a session
// without a running ydotool daemon, without /dev/uinput access or without the
// tool itself cannot inject anything. Saying so up front turns "the text was not
// pasted and nothing was logged" into an explicit clipboard-only state the user
// can act on. A successful probe is a hint, not a guarantee: the focused client
// may still refuse the synthetic chord.
//
// The suspend flag: while the settings window captures a hotkey, a synthetic
// Ctrl+V would be captured as input instead of pasting, so injection is
// suppressed and the transcript stays on the clipboard.
//
// Thread affinity: paste() synthesizes input for whatever window has focus and
// is called from the UI thread. The suspend flag is atomic and may be set from
// any thread.
//
// Ownership: stateless apart from the atomic suspend flag; no text is held.

#include "domain/error.hpp"
#include "platform/api/paste.hpp"

#include <atomic>
#include <string>
#include <string_view>

namespace voicetyper::platform::linuxos {

using domain::ErrorCode;
using domain::Status;

/// Environment variable that points the backend at a specific ydotool binary.
/// The default is a PATH lookup of "ydotool".
inline constexpr std::string_view kYdotoolExecutableVariable = "VOICETYPER_YDOTOOL";

/// The socket the ydotool daemon listens on, relative to XDG_RUNTIME_DIR.
inline constexpr std::string_view kYdotoolSocketName = ".ydotool_socket";

class LinuxPasteSimulator final : public platform::PasteSimulator {
public:
    LinuxPasteSimulator() = default;
    ~LinuxPasteSimulator() override;

    LinuxPasteSimulator(const LinuxPasteSimulator&) = delete;
    LinuxPasteSimulator& operator=(const LinuxPasteSimulator&) = delete;
    LinuxPasteSimulator(LinuxPasteSimulator&&) = delete;
    LinuxPasteSimulator& operator=(LinuxPasteSimulator&&) = delete;

    /// Sends Ctrl+V once through ydotool.
    ///
    /// Failure: unavailable (no ydotool, no daemon socket, no /dev/uinput
    /// access, injection suspended for hotkey capture, or the helper reported a
    /// failure), permission_denied (the helper could not open the input device).
    /// The clipboard is never touched by this call, so the transcript survives
    /// either way.
    Status paste() override;

    /// True when this session has everything the injection needs. A false value
    /// is a diagnostic hint only: the focused client can still refuse.
    [[nodiscard]] bool is_injection_supported() const noexcept override;

    /// Suppresses injection while the settings window captures a hotkey.
    void set_suspended(bool suspended) noexcept;
    [[nodiscard]] bool paste_suspended() const noexcept;

    /// The reason the last probe or injection failed, for the log and the UI.
    [[nodiscard]] std::string diagnostics() const;

    /// The ydotool command line this backend would run, so the log can show what
    /// was actually attempted.
    [[nodiscard]] std::string command_description() const;

private:
    std::atomic_bool suspended_{false};
    mutable std::string diagnostics_;
};

} // namespace voicetyper::platform::linuxos
