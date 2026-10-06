#pragma once

// Windows global hotkeys: RegisterHotKey plus the WH_KEYBOARD_LL capture hook.
//
// Evidence and contract:
//   * src/platform/api/hotkeys.hpp - the frozen platform contract implemented
//     here by `Win32HotkeyService`.
//   * VoiceTyper.App/Services/HotkeyService.cs:14-210,363-398 - the .NET
//     service: RegisterHotKey on a dedicated message-pump thread, per-hotkey
//     error strings, the VK <-> name table and the 30 ms push-to-talk release
//     poll.
//   * VoiceTyper.App/Services/HotkeyCaptureHook.cs:17-292 - the settings
//     "press a key" flow: a global WH_KEYBOARD_LL hook on a background thread
//     with its own pump, Win suppressed, Escape cancels, a modifier is
//     required unless the key is F1..F24.
//   * docs/migration/cpp/compatibility-contracts.md section 6 "Hotkeys" and
//     docs/migration/cpp/feature-parity.md rows "Global hotkeys" / "Hotkey
//     capture" (KEY-01 in docs/migration/cpp/parity-ledger.md).
//
// Defects of the .NET reference this backend deliberately does not reproduce
// (audit m_00379ac352a2):
//   * HotkeyService.RunOnPump (HotkeyService.cs:134-162) ignores the result of
//     its 3 s wait, so a stalled pump thread looks like "registration failed".
//     Here every pump round trip is bounded *and reports* the timeout as
//     ErrorCode::timeout; a slow pump can never be mistaken for a conflict.
//   * HotkeyService.Dispose (HotkeyService.cs:84-89) never joins its thread,
//     so hotkeys can stay registered after the service is gone. Here
//     unregister_all() and the destructor both drive the pump and *wait* for
//     the unregister to complete, and the destructor additionally joins the
//     thread with a bounded deadline.
//   * HotkeyCaptureHook.Stop (HotkeyCaptureHook.cs:218-234) posts WM_QUIT only
//     when the thread already recorded a thread id, so a thread that has not
//     reached its pump is never stopped and the awaiting UI hangs forever.
//     Here the thread id is published before the hook is installed, the
//     message queue is created explicitly, WM_QUIT is posted unconditionally
//     and the pump also wakes on a stop event every 25 ms - so a lost WM_QUIT
//     cannot strand the thread.
//   * The stale push-to-talk continuation (App.axaml.cs:544-556 calling the
//     static WaitForKeyRelease with no token) is not reproduced: the release
//     edge is produced by the service's own release poll, is bound to the
//     currently registered record hotkey, and stops with the service.
//
// Intended-parity decisions (differences from the current .NET build, on
// purpose - the ledger is scored against the declared feature, not against the
// C# no-ops):
//   * Registration always passes MOD_NOREPEAT, so holding a key produces one
//     press edge instead of a stream of them. The release edge that
//     push-to-talk needs is then produced by the in-service 30 ms release
//     poll instead of the static, cancellation-less WaitForKeyRelease.
//   * A refused registration is reported with a *reason*, not just a string:
//     already-registered by another application, blocked by a hotkey policy,
//     a combination Windows refuses, or an unclassified OS failure. The
//     process integrity level is included in the message, because the most
//     common real-world cause of "already registered" is another application -
//     often one running elevated - holding the combination at a different
//     integrity level, and the OS does not name the owner.
//   * A combination that a documented policy forbids is reported *before* any
//     RegisterHotKey call, so the user gets the real cause instead of a
//     misleading conflict.
//
// Platform boundary: the header is standard-C++20 and free of <windows.h>, so
// the mapping, the reason taxonomy and the diagnostics rendering compile and
// are testable on any host. All Win32 calls live in win32_hotkeys.cpp, and the
// OS state is reached through a pimpl.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/hotkey_gesture.hpp"
#include "domain/settings.hpp"
#include "platform/api/hotkeys.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::platform::win32 {

using domain::AppSettings;
using domain::CancellationToken;
using domain::Error;
using domain::ErrorCode;
using domain::HotkeyGesture;
using domain::Result;
using domain::Status;

