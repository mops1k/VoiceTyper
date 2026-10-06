#pragma once

// The update hand-off: `run-update.cmd`, as VoiceTyper.App/Services/UpdateLauncher.cs
// writes and starts it.
//
// Why a script at all: the installer cannot replace the running executable, so a
// detached batch file waits for the Inno Setup wizard (/AutoUpdate turns off the
// installer's own [Run] relaunch) and then starts the app again - the new version
// after a successful install, the previous one after a cancel or a failure.
//
// Split on purpose: this module owns the *bytes* and the file write, so a test
// can assert the script byte for byte (including the CRLF pairs) and the cmd.exe
// arguments without starting anything. Starting the hidden cmd.exe is the
// Windows backend (src/platform/windows/win32_update_launcher.hpp).
//
// The script carries explicit CRLF, which is why it must be written in binary
// mode on every platform: a Windows text stream would translate each LF into a
// second CR and produce "\r\r\n", which is not what cmd.exe (or the .NET
// File.WriteAllText that this reproduces) would have written.

#include "domain/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace voicetyper::core::support {

/// The program the runner is started through, UpdateLauncher.cs:41.
inline constexpr std::string_view kUpdateRunnerProgram = "cmd.exe";

/// The exact bytes of `run-update.cmd`:
/// `@echo off` CRLF, the quoted installer waited for with `/AutoUpdate`, then
/// the quoted app - in that order, because the app must start after the wizard
/// has finished.
///
/// Pure text: an empty path still yields a well-formed script, so the shape of
/// the file never depends on validation. Rejecting empty paths is the writer's
/// and the launcher's job.
[[nodiscard]] std::string build_update_runner_script(std::string_view installer_path, std::string_view app_path);

/// The cmd.exe arguments that run the script: `/d /c "<runner_path>"`.
///
/// `/d` skips AutoRun commands, so a user's AutoRun entry cannot change what the
/// updater does; `/c` runs the script and then terminates.
[[nodiscard]] std::string build_update_runner_arguments(std::string_view runner_path);

/// Writes build_update_runner_script(...) to `runner_path` in binary mode.
///
/// Deliberately does not create the parent directory: the updates directory
/// belongs to AppPaths, and a caller that forgot to prepare it must see
/// io_failure rather than believe a hand-off exists. An empty `runner_path` is
/// invalid_argument; an empty installer or app path is invalid_argument too,
/// because such a script would silently start nothing.
[[nodiscard]] domain::Status write_update_runner_script(
    const std::filesystem::path& runner_path, std::string_view installer_path, std::string_view app_path);

} // namespace voicetyper::core::support
