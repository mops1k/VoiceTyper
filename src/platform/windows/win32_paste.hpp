#pragma once

// Win32 implementation of the frozen platform::PasteSimulator contract.
//
// Evidence and contract:
//   * src/platform/api/paste.hpp - one synthetic Ctrl+V, no clipboard access,
//     permission_denied for the UAC integrity mismatch, unavailable for a
//     missing injection backend or a refusing window, and kPasteChord
//     ("Ctrl+V") as the only chord this product ever sends.
//   * docs/migration/cpp/compatibility-contracts.md §3 - "UAC integrity mismatch
//     can prevent SendInput paste; clipboard must remain authoritative" and the
//     Linux branch's explicit clipboard-only state.
//   * src/domain/text_output.* - a refused paste is a *success* with
//     TextOutputOutcome::clipboard_only, so every failure code here must be one
//     the caller can act on rather than a bare exception.
//
// Why a pre-flight integrity probe instead of trusting SendInput's return value:
// SendInput is subject to UIPI. When the focused window runs at a higher
// integrity level the call inserts nothing and the only evidence is a zero
// return with no Win32 error, which is indistinguishable from "this session has
// no input desktop". Probing the focused window's token integrity level first
// (GetWindowThreadProcessId -> OpenProcess -> OpenProcessToken ->
// GetTokenInformation(TokenIntegrityLevel)) turns the UAC case into a
// permission_denied the UI can explain, and keeps plain unavailable for the
// genuinely unavailable cases. The probe is read-only: no process is written to
// and no token is duplicated beyond the query handle.
//
// The suspend flag: while the settings window captures a hotkey, Ctrl+V would be
// captured as input instead of pasting, so injection is suppressed and the
// transcript stays on the clipboard. Suspension is reported as unavailable with
// an explicit message, which the caller renders as clipboard_only - the exact
// state the contract asks for during hotkey capture.
//
// Platform boundary: this header is standard-C++20 and includes no Windows
// header. The token, desktop and SendInput calls live in win32_paste.cpp.
//
// Thread affinity: paste() synthesizes input for whatever window has focus, so
// it belongs on the thread that owns input handling - the UI thread. The class
// itself holds no state that makes this mandatory, and the suspend flag is
// atomic so the settings window can set it from any thread.
//
// Ownership: stateless apart from the atomic suspend flag; no text is held.

#include "domain/error.hpp"
#include "platform/api/paste.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace voicetyper::platform {

using domain::ErrorCode;
using domain::Status;

/// Extra-info tag stamped on every injected event. A low-level keyboard hook
/// (the hotkey backend owns one) can recognise VoiceTyper's own synthetic
/// Ctrl+V and ignore it instead of treating the paste as a user chord.
inline constexpr std::uintptr_t kWin32PasteInputTag = 0x5648; // 'V''H'

/// What the read-only integrity probe learned about the focused window.
struct FocusedWindowProbe {
    /// False when there is no foreground window, or its integrity level could
    /// not be read. `target_above_own` is then false and the caller must not
    /// treat the probe as a clearance.
    bool resolved = false;
    /// True when the focused window belongs to this process, so no cross-process
    /// integrity question exists.
    bool same_process = false;
    /// Mandatory integrity RID of the focused window's process.
    std::uint32_t target_level = 0;
    /// Mandatory integrity RID of this process.
    std::uint32_t own_level = 0;
    /// The last Win32 error of the probe, for the diagnostic.
    unsigned long last_error = 0;
    /// True only when the probe resolved and the target sits strictly above this
    /// process: the UAC case, which is permission_denied.
    bool target_above_own = false;
    /// Window handle the probe looked at, 0 when there was none.
    std::uintptr_t window = 0;
};

class Win32PasteSimulator final : public PasteSimulator {
public:
    Win32PasteSimulator() = default;
    ~Win32PasteSimulator() override;

    Win32PasteSimulator(const Win32PasteSimulator&) = delete;
    Win32PasteSimulator& operator=(const Win32PasteSimulator&) = delete;
    Win32PasteSimulator(Win32PasteSimulator&&) = delete;
    Win32PasteSimulator& operator=(Win32PasteSimulator&&) = delete;

    /// Sends Ctrl+V once through SendInput.
    ///
    /// Failure: permission_denied (the focused window runs at a higher integrity
    /// level, or UIPI refused the injection), unavailable (no input desktop in
    /// this session, no foreground window, injection suspended for hotkey
    /// capture, or SendInput inserted nothing). The clipboard is never touched
    /// by this call, so the transcript survives either way.
    Status paste() override;

    /// True when this session has an interactive input desktop. A false value is
    /// a diagnostic hint only: UIPI and window policies can still refuse.
    [[nodiscard]] bool is_injection_supported() const noexcept override;

    /// Suppresses injection while the settings window captures a hotkey. The
    /// text still reaches the clipboard, so this reports clipboard_only, never a
    /// lost transcript.
    void set_suspended(bool suspended) noexcept;
    [[nodiscard]] bool paste_suspended() const noexcept;

    /// The read-only integrity probe the paste path uses. Exposed so a contract
    /// test can assert the classification without needing an elevated window.
    [[nodiscard]] FocusedWindowProbe probe_focused_window() const;

    /// The integrity RID of this process, 0 when it could not be read.
    [[nodiscard]] static std::uint32_t own_integrity_level() noexcept;

private:
    std::atomic_bool suspended_{false};
};

} // namespace voicetyper::platform