/// Win32 modifier bits (RegisterHotKey, not the domain HotkeyModifiers values).
inline constexpr std::uint32_t kModAlt = 0x0001;
inline constexpr std::uint32_t kModControl = 0x0002;
inline constexpr std::uint32_t kModShift = 0x0004;
inline constexpr std::uint32_t kModWin = 0x0008;
/// Windows 7+. Without it a held key produces a stream of WM_HOTKEY messages;
/// with it, exactly one. The push-to-talk release edge is the service's job.
inline constexpr std::uint32_t kModNoRepeat = 0x4000;

/// Hotkey ids. RegisterHotKey with a null window binds a registration to the
/// *calling thread*, so these are per-process-per-thread and only need to be
/// stable. They match the .NET constants for log readability.
inline constexpr int kRecordHotkeyId = 1;
inline constexpr int kCancelHotkeyId = 2;

/// Release-edge poll cadence, milliseconds. The .NET WaitForKeyRelease polls
/// GetAsyncKeyState every 30 ms (HotkeyService.cs:25,367-372); the same cadence
/// is used here so push-to-talk feels identical.
inline constexpr std::uint32_t kReleasePollIntervalMs = 30;

/// Default bound for one pump round trip. The .NET code also waited 3 s and
/// then threw the answer away; here the same bound is *reported*.
inline constexpr std::uint32_t kDefaultPumpActionTimeoutMs = 3000;

/// Default bound for the shutdown join. Generous enough for one 25 ms pump
/// tick, short enough that a wedged thread cannot hang the UI thread.
inline constexpr std::uint32_t kDefaultShutdownTimeoutMs = 2000;

// ---------------------------------------------------------------------------
// Key table: a direct port of HotkeyService.ToVirtualKey / VirtualKeyToName
// (HotkeyService.cs:212-335), because the settings.json spelling of a key must
// round-trip unchanged across the migration.
// ---------------------------------------------------------------------------

/// Maps a `System.Windows.Input.Key` style name to a Win32 virtual-key code.
/// Returns 0 for an unknown name. Single A-Z and 0-9 characters map to their
/// ASCII code, exactly as in the .NET reference.
[[nodiscard]] std::int32_t hotkey_virtual_key(std::string_view key_name) noexcept;

/// Inverse mapping. Returns nullopt for a code the product does not bind, so a
/// physical key outside the table (media keys, IME keys) simply cannot be
/// captured instead of producing a nonsense gesture.
[[nodiscard]] std::optional<std::string> hotkey_key_name(std::int32_t virtual_key) noexcept;

/// Domain modifier set -> Win32 MOD_* bits. MOD_NOREPEAT is *not* included:
/// the caller decides the repeat policy (the service always wants
/// kModNoRepeat, the capture hook does not register at all).
[[nodiscard]] std::uint32_t hotkey_native_modifiers(domain::HotkeyModifiers modifiers) noexcept;

// ---------------------------------------------------------------------------
// Failure diagnostics
// ---------------------------------------------------------------------------

/// Why a registration was refused, in the order the evidence is checked.
enum class HotkeyFailureReason : std::uint8_t {
    /// Registration succeeded.
    none = 0,
    /// The gesture does not parse, has no key, or names a key that has no
    /// Win32 virtual-key code in the product table.
    invalid_gesture,
    /// A documented policy value forbids this combination (see
    /// HotkeyPolicyFindings). Reported *before* RegisterHotKey is attempted.
    policy_disabled,
    /// Win32 ERROR_HOTKEY_ALREADY_REGISTERED: some application already owns
    /// the combination. The OS never names the owner, which is why the
    /// integrity level is reported alongside.
    already_registered,
    /// Win32 ERROR_INVALID_PARAMETER: Windows refuses this combination as
    /// specified (a bare key that is not F1..F12, a non-registerable code).
    invalid_parameter,
    /// Any other failure; the raw Win32 code is carried verbatim.
    os_failure,
    /// The pump thread did not answer within its bound. Never reported as a
    /// conflict: this is the .NET RunOnPump bug fixed.
    pump_timeout,
};

