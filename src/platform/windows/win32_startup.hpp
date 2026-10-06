#pragma once

// Windows autostart: the HKCU Run-key backend for «Запускать вместе с Windows».
//
// Evidence and contract:
//   * VoiceTyper.App/Services/StartupManager.cs - the .NET reference, 53 lines:
//     `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value `VoiceTyper`,
//     data `"<Environment.ProcessPath>"`, delete on disable, and *every*
//     exception swallowed.
//   * VoiceTyper.App/ViewModels/SettingsViewModel.cs:1262 - the only call site:
//     `StartupManager.SetRunAtStartup(settings.StartWithWindows)` on save, with
//     the data always the plain quoted exe, never a switch.
//   * docs/migration/cpp/compatibility-contracts.md §2 "Windows paths and
//     lifecycle" (the Autostart row plus "Autostart failures are swallowed and
//     the Run key is not created when absent") and
//     docs/migration/cpp/parity-ledger.md row LIFE-01.
//
// Intent-parity decisions (differences from the current .NET build, on purpose):
//   * start_minimized is part of the stored command line: `"<exe>"
//     --start-minimized` when the setting is on, `"<exe>"` otherwise. The .NET
//     build never wrote the switch, so `startMinimized` only worked when the
//     user started the app by hand - the setting was persisted and bound with
//     no consumer (compatibility-contracts.md §1 marks it as exactly that).
//     Writing the switch makes the two settings mean what they say, and
//     `src/app/windows_application.cpp` honours the same switch on launch.
//   * Failures are *reported*, not swallowed. The .NET `catch {}` turns a
//     refused write into "the checkbox stayed ticked and nothing happened",
//     which is indistinguishable from success; here a refused write is
//     `permission_denied`/`io_failure` with the LSTATUS in the detail so the
//     composition can log it and the user is not lied to.
//   * A missing Run *key* is created on write. The .NET path opened the key
//     read/write and silently returned when it was absent, so on a profile
//     whose Run key was removed the checkbox was a no-op. Absence of the *key*
//     is not a reason to refuse a write the user explicitly asked for.
//   * The command line is quoted with the CommandLineToArgvW rules rather than
//     by wrapping in two literal quote characters, so a path that ends in a
//     backslash (or contains one) still parses back to the same path.
//   * `is_enabled()` keeps the .NET spelling - existence of the value, false on
//     any failure - and `read()` is the diagnostic read: the UI can show the
//     registry instead of a stored setting that may disagree with it.
//     compatibility-contracts.md §2 records that the .NET
//     `IsRunAtStartupEnabled()` "is not used by the current UI"; that is the
//     gap this backend exists to close.
//
// Scope, deliberately NOT here (see compatibility-contracts.md §2):
//   * HKCU only. There is no per-user/per-machine choice and no HKLM write, so
//     no elevation prompt is ever needed and none is ever shown.
//   * No Task Scheduler dependency: this is the Run key, not a scheduled task,
//     so there is no trigger delay, no run-level choice and no
//     `schtasks`/`ITaskService` surface to keep working.
//   * No StartupApproved/Startup folder support and no per-app Windows 10/11
//     "Startup apps" enable/disable round-trip: this backend writes the same
//     value the .NET build wrote, and nothing reads the StartupApproved state
//     Windows shows in Task Manager.
//   * No per-instance mutex or update/installer interplay.
//
// Platform boundary: this header is standard-C++20 and includes no Windows
// header, so the quoting rules, the command-line builder and the switch parser
// are testable on any host. All Reg*/GetModuleFileNameW calls live in
// win32_startup.cpp.
//
// Threading: Win32Startup is an immutable value; every method is const and
// holds no state between calls, so one instance can be shared. Registry calls
// are per-call opens, so two threads writing the same value can interleave -
// the same last-writer-wins the .NET code had, and no worse.

#include "domain/error.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace voicetyper::platform::win32 {

using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// The Run key, spelled exactly as StartupManager.cs:9 spells it. Kept as a
/// `wstring_view` so the production path and the contract test can compare it
/// without a locale-dependent narrow conversion.
inline constexpr std::wstring_view kAutostartKeyPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

