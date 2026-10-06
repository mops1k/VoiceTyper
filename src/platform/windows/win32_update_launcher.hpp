#pragma once

// Windows hand-off for an update: writes `run-update.cmd` and starts it hidden,
// reproducing VoiceTyper.App/Services/UpdateLauncher.cs:13-50.
//
// Platform boundary: this header is standard C++20 and includes no Windows
// header, so the paths, the validation and the script bytes stay testable on any
// host; the single OS call (CreateProcessW) lives in win32_update_launcher.cpp.
// The script bytes and the cmd.exe arguments are produced by
// src/core/support/update_launcher.hpp, which the contract test asserts byte for
// byte - so what this backend starts is already the contract-tested text.
//
// Threading: start_update_runner does not wait for the runner; the caller is
// expected to quit the app right afterwards, exactly as the .NET launcher did.
//
// Parity caveat (deliberately unchanged): the script is written as UTF-8
// without a BOM, the encoding File.WriteAllText used, and cmd.exe reads a batch
// file in the console's current code page. A path outside that code page is
// therefore mangled exactly as it was in the .NET build; changing it would be a
// new policy decision, not a port (compatibility-contracts.md §8).

#include "domain/error.hpp"

#include <filesystem>

namespace voicetyper::platform::win32 {

/// Starts `cmd.exe /d /c "<runner_path>"` with CREATE_NO_WINDOW and returns
/// without waiting for it.
///
/// Failure codes: invalid_argument (empty path), io_failure (CreateProcessW
/// refused; the Win32 error code is kept in the message), so a caller can log
/// why the update did not start instead of showing a progress bar forever.
[[nodiscard]] domain::Status start_update_runner(const std::filesystem::path& runner_path);

/// Writes the runner script and starts it: the whole hand-off.
///
/// `runner_path` is the caller's path (AppPaths::update_launcher_file()); its
/// parent directory is created when absent, which is the .NET
/// Directory.CreateDirectory step and matters on a clean profile that has never
/// downloaded an update.
///
/// Refuses an installer that does not exist (the .NET FileNotFoundException) and
/// a missing app path, because a script pointing at nothing looks like a
/// successful hand-off and then starts nothing after the app has exited.
[[nodiscard]] domain::Status launch_update(const std::filesystem::path& runner_path,
    const std::filesystem::path& installer_path, const std::filesystem::path& app_path);

} // namespace voicetyper::platform::win32