[[nodiscard]] constexpr std::string_view hotkey_failure_reason_name(HotkeyFailureReason reason) noexcept
{
    switch (reason) {
    case HotkeyFailureReason::none: return "none";
    case HotkeyFailureReason::invalid_gesture: return "invalid_gesture";
    case HotkeyFailureReason::policy_disabled: return "policy_disabled";
    case HotkeyFailureReason::already_registered: return "already_registered";
    case HotkeyFailureReason::invalid_parameter: return "invalid_parameter";
    case HotkeyFailureReason::os_failure: return "os_failure";
    case HotkeyFailureReason::pump_timeout: return "pump_timeout";
    }
    return "unknown";
}

/// One registry value that can disable global hotkeys, named exactly as it is
/// read, so a diagnostic can be audited against the machine it came from.
struct HotkeyPolicyFinding {
    /// Full registry path, e.g. HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer\NoWinKeys.
    std::string key;
    /// Dumped value, e.g. "1" or "VoiceTyper.exe".
    std::string value;
    /// What that value means for this product.
    std::string effect;
};

/// The policy state of this machine, read without side effects.
///
/// Exactly two documented values are probed, and nothing else is guessed:
///   * HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer\NoWinKeys
///     (REG_DWORD) - the "Windows key hotkeys disabled" policy. It forbids
///     every combination that uses the Win modifier.
///   * HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\DisabledHotkeys
///     (REG_SZ) - Explorer's application-scoped list of programs whose hotkeys
///     are disabled. Only a list naming *this* executable is treated as a
///     finding, because the list is per application.
[[nodiscard]] std::vector<HotkeyPolicyFinding> hotkey_policy_findings();

/// The integrity label of the current process: "medium", "high", "system",
/// "untrusted" or "unknown". Reported with a registration conflict because a
/// hotkey held by an elevated application is invisible to a medium-integrity
/// process, and the OS does not name the owner.
[[nodiscard]] std::string hotkey_process_integrity_label();

/// Everything a UI needs to explain a refused registration.
struct HotkeyDiagnostics {
    HotkeyFailureReason reason = HotkeyFailureReason::none;
    /// GetLastError() immediately after the refused call, 0 when the refusal
    /// was decided before the call.
    std::uint32_t win32_error = 0;
    /// hotkey_process_integrity_label() at the time of the failure.
    std::string integrity_label = "unknown";
    /// Policy values that were read, whether or not they caused the failure.
    std::vector<HotkeyPolicyFinding> policy_findings;
    /// The combination in settings.json spelling, for the message.
    std::string gesture;

    /// One line for the settings UI, built only from the facts above.
    [[nodiscard]] std::string describe() const;
};

/// Builds a diagnostic. `reason` and `win32_error` come from the actual call;
/// everything else is read here. Cheap (two registry values plus one token
/// query), so it runs on failure only.
[[nodiscard]] HotkeyDiagnostics hotkey_diagnostics(const HotkeyGesture& gesture,
                                                   HotkeyFailureReason reason,
                                                   std::uint32_t win32_error = 0);

/// True when the machine's policy forbids this gesture. When it returns true
/// the caller must not call RegisterHotKey: the combination would be refused
/// for a reason the UI can explain, and reporting "already registered" would be
/// a lie.
[[nodiscard]] bool hotkey_blocked_by_policy(const HotkeyGesture& gesture,
                                            const std::vector<HotkeyPolicyFinding>& findings = hotkey_policy_findings());

// ---------------------------------------------------------------------------
// Global hotkey service
// ---------------------------------------------------------------------------

/// The frozen `platform::HotkeyService` contract on Win32.
///
/// Threading: the sink is called on an owned pump thread, never on the caller.
/// apply_settings/unregister_all are thread-safe and bounded; the destructor
/// unregisters and joins, so no hotkey outlives the object and no sink callback
/// can run after unregister_all() returns.
class Win32HotkeyService final : public HotkeyService {
public:
    Win32HotkeyService();
    ~Win32HotkeyService() override;

    Win32HotkeyService(const Win32HotkeyService&) = delete;
    Win32HotkeyService& operator=(const Win32HotkeyService&) = delete;
    Win32HotkeyService(Win32HotkeyService&&) = delete;
    Win32HotkeyService& operator=(Win32HotkeyService&&) = delete;

    Status set_event_sink(HotkeyEventSink sink) override;
    Result<HotkeyRegistrationReport> apply_settings(const AppSettings& settings) override;
    Status unregister_all() override;
    [[nodiscard]] std::int32_t record_key_code() const noexcept override;

