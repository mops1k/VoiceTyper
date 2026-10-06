#include "core/support/update_launcher.hpp"

#include <fstream>
#include <string>

namespace voicetyper::core::support {

std::string build_update_runner_script(std::string_view installer_path, std::string_view app_path)
{
    // Byte-for-byte the .NET concatenation (UpdateLauncher.cs:33-36):
    //   "@echo off\r\n"
    //   + $"start \"\" /wait \"{installerPath}\" /AutoUpdate\r\n"
    //   + $"start \"\" \"{appPath}\"\r\n"
    // The empty "" is the window title `start` needs before a quoted path.
    std::string script;
    script.reserve(installer_path.size() + app_path.size() + 64);
    script += "@echo off\r\n";
    script += "start \"\" /wait \"";
    script += installer_path;
    script += "\" /AutoUpdate\r\n";
    script += "start \"\" \"";
    script += app_path;
    script += "\"\r\n";
    return script;
}

std::string build_update_runner_arguments(std::string_view runner_path)
{
    std::string arguments = "/d /c \"";
    arguments += runner_path;
    arguments += '"';
    return arguments;
}

domain::Status write_update_runner_script(
    const std::filesystem::path& runner_path, std::string_view installer_path, std::string_view app_path)
{
    // An empty path would still produce a syntactically valid script that starts
    // nothing, so every caller mistake is refused before a file appears.
    if (runner_path.empty()) {
        return domain::Status::failure(domain::ErrorCode::invalid_argument, "update runner path is empty");
    }
    if (installer_path.empty()) {
        return domain::Status::failure(domain::ErrorCode::invalid_argument, "installer path is empty");
    }
    if (app_path.empty()) {
        return domain::Status::failure(domain::ErrorCode::invalid_argument, "application path is empty");
    }

    // Binary, deliberately: the script carries explicit CRLF, and a Windows text
    // stream would translate each LF into a second CR ("\r\r\n"), which cmd.exe
    // never saw from the .NET File.WriteAllText this reproduces.
    std::ofstream output(runner_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return domain::Status::failure(
            domain::ErrorCode::io_failure, "cannot open the update runner script: " + runner_path.string());
    }

    const std::string script = build_update_runner_script(installer_path, app_path);
    output.write(script.data(), static_cast<std::streamsize>(script.size()));
    output.flush();
    if (!output.good()) {
        // A half-written script would launch the wrong thing (or nothing), so a
        // failed flush is reported instead of being assumed successful.
        return domain::Status::failure(
            domain::ErrorCode::io_failure, "cannot write the update runner script: " + runner_path.string());
    }

    return domain::Status::success();
}

} // namespace voicetyper::core::support