/// The value name, StartupManager.cs:10. The Run key is shared by every
/// installed program, so this name is the only thing that keeps VoiceTyper's
/// entry separate from OneDrive/Teams and from any other port of this app.
inline constexpr std::wstring_view kAutostartValueName = L"VoiceTyper";

/// The switch that starts the app with its window hidden. Written into the Run
/// value when `startMinimized` is on and honoured on launch by
/// `src/app/windows_application.cpp`.
inline constexpr std::wstring_view kStartMinimizedSwitch = L"--start-minimized";

/// The state of the autostart value as the registry actually holds it.
///
/// `present == false` is a normal state, never a failure: a first run, a user
/// who unticked the box, or a profile without the key all look exactly like
/// this, and the .NET `IsRunAtStartupEnabled()` reported the same thing.
struct StartupEntry {
    /// The value exists in the key.
    bool present = false;
    /// The stored command line carries `kStartMinimizedSwitch`. Only meaningful
    /// when `present`.
    bool start_minimized = false;
    /// The raw REG_SZ/REG_EXPAND_SZ data, empty when `!present`. Not expanded:
    /// the value is reported as Windows stores it.
    std::wstring command_line;
    /// The registry type as text, e.g. L"REG_SZ", empty when `!present`. A
    /// non-string type means something else owns the value, which is exactly
    /// the fact a UI needs before it claims "autostart is on".
    std::wstring value_type;
};

/// Where this instance reads and writes. The defaults are the production
/// constants; a contract test overrides only the *value name* so it can write
/// into the real Run key without touching the installed app's entry, and a
/// caller may pin the executable path so the stored command line is
/// reproducible.
struct StartupOptions {
    /// Subpath below HKEY_CURRENT_USER. Defaults to `kAutostartKeyPath`.
    std::wstring key_path{kAutostartKeyPath};
    /// Value name inside that key. Defaults to `kAutostartValueName`.
    std::wstring value_name{kAutostartValueName};
    /// Executable to register. `std::nullopt` (the production spelling) means
    /// "ask GetModuleFileNameW for this process".
    std::optional<std::wstring> executable_path;
};

/// The HKCU Run-key backend.
///
/// Lifetime: it owns nothing. Construct it once per composition root and call
/// it from anywhere; there is no handle to close and no failed-construction
/// state to leak (a missing key surfaces as `present == false`, not as a
/// null object).
class Win32Startup final {
public:
    /// Production instance: the real key path, the real value name and this
    /// process's own executable path.
    Win32Startup();

    /// Explicit instance, for a pinned executable path and/or a different
    /// value name. An empty `key_path` or `value_name` is `invalid_argument`
    /// rather than a silent fallback to the production constants: a test that
    /// meant to be isolated must not be able to write the real value by
    /// accident.
    explicit Win32Startup(StartupOptions options);

    Win32Startup(const Win32Startup&) = delete;
    Win32Startup& operator=(const Win32Startup&) = delete;
    Win32Startup(Win32Startup&&) = delete;
    Win32Startup& operator=(Win32Startup&&) = delete;
    ~Win32Startup() = default;

    /// The key path this instance uses, for logs and contract assertions.
    [[nodiscard]] const std::wstring& key_path() const noexcept { return options_.key_path; }

    /// The value name this instance uses, for logs and contract assertions.
    [[nodiscard]] const std::wstring& value_name() const noexcept { return options_.value_name; }

    /// The registry truth, so the UI can show reality instead of a stored
    /// setting that may disagree with the Run key.
    ///
    /// A missing value - or a missing key - is a *success* with
    /// `present == false`; only a genuinely unreadable registry is an error
    /// (`permission_denied`, `io_failure`), and then nothing is invented.
    [[nodiscard]] Result<StartupEntry> read() const;

    /// Existence of the value, as `StartupManager.IsRunAtStartupEnabled()`
    /// answered it. A read failure is `false` for the same reason the .NET
    /// method had one answer for every exception; call `read()` when the reason
    /// matters.
    [[nodiscard]] bool is_enabled() const;