    /// The most recent apply_settings() report, or a default-constructed one.
    /// Exposed so the settings UI and the contract test can show *why* a
    /// combination was refused without re-running the registration.
    [[nodiscard]] HotkeyRegistrationReport last_report() const;

    /// Diagnostics of the most recent refusal, per action, so the UI can render
    /// the reason, the Win32 code, the integrity level and the policy values.
    [[nodiscard]] HotkeyDiagnostics last_diagnostics(HotkeyAction action) const;

    /// Bound of one pump round trip. A request that exceeds it fails with
    /// ErrorCode::timeout - it is never reported as a registration conflict.
    void set_pump_action_timeout(std::chrono::milliseconds timeout) noexcept;

    /// Bound of the shutdown join. Exceeding it is reported as timeout; the
    /// thread is left to finish on its own rather than being killed, because
    /// a registered hotkey belonging to a dying thread is unrecoverable while a
    /// leaked thread is not.
    void set_shutdown_timeout(std::chrono::milliseconds timeout) noexcept;

    /// True while the owned pump thread is alive. Diagnostics only.
    [[nodiscard]] bool pump_running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Settings capture: the "press a key to bind" flow
// ---------------------------------------------------------------------------

/// Global WH_KEYBOARD_LL capture of one gesture.
///
/// Why a low-level hook and not a window key handler: with a window handler,
/// pressing Win moves focus away (the Start menu opens) and the rest of the
/// combination never arrives - the reason the .NET service moved to a hook in
/// v1.1.1. Behaviour kept from HotkeyCaptureHook.cs:
///   * Win and Win+... are suppressed (the hook returns 1) so Start never
///     opens;
///   * Escape with no modifier cancels;
///   * a modifier is required unless the key is F1..F24, otherwise
///     `on_modifier_required` fires and the keystroke is passed through;
///   * the hook is one-shot: the first accepted combination or an Escape ends
///     the capture.
/// Ownership: the hook lives on an owned thread with an owned message pump.
/// stop() is idempotent, bounded and callable from any thread; after it
/// returns the hook is uninstalled and the thread is joined.
class HotkeyCaptureHook {
public:
    /// Fired on the hook thread when a key without a modifier was pressed and
    /// only F-keys are allowed. Must be cheap; the UI marshals it itself.
    using ModifierRequiredCallback = std::function<void()>;

    explicit HotkeyCaptureHook(ModifierRequiredCallback on_modifier_required = {});
    ~HotkeyCaptureHook();

    HotkeyCaptureHook(const HotkeyCaptureHook&) = delete;
    HotkeyCaptureHook& operator=(const HotkeyCaptureHook&) = delete;
    HotkeyCaptureHook(HotkeyCaptureHook&&) = delete;
    HotkeyCaptureHook& operator=(HotkeyCaptureHook&&) = delete;

    /// Starts the hook thread. Fails with ErrorCode::unavailable when the
    /// WH_KEYBOARD_LL hook cannot be installed, so the UI can say *why* the
    /// settings flow is unavailable instead of hanging on a promise. Calling
    /// start() twice stops the previous capture first.
    Status start();

    /// True between a successful start() and stop().
    [[nodiscard]] bool is_running() const noexcept;

    /// True when SetWindowsHookEx succeeded on the owned thread. False after
    /// the capture ended or when the hook was refused.
    [[nodiscard]] bool hook_installed() const noexcept;

    /// The owned thread's id, 0 when no capture is running. Diagnostics only.
    [[nodiscard]] std::uint32_t thread_id() const noexcept;

    /// Waits for the next captured combination. Blocks a worker thread (no UI
    /// thread is involved) and returns ErrorCode::cancelled when `cancellation`
    /// fires, when stop() is called, or when the hook thread ends. Must not be
    /// called from the hook thread.
    [[nodiscard]] Result<HotkeyGesture> capture_next(const CancellationToken& cancellation);

    /// Uninstalls the hook, wakes the pump and joins the thread within
    /// `shutdown_timeout()`. Idempotent and safe from any thread; never hangs.
    Status stop();

    /// Bound of the stop() join. Defaults to kDefaultShutdownTimeoutMs.
    void set_shutdown_timeout(std::chrono::milliseconds timeout) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voicetyper::platform::win32
