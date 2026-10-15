#pragma once

// Linux autostart: the freedesktop desktop-entry backend for «Запускать вместе
// с системой».
//
// Evidence and contract:
//   * src/platform/windows/win32_startup.hpp - the Windows backend of the same
//     setting; the observable behaviour (the stored command line, the
//     start-minimized switch, idempotence, reported failures, the
//     launch-time reconciliation rule) is reproduced here on purpose, so the
//     two platforms cannot drift.
//   * src/platform/api/lifecycle.hpp - "Failure codes: unsupported (no
//     autostart mechanism on this platform), permission_denied, io_failure.
//     Callers must log the failure and continue; autostart is never a reason to
//     refuse to start."
//
// Where the entry lives: `$XDG_CONFIG_HOME/autostart/<name>.desktop`, with
// `$HOME/.config/autostart` as the XDG default. That is the location every
// desktop environment (KDE, GNOME, Xfce) reads at session start; no systemd
// unit and no distribution-specific path is involved, so no root rights are
// needed and nothing outside the user's own configuration directory is touched.
//
// Intent-parity decisions (differences from the Windows backend, on purpose):
//   * `start_minimized` is part of the stored command line, exactly as on
//     Windows: `<executable> --start-minimized` when the setting is on, and the
//     plain executable otherwise. The composition root honours the same switch
//     on launch.
//   * Failures are reported, never swallowed: a refused write is
//     permission_denied/io_failure with the reason, so the composition can log
//     it instead of showing a checkbox that lies.
//   * The entry is written atomically (a temporary file plus rename), so a
//     crash in the middle cannot leave a truncated desktop entry that the
//     session would then try to execute.
//   * Two entry names, one per instance kind: `voicetyper.desktop` for an
//     installed copy and `voicetyper-dev.desktop` for a copy running from a
//     build tree (2026-10-16), mirroring `VoiceTyper`/`VoiceTyper-dev` on
//     Windows; see kDevelopmentAutostartFileName.
//   * Removing an entry that names a different executable is refused unless the
//     caller says the user asked for it (`override_owner`), exactly as the
//     Windows backend does: the two platforms must not drift on ownership.
//
// Scope, deliberately NOT here: no systemd user unit, no `~/.profile` edit, no
// per-machine installation and no desktop-environment-specific "Startup
// Applications" database. The desktop entry is the interoperable mechanism and
// the only one this product needs.
//
// Platform boundary: this header is standard-C++20 and includes no Linux
// header, so the quoting rules, the entry builder, the parser and the switch
// parser are testable on any host.

#include "domain/app_paths.hpp"
#include "domain/error.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace voicetyper::platform::linuxos {

using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// File name of the autostart entry. The name is the only thing that keeps
/// VoiceTyper's entry apart from every other application's, so it is a constant
/// rather than a string literal at the call site.
inline constexpr std::string_view kAutostartFileName = "voicetyper.desktop";

/// File name of the autostart entry a *development* copy owns: one running from
/// a build tree rather than from an installed prefix.
///
/// Mirrors `platform::win32::kAutostartDevelopmentValueName` and exists for the
/// same reason (measured 2026-10-16 on Windows): two copies of the product - the
/// installed one the user uses and the build being tested - share one entry
/// name, so a build could take over the installed copy's autostart and the
/// installed copy could not take it back. On this platform the rule is expressed
/// with the file name, which is what a desktop environment lists in
/// "Startup Applications".
inline constexpr std::string_view kDevelopmentAutostartFileName = "voicetyper-dev.desktop";

/// Whether the executable at `executable` belongs to an *installed* copy of the
/// product. This platform has no installer yet, so the rule is the build-tree
/// convention: the product's build trees live under a directory named `build`
/// (`build/<preset>/...`, the layout every CMake preset here produces), while an
/// installed copy lives in a system prefix, an AppImage mount or a user prefix
/// without that component.
///
/// An empty path answers `false`: an executable that cannot be *shown* not to be
/// a build is treated as a development copy, so it can never take over the
/// installed copy's entry.
[[nodiscard]] bool is_installed_instance(const std::filesystem::path& executable);

