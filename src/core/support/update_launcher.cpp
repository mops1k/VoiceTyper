#include "core/support/update_launcher.hpp"

#include <fstream>
#include <string>

namespace voicetyper::core::support {
namespace {

/// The last component of the application path: the image name `tasklist` filters on.
/// Split by hand rather than through std::filesystem so the bytes of a UTF-8 path are
/// never reinterpreted in the console code page.
[[nodiscard]] std::string app_image_name(std::string_view app_path)
{
    const std::size_t separator = app_path.find_last_of("\\/");
    if (separator == std::string_view::npos) {
        return std::string(app_path);
    }
    return std::string(app_path.substr(separator + 1));
}

} // namespace

std::string build_update_runner_script(
    std::string_view installer_path, std::string_view app_path, std::uint32_t app_process_id)
{
    // The .NET launcher wrote three lines:
    //   "@echo off\r\n"
    //   + $"start \"\" /wait \"{installerPath}\" /AutoUpdate\r\n"
    //   + $"start \"\" \"{appPath}\"\r\n"
    // The two deliberate differences (Alexander, 2026-10-12):
    //   * the script waits for THIS application process to exit first. The installer
    //     cannot replace a running executable, and Inno refuses to start while the
    //     application is up - it showed "обнаружена работающая программа VoiceTyper"
    //     because the app needs several seconds to shut down, so the dialog waited for
    //     a click that an automatic update has nobody to give;
    //   * the installer runs with /SILENT /SUPPRESSMSGBOXES: an automatic update must
    //     not stop at a wizard page or a message box. /AutoUpdate still keeps the
    //     installer from starting the app itself - the last line does that.
    // The wait is bounded (120 attempts of about one second): a hung application must
    // not make the updater wait forever, and a suppressed message box cannot block it.
    const std::string process_id = std::to_string(app_process_id);
    const std::string image_name = app_image_name(app_path);

    std::string script;
    script.reserve(installer_path.size() + app_path.size() + image_name.size() + 512);
    script += "@echo off\r\n";
    script += "set waited=0\r\n";
    script += ":wait\r\n";
    script += "tasklist /FI \"PID eq " + process_id + "\" /FI \"IMAGENAME eq " + image_name
        + "\" 2>nul | find \"" + process_id + "\" >nul\r\n";
    script += "if errorlevel 1 goto install\r\n";
    script += "set /a waited+=1 >nul\r\n";
    script += "if %waited% geq 120 goto install\r\n";
    script += "ping -n 2 127.0.0.1 >nul\r\n";
    script += "goto wait\r\n";
    script += ":install\r\n";
    script += "start \"\" /wait \"";
    script += installer_path;
    script += "\" /SILENT /SUPPRESSMSGBOXES /AutoUpdate\r\n";
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

domain::Status write_update_runner_script(const std::filesystem::path& runner_path,
    std::string_view installer_path, std::string_view app_path, std::uint32_t app_process_id)
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

    const std::string script = build_update_runner_script(installer_path, app_path, app_process_id);
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