    /// Registers this process's executable when `enabled`, removes the value
    /// when not. The stored command line is `"<exe>"` plus
    /// ` --start-minimized` when `start_minimized` is set.
    ///
    /// Idempotent in both directions: enabling twice stores the same bytes,
    /// and disabling a value that is not there succeeds. `start_minimized` is
    /// only consulted when enabling, so a caller that wants to change just the
    /// switch passes `enabled = true` again.
    [[nodiscard]] Status set_enabled(bool enabled, bool start_minimized) const;

    /// Removes the autostart value. Named `remove_entry`, not `remove`, because
    /// `<cstdio>` declares a global `remove(const char*)` and a member with the
    /// bare name is a needless trap in a Windows translation unit.
    /// Idempotent: an absent value is the state the caller asked for.
    [[nodiscard]] Status remove_entry() const;

    /// The executable this instance would register: the pinned
    /// `StartupOptions::executable_path`, or this process's own image.
    /// `GetModuleFileNameW` failure is reported instead of registering an empty
    /// path.
    [[nodiscard]] Result<std::wstring> executable_path() const;

private:
    StartupOptions options_;
};

/// Quotes one argument so CommandLineToArgvW parses it back unchanged: wraps
/// in double quotes, escapes an embedded `"` and doubles the backslashes that
/// would otherwise escape the closing quote. A path with no quote and no
/// trailing backslash - every real executable path - comes back as
/// `"<path>"`, which is byte-identical to the .NET `$"\"{exePath}\""`.
[[nodiscard]] std::wstring quote_argument(std::wstring_view argument);

/// The exact data stored in the Run value: `quote_argument(executable)` plus
/// ` L" --start-minimized"` when the switch is requested. An empty executable
/// yields an empty string, because an entry pointing at nothing is worse than
/// no entry and the caller can see the empty result.
[[nodiscard]] std::wstring build_command_line(std::wstring_view executable, bool start_minimized);

/// Whether a stored command line requests the minimized start. A plain
/// substring test of the switch, which is what the writer produces; it does not
/// attempt to parse the whole command line.
[[nodiscard]] bool carries_start_minimized(std::wstring_view command_line);

/// Whether one process argument is the switch. At least one leading dash is
/// required (both `-start-minimized` and `--start-minimized` are accepted, the
/// way Windows itself treats switch names) and the comparison is
/// case-insensitive. A bare word with the same letters is a file name, not a
/// switch.
[[nodiscard]] bool is_start_minimized_switch(std::string_view argument);

/// The executable named by a command line that `build_command_line` produced:
/// the first argument, with its quotes removed and the backslash-before-quote
/// escape resolved. Empty when nothing usable is parsed, so a caller can tell
/// "no entry" from "an entry naming something else".
[[nodiscard]] std::wstring command_line_executable(std::wstring_view command_line);

/// Whether a *launch-time* reconciliation may rewrite the entry that is already
/// registered.
///
/// The .NET build stays the reference release until the cutover, so merely
/// running a C++ build must not repoint the user's autostart at it - which is
/// exactly what happened when a development build was launched once: the Run
/// value was rewritten to the build tree, and Windows would have started that
/// tree at the next logon. An entry naming a different executable that still
/// exists belongs to another installation and is left alone; an absent,
/// unparsable or dead-target entry is repaired, which is the cutover case.
///
/// `target_exists` is supplied by the caller (even a pure function should not
/// guess about the filesystem), so the decision is testable without a registry.
[[nodiscard]] bool launch_may_rewrite(std::wstring_view stored_command_line,
    std::wstring_view this_executable, bool target_exists);

/// Whether this process was launched with the switch anywhere in `argv`.
/// `argc`/`argv` are read before QApplication rewrites them.
[[nodiscard]] bool wants_start_minimized(int argc, char** argv);

/// This process's own image path from GetModuleFileNameW(nullptr, ...). The
/// buffer grows up to a bounded maximum, so a pathological path cannot spin
/// forever; a truncated result is retried rather than returned truncated.
[[nodiscard]] Result<std::wstring> module_file_path();

/// The narrow spelling of a wide string, for log lines. Best effort: a value
/// that is not convertible is reported as-is, never dropped.
[[nodiscard]] std::string to_log_text(std::wstring_view text);

} // namespace voicetyper::platform::win32
