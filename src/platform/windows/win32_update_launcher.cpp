#include "platform/windows/win32_update_launcher.hpp"

#include "core/support/update_launcher.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace voicetyper::platform::win32 {
namespace {

/// The UTF-8 bytes of a path: the encoding File.WriteAllText wrote, so cmd.exe
/// receives the byte sequence the .NET launcher produced.
[[nodiscard]] std::string to_utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// UTF-16 for CreateProcessW. UTF-8 is the app's path encoding, so the one
/// conversion happens here rather than at every call site.
[[nodiscard]] std::wstring to_wide(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, wide.data(), size) <= 0) {
        return {};
    }
    return wide;
}

/// A path for a diagnostic message, in the same UTF-8 encoding the script uses.
[[nodiscard]] std::string to_log_text(const std::filesystem::path& path)
{
    return to_utf8(path);
}

} // namespace

domain::Status start_update_runner(const std::filesystem::path& runner_path)
{
    if (runner_path.empty()) {
        return domain::Status::failure(domain::ErrorCode::invalid_argument, "update runner path is empty");
    }

    // The command line is `cmd.exe` plus the arguments the portable builder
    // produces; CreateProcessW may write into the buffer, so it is a vector, not
    // a string literal.
    const std::wstring command_line
        = to_wide(std::string(core::support::kUpdateRunnerProgram) + ' '
            + core::support::build_update_runner_arguments(to_utf8(runner_path)));
    std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    // CREATE_NO_WINDOW is the .NET `CreateNoWindow = true`: the user must not see
    // a console window appear while the app is closing.
    const BOOL started = ::CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process);
    if (started == FALSE) {
        const DWORD last_error = ::GetLastError();
        return domain::Status::failure(domain::ErrorCode::io_failure,
            "cannot start the update runner (GetLastError=" + std::to_string(last_error) + ')');
    }

    // Nothing waits for the runner (the app exits next), and both handles
    // CreateProcess hands over must be closed or the child would keep them alive.
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return domain::Status::success();
}

domain::Status launch_update(const std::filesystem::path& runner_path,
    const std::filesystem::path& installer_path, const std::filesystem::path& app_path)
{
    if (runner_path.empty()) {
        return domain::Status::failure(domain::ErrorCode::invalid_argument, "update runner path is empty");
    }
    if (installer_path.empty()) {
        return domain::Status::failure(domain::ErrorCode::invalid_argument, "installer path is empty");
    }
    if (app_path.empty()) {
        return domain::Status::failure(domain::ErrorCode::invalid_argument, "application path is empty");
    }

    std::error_code error;
    if (!std::filesystem::exists(installer_path, error) || error) {
        return domain::Status::failure(
            domain::ErrorCode::not_found, "the installer does not exist: " + to_log_text(installer_path));
    }

    // A clean profile has no updates directory yet; the .NET launcher created it
    // before writing the script, and so does this one.
    std::filesystem::create_directories(runner_path.parent_path(), error);
    if (error) {
        return domain::Status::failure(domain::ErrorCode::io_failure,
            "cannot create the updates directory: " + error.message());
    }

    const domain::Status written = core::support::write_update_runner_script(
        runner_path, to_utf8(installer_path), to_utf8(app_path));
    if (!written.is_ok()) {
        return written;
    }

    return start_update_runner(runner_path);
}

} // namespace voicetyper::platform::win32