/// The entry file name an instance running from `executable` owns:
/// kAutostartFileName for an installed copy, kDevelopmentAutostartFileName for
/// one running from a build tree. The production constructor and the contract
/// test both go through this single function, so the rule has one spelling.
[[nodiscard]] std::string autostart_file_name_for(const std::filesystem::path& executable);

/// The switch that starts the application with its window hidden, byte-identical
/// to the Windows one so both platforms accept the same launch arguments.
inline constexpr std::string_view kStartMinimizedSwitch = "--start-minimized";

/// The state of the autostart entry as the filesystem actually holds it.
///
/// `present == false` is a normal state, never a failure: a first run, a user
/// who unticked the box, or a session that removed the entry all look like this.
struct StartupEntry {
    /// The entry file exists.
    bool present = false;
    /// The stored command line carries kStartMinimizedSwitch. Only meaningful
    /// when `present`.
    bool start_minimized = false;
    /// The Exec value as stored, with the desktop-entry quoting resolved.
    std::string command_line;
    /// Full path of the entry file, empty when `!present`.
    std::filesystem::path file_path;
};

/// Where this instance reads and writes. The defaults are the production
/// values; a contract test overrides the directory and the executable path so it
/// never touches the user's real autostart directory.
struct StartupOptions {
    /// Directory holding the `.desktop` file, e.g. `$XDG_CONFIG_HOME/autostart`.
    std::filesystem::path autostart_directory;
    /// Entry file name inside that directory. Defaults to kAutostartFileName;
    /// a development copy passes kDevelopmentAutostartFileName, which the
    /// production constructor decides on its own (see is_installed_instance).
    std::string file_name{kAutostartFileName};
    /// Executable to register. `std::nullopt` (the production spelling) means
    /// "the running binary" from /proc/self/exe.
    std::optional<std::filesystem::path> executable_path;
};

/// `$XDG_CONFIG_HOME/autostart`, with `$HOME/.config/autostart` as the XDG
/// fallback. Injected environment, so the resolution is contract-tested without
/// touching the real session.
[[nodiscard]] std::filesystem::path default_autostart_directory(
    const domain::EnvironmentLookup& environment);

/// The desktop-entry backend. It owns nothing and holds no handle; construct it
/// once per composition root and call it from anywhere.
class LinuxStartup final {
public:
    /// Production instance: the XDG autostart directory, this process's own
    /// executable path, and the file name that belongs to *this instance kind* -
    /// kAutostartFileName for an installed copy, kDevelopmentAutostartFileName
    /// for one running from a build tree. Pass an explicit `StartupOptions` to
    /// pin a name: the contract test does, and so does a reader that wants the
    /// *other* copy's entry.
    LinuxStartup();

    /// Explicit instance, for a pinned directory and executable path. An empty
    /// directory or file name is `invalid_argument` rather than a silent
    /// fallback to the production values: a test that meant to be isolated must
    /// not be able to write the real entry by accident.
    explicit LinuxStartup(StartupOptions options);

    LinuxStartup(const LinuxStartup&) = delete;
    LinuxStartup& operator=(const LinuxStartup&) = delete;
    LinuxStartup(LinuxStartup&&) = delete;
    LinuxStartup& operator=(LinuxStartup&&) = delete;
    ~LinuxStartup() = default;

    /// Full path of the entry this instance reads and writes.
    [[nodiscard]] const std::filesystem::path& file_path() const noexcept { return file_path_; }

    /// The filesystem truth, so the UI can show reality instead of a stored
    /// setting that may disagree with the autostart directory.
    ///
    /// A missing entry is a *success* with `present == false`; only a genuinely
    /// unreadable file is an error (`permission_denied`, `io_failure`).
    [[nodiscard]] Result<StartupEntry> read() const;

    /// Existence of the entry, false on any read failure; call read() when the
    /// reason matters.
    [[nodiscard]] bool is_enabled() const;

