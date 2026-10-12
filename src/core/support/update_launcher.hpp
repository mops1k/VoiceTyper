#pragma once

// The update hand-off: `run-update.cmd`, as VoiceTyper.App/Services/UpdateLauncher.cs
// wrote and started it.
//
// Why a script at all: the installer cannot replace the running executable, so a
// detached batch file waits for the application to exit, runs the installer and then
// starts the app again - the new version after a successful install, the previous one
// after a failure.
//
// Two deliberate departures from the .NET script (Alexander, 2026-10-12), both about
// an automatic update nobody is watching:
//   * it waits for the application process to exit before touching the installer,
//     because Inno refuses to start while the app is still up and shows a dialog that
//     waits for a click;
//   * it starts the installer with /SILENT /SUPPRESSMSGBOXES, so neither a wizard page
//     nor a message box can stop the update.
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

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace voicetyper::core::support {

/// The program the runner is started through, UpdateLauncher.cs:41.
inline constexpr std::string_view kUpdateRunnerProgram = "cmd.exe";

/// How many times the runner re-checks whether the application is still running.
/// One attempt is about a second (`ping -n 2 127.0.0.1`), so the bound is ~2 minutes.
inline constexpr int kUpdateRunnerWaitAttempts = 120;

/// The exact bytes of `run-update.cmd`:
/// `@echo off` CRLF, a bounded wait for the application process (`app_process_id`,
/// filtered by its image name so a recycled PID cannot keep it alive), the quoted
/// installer waited for with `/SILENT /SUPPRESSMSGBOXES /AutoUpdate`, then the quoted
/// app - in that order, because the app must start after the installation has finished.
///
/// Pure text: an empty path still yields a well-formed script, so the shape of
/// the file never depends on validation. Rejecting empty paths is the writer's
/// and the launcher's job.
[[nodiscard]] std::string build_update_runner_script(std::string_view installer_path,
    std::string_view app_path, std::uint32_t app_process_id);

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
[[nodiscard]] domain::Status write_update_runner_script(const std::filesystem::path& runner_path,
    std::string_view installer_path, std::string_view app_path, std::uint32_t app_process_id);

} // namespace voicetyper::core::support
