#include "domain/app_paths.hpp"
#include "domain/cpu_topology.hpp"
#include "domain/file_logger.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace voicetyper;
using namespace voicetyper::platform;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

std::filesystem::path temp_directory()
{
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() / ("voicetyper-core-" + stamp);
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void best_effort_remove(const std::filesystem::path& path)
{
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (!error) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // A scanner may hold a transient Windows handle. Cleanup is test hygiene,
    // not a product contract; never turn that external condition into a flake.
}

void check_paths()
{
    using namespace voicetyper::domain;
    const AppPaths paths(AppPathRoots{"/roots/roaming", "/roots/local", "/app"});
    check(paths.settings_directory().filename() == "VoiceTyper", "settings directory is the product directory");
    check(paths.settings_file().filename() == "settings.json", "settings file name");
    check(paths.settings_temp_file().filename() == "settings.json.tmp", "settings temp name");
    check(paths.models_directory().parent_path().filename() == "VoiceTyper" && paths.models_directory().filename() == "models", "models path");
    check(paths.logs_directory().filename() == "logs" && paths.log_file().filename() == "voiceTyper.log", "log path");
    check(paths.log_archive_file(5).filename() == "voiceTyper.5.log", "log archive path");
    check(paths.updates_directory().filename() == "updates", "updates path");
    check(paths.update_installer_file("1.2.3").filename() == "VoiceTyper-1.2.3-Setup.exe", "installer name");
    check(paths.update_installer_temp_file("1.2.3").filename() == "VoiceTyper-1.2.3-Setup.exe.download", "installer temp name");
    check(paths.update_launcher_file().filename() == "run-update.cmd", "launcher name");
    check(paths.native_library_file("parakeet.dll").filename() == "parakeet.dll" && paths.native_library_file("parakeet.dll").parent_path() == std::filesystem::path("/app"), "native library path");
}

void check_windows_app_path_roots()
{
    using namespace voicetyper::domain;

    // The .NET layout: settings are Roaming (%APPDATA%), models/logs/updates are
    // Local (%LOCALAPPDATA%). A composition root that decided this itself put the
    // settings into the Local profile and silently ignored the user's real file.
    const EnvironmentLookup both = [](std::string_view name) -> std::optional<std::string> {
        if (name == "APPDATA") {
            return std::string("C:/Users/x/AppData/Roaming");
        }
        if (name == "LOCALAPPDATA") {
            return std::string("C:/Users/x/AppData/Local");
        }
        return std::nullopt;
    };
    const std::filesystem::path roaming("C:/Users/x/AppData/Roaming");
    const std::filesystem::path local("C:/Users/x/AppData/Local");
    const AppPaths paths(windows_app_path_roots(both, "C:/apps/VoiceTyper"));
    check(paths.settings_directory() == roaming / "VoiceTyper",
        "settings live in the Roaming profile, as the .NET SettingsService did");
    check(paths.settings_file() == roaming / "VoiceTyper" / "settings.json", "settings file is the Roaming one");
    check(paths.models_directory() == local / "VoiceTyper" / "models", "models live in the Local profile");
    check(paths.logs_directory() == local / "VoiceTyper" / "logs", "logs live in the Local profile");
    check(paths.updates_directory() == local / "VoiceTyper" / "updates", "updates live in the Local profile");
    check(paths.native_library_file("parakeet.dll").parent_path() == std::filesystem::path("C:/apps/VoiceTyper"),
        "native libraries sit beside the executable");

    // A stripped environment must not produce an empty path: the missing root
    // falls back to the other one.
    const EnvironmentLookup local_only = [](std::string_view name) -> std::optional<std::string> {
        return name == "LOCALAPPDATA" ? std::optional<std::string>("C:/only-local") : std::nullopt;
    };
    const AppPaths from_local(windows_app_path_roots(local_only, "C:/apps"));
    check(from_local.settings_directory() == std::filesystem::path("C:/only-local") / "VoiceTyper",
        "a missing roaming root falls back to the local one");

    const EnvironmentLookup roaming_only = [](std::string_view name) -> std::optional<std::string> {
        return name == "APPDATA" ? std::optional<std::string>("C:/only-roaming") : std::nullopt;
    };
    const AppPaths from_roaming(windows_app_path_roots(roaming_only, "C:/apps"));
    check(from_roaming.models_directory() == std::filesystem::path("C:/only-roaming") / "VoiceTyper" / "models",
        "a missing local root falls back to the roaming one");

    const EnvironmentLookup empty;
    const AppPaths sandboxed(windows_app_path_roots(empty, "C:/apps"));
    check(sandboxed.settings_directory() == std::filesystem::path("C:/apps") / "VoiceTyper",
        "with no environment at all the application directory is used");
    check(sandboxed.logs_directory() == std::filesystem::path("C:/apps") / "VoiceTyper" / "logs",
        "the log directory never becomes empty");

    // An empty variable counts as unset, not as a root of "".
    const EnvironmentLookup blank_values = [](std::string_view) -> std::optional<std::string> {
        return std::string();
    };
    const AppPaths blank(windows_app_path_roots(blank_values, "C:/apps"));
    check(blank.settings_directory() == std::filesystem::path("C:/apps") / "VoiceTyper",
        "an empty environment variable is treated as unset");
}

void check_logger()
{
    using namespace voicetyper::domain;
    const auto directory = temp_directory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    check(!error, "logger temp directory is created");

    platform::LoggerOptions options;
    options.rotate_threshold_bytes = 200;
    options.archive_count = 5;
    FileLogger logger(directory, options, [] { return std::string("2026-01-02 03:04:05.678"); }, "\n");
    check(logger.log_directory() == directory.string(), "logger directory is exposed");
    check(logger.log_file_path() == (directory / "voiceTyper.log").string(), "logger file is exposed");
    check(logger.write(platform::LogLevel::info, "startup").is_ok(), "logger writes info");
    check(logger.write(platform::LogLevel::warn, "warning").is_ok(), "logger writes warn");
    check(logger.write(platform::LogLevel::error, "failure", "stack detail").is_ok(), "logger writes error detail");

    const auto content = read_file(logger.log_file_path());
    check(content.find("2026-01-02 03:04:05.678 [INFO] startup\n") != std::string::npos, "INFO format");
    check(content.find("[WARN] warning") != std::string::npos, "WARN format");
    check(content.find("[ERROR] failure\nstack detail\n") != std::string::npos, "ERROR detail line");

    platform::LoggerOptions rotation_options;
    rotation_options.rotate_threshold_bytes = 40;
    rotation_options.archive_count = 5;
    FileLogger rotation_logger(directory, rotation_options, [] { return std::string("2026-01-02 03:04:05.678"); }, "\n");
    for (int i = 0; i < 8; ++i) {
        (void)rotation_logger.info("rotation line that crosses the threshold");
    }
    check(std::filesystem::exists(directory / "voiceTyper.1.log"), "rotation creates archive 1");
    check(!std::filesystem::exists(directory / "voiceTyper.6.log"), "rotation retains at most five archives");
    const auto tail = rotation_logger.tail(3);
    check(tail.is_ok() && !tail.value().empty(), "tail returns lines");
    check(tail.value().find("[INFO]") != std::string::npos, "tail contains a formatted line");

    check(logger.clear().is_ok(), "clear succeeds");
    check(read_file(logger.log_file_path()).empty(), "clear truncates only the current log");
    check(std::filesystem::exists(directory / "voiceTyper.1.log"), "clear keeps archives");

    platform::LoggerOptions clear_options;
    clear_options.clear_on_start = true;
    FileLogger startup_logger(directory, clear_options, [] { return std::string("2026-01-02 03:04:05.678"); }, "\n");
    check(read_file(startup_logger.log_file_path()).empty(), "clear_on_start truncates the current log");
    best_effort_remove(directory);
}

void check_cpu()
{
    using namespace voicetyper::domain;
    const auto unknown = make_cpu_topology(8);
    check(!unknown.physical_cores_known && unknown.physical_cores == 4, "unknown physical cores use .NET fallback");
    check(platform::inference_thread_count(unknown) == 4, "inference threads use physical cores");
    check(platform::vad_thread_count(unknown) == 4, "VAD threads use half logical processors");

    const auto large = make_cpu_topology(64, 32);
    check(platform::inference_thread_count(large) == 16, "inference threads clamp at 16");
    check(platform::vad_thread_count(large) == 8, "VAD threads clamp at 8");

    const auto tiny = make_cpu_topology(1, 1);
    check(platform::inference_thread_count(tiny) == 1, "inference threads clamp at 1");
    check(platform::vad_thread_count(tiny) == 2, "VAD threads clamp at 2");

    const StandardCpuTopologyProvider provider;
    const auto detected = provider.detect();
    check(detected.is_ok() && detected.value().logical_processors >= 1, "standard provider reports logical processors");
    check(!detected.value().physical_cores_known, "standard provider leaves physical cores unknown");
}

} // namespace

int main()
{
    check_paths();
    check_windows_app_path_roots();
    check_logger();
    check_cpu();
    if (failures != 0) {
        std::cerr << "core-support-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "core-support-contract: OK\n";
    return 0;
}