    /// Registers the executable when `enabled`, removes the entry when not. The
    /// stored command line is the executable plus ` --start-minimized` when
    /// `start_minimized` is set.
    ///
    /// Idempotent in both directions: enabling twice stores the same bytes, and
    /// disabling an absent entry succeeds.
    ///
    /// `override_owner` is the difference between "the user said so" and "the app
    /// is starting": a write always writes, but *removing* an entry that names a
    /// different executable is refused unless this flag is set, because that
    /// entry belongs to another installation. Only an explicit settings change
    /// passes `true`; a launch-time reconciliation never does. The Windows
    /// backend refuses in exactly the same cases, so the two platforms keep the
    /// same ownership rule.
    [[nodiscard]] Status set_enabled(bool enabled, bool start_minimized, bool override_owner = false) const;

    /// Removes the autostart entry. Idempotent: an absent entry is the state the
    /// caller asked for.
    ///
    /// An entry naming a *different* executable is refused (`permission_denied`)
    /// unless `override_owner` is set - see set_enabled.
    [[nodiscard]] Status remove_entry(bool override_owner = false) const;

    /// The executable this instance would register: the pinned
    /// `StartupOptions::executable_path`, or this process's own image. A failure
    /// to resolve the running binary is reported instead of registering an empty
    /// path.
    [[nodiscard]] Result<std::filesystem::path> executable_path() const;

private:
    StartupOptions options_;
    std::filesystem::path file_path_;
};

/// Quotes one argument for a desktop-entry Exec value: a plain path is stored as
/// it is, and a path containing whitespace, a quote or a backslash is wrapped in
/// double quotes with the freedesktop escapes applied, so the Exec value parses
/// back to the same single argument.
[[nodiscard]] std::string quote_exec_argument(std::string_view argument);

/// The complete desktop-entry file for an executable: the `[Desktop Entry]`
/// group, the application type, the name, the Exec value (with the switch) and
/// the keys that make the entry a user autostart entry rather than a launcher.
/// An empty executable yields an empty string, because an entry pointing at
/// nothing is worse than no entry and the caller can see the empty result.
[[nodiscard]] std::string build_desktop_entry(std::string_view executable, bool start_minimized);

/// The Exec value of a desktop-entry file, unquoted. Empty when the text carries
/// no Exec key, so a caller can tell "no entry" from "an entry naming something
/// else".
[[nodiscard]] std::string desktop_entry_command_line(std::string_view text);

/// Whether a stored command line requests the minimized start.
[[nodiscard]] bool carries_start_minimized(std::string_view command_line);

/// The executable named by a stored command line, with its quoting resolved.
/// Empty when nothing usable is parsed, so a caller can tell "no entry" from "an
/// entry naming something else".
[[nodiscard]] std::string command_line_executable(std::string_view command_line);

/// Whether a stored command line names `executable`: the "is this entry ours"
/// question. An empty `executable` or a command line that parses to nothing
/// answers `false`, so a caller can never mistake a missing entry for its own.
[[nodiscard]] bool command_line_names(std::string_view command_line, std::string_view executable);

/// Whether one process argument is the switch. At least one leading dash is
/// required (`-start-minimized` and `--start-minimized` are both accepted) and
/// the comparison is case-sensitive, as it is on Linux. A bare word with the
/// same letters is a file name, not a switch.
[[nodiscard]] bool is_start_minimized_switch(std::string_view argument);

/// Whether a *launch-time* reconciliation may rewrite the entry that is already
/// registered. An entry naming a different executable that still exists belongs
/// to another installation and is left alone; an absent, unparsable or
/// dead-target entry is repaired.
[[nodiscard]] bool launch_may_rewrite(std::string_view stored_command_line,
    std::string_view this_executable, bool target_exists);

/// Whether this process was launched with the switch anywhere in `argv`.
/// `argc`/`argv` are read before QApplication rewrites them.
[[nodiscard]] bool wants_start_minimized(int argc, char** argv);

} // namespace voicetyper::platform::linuxos
