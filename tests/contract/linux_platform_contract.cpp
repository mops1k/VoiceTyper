// Linux-only contract test for the Linux platform backends.
//
// What this proves, and what it deliberately does not:
//
//   Proves, on Linux, with no desktop interaction:
//     * linux_app_path_roots: the XDG layout the composition root must use
//       (settings under XDG_CONFIG_HOME, models/logs/updates under
//       XDG_DATA_HOME), the HOME fallbacks, the empty-variable rule and the
//       "no environment at all" fallback to the application directory.
//
//   Deliberately NOT exercised automatically: the real microphone, the real
//   global hotkeys and the real text injection. Those need a desktop session,
//   a device and a focused window, and belong to the physical Arch Linux gate.
//
// Every device-dependent section prints a skip reason instead of failing when
// the environment cannot provide it, and nothing waits without a bound.

#include "domain/app_paths.hpp"
#include "domain/error.hpp"
#include "platform/api/executor.hpp"
#include "platform/api/paths.hpp"
#include "platform/linux/linux_executor.hpp"
#include "platform/linux/linux_paths.hpp"
#include "platform/linux/linux_startup.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

using namespace voicetyper;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

void check_linux_app_path_roots()
{
    using namespace voicetyper::domain;

    // The documented XDG layout: settings are configuration, everything the
    // application caches or writes (models, logs, updates) is data.
    const EnvironmentLookup xdg = [](std::string_view name) -> std::optional<std::string> {
        if (name == "XDG_CONFIG_HOME") {
            return std::string("/home/x/.config");
        }
        if (name == "XDG_DATA_HOME") {
            return std::string("/home/x/.local/share");
        }
        if (name == "HOME") {
            return std::string("/home/x");
        }
        return std::nullopt;
    };
    const std::filesystem::path config("/home/x/.config");
    const std::filesystem::path data("/home/x/.local/share");
    const AppPaths paths(linux_app_path_roots(xdg, "/usr/bin"));
    check(paths.settings_directory() == config / "VoiceTyper",
        "settings live in XDG_CONFIG_HOME");
    check(paths.settings_file() == config / "VoiceTyper" / "settings.json",
        "settings file is the XDG one");
    check(paths.models_directory() == data / "VoiceTyper" / "models",
        "models live in XDG_DATA_HOME");
    check(paths.logs_directory() == data / "VoiceTyper" / "logs",
        "logs live in XDG_DATA_HOME");
    check(paths.updates_directory() == data / "VoiceTyper" / "updates",
        "updates live in XDG_DATA_HOME");
    check(paths.native_library_file("libparakeet.so").parent_path() == std::filesystem::path("/usr/bin"),
        "native libraries sit beside the executable");

    // HOME is the documented fallback when the XDG variables are unset.
    const EnvironmentLookup home_only = [](std::string_view name) -> std::optional<std::string> {
        return name == "HOME" ? std::optional<std::string>("/home/x") : std::nullopt;
    };
    const AppPaths from_home(linux_app_path_roots(home_only, "/usr/bin"));
    check(from_home.settings_directory() == std::filesystem::path("/home/x/.config") / "VoiceTyper",
        "an unset XDG_CONFIG_HOME falls back to $HOME/.config");
    check(from_home.models_directory() == std::filesystem::path("/home/x/.local/share") / "VoiceTyper" / "models",
        "an unset XDG_DATA_HOME falls back to $HOME/.local/share");

    // A stripped environment must not produce an empty path.
    const EnvironmentLookup empty;
    const AppPaths sandboxed(linux_app_path_roots(empty, "/opt/voicetyper"));
    check(sandboxed.settings_directory() == std::filesystem::path("/opt/voicetyper") / "VoiceTyper",
        "with no environment at all the application directory is used");
    check(sandboxed.models_directory() == std::filesystem::path("/opt/voicetyper") / "VoiceTyper" / "models",
        "the models directory never becomes empty");

    // An empty variable counts as unset, not as a root of "".
    const EnvironmentLookup blank_values = [](std::string_view) -> std::optional<std::string> {
        return std::string();
    };
    const AppPaths blank(linux_app_path_roots(blank_values, "/opt/voicetyper"));
    check(blank.settings_directory() == std::filesystem::path("/opt/voicetyper") / "VoiceTyper",
        "an empty environment variable is treated as unset");

    // A missing data root falls back to the config one and the other way round,
    // exactly like the Windows resolver, so a partially set environment still
    // produces one consistent pair of roots.
    const EnvironmentLookup config_only = [](std::string_view name) -> std::optional<std::string> {
        return name == "XDG_CONFIG_HOME" ? std::optional<std::string>("/only-config") : std::nullopt;
    };
    const AppPaths from_config(linux_app_path_roots(config_only, "/opt/voicetyper"));
    check(from_config.models_directory() == std::filesystem::path("/only-config") / "VoiceTyper" / "models",
        "a missing data root falls back to the config root");

    const EnvironmentLookup data_only = [](std::string_view name) -> std::optional<std::string> {
        return name == "XDG_DATA_HOME" ? std::optional<std::string>("/only-data") : std::nullopt;
    };
    const AppPaths from_data(linux_app_path_roots(data_only, "/opt/voicetyper"));
    check(from_data.settings_directory() == std::filesystem::path("/only-data") / "VoiceTyper",
        "a missing config root falls back to the data root");
}

void check_linux_executable_path()
{
    using namespace voicetyper::platform::linuxos;

    const std::filesystem::path executable = executable_file_path();
    check(!executable.empty(), "the executable path is never empty");
    check(executable.is_absolute(), "the executable path is absolute");

    std::error_code error;
    check(std::filesystem::exists(executable, error) && !error,
        "the executable path names a file that exists");
    check(executable == std::filesystem::read_symlink("/proc/self/exe", error) && !error,
        "the executable path is the running binary, not a guess");
}

void check_linux_executor()
{
    using namespace voicetyper::platform;

    LinuxExecutor executor;
    check(executor.is_running(), "the executor starts its worker thread");

    const std::thread::id caller = std::this_thread::get_id();
    std::thread::id inside{};
    std::atomic<bool> ran{false};
    const auto status = executor.invoke([&] {
        inside = std::this_thread::get_id();
        ran.store(true);
    }, std::chrono::milliseconds(2000));
    check(status.is_ok() && ran.load(), "invoke runs the task and reports success");
    check(inside != caller, "the task runs on the executor thread, not on the caller");
    check(!executor.on_this_thread(), "on_this_thread() is false on the caller");

    // The self-deadlock trap: invoke() from the executor's own thread cannot
    // post-and-wait, because the only thread able to run the task is the one
    // blocked in the wait. It has to run inline.
    std::atomic<bool> inline_ran{false};
    std::atomic<bool> nested_ok{false};
    const auto nested = executor.invoke([&] {
        check(executor.on_this_thread(), "on_this_thread() is true inside a task");
        const auto inner = executor.invoke([&] { inline_ran.store(true); },
            std::chrono::milliseconds(1000));
        nested_ok.store(inner.is_ok());
    }, std::chrono::milliseconds(2000));
    check(nested.is_ok(), "a task that invokes the executor again completes");
    check(nested_ok.load() && inline_ran.load(), "a nested invoke from the executor thread runs inline");

    // An empty task and a throwing task never cross the boundary.
    check(executor.invoke(Task{}, std::chrono::milliseconds(100)).code() == domain::ErrorCode::invalid_argument,
        "an empty task is invalid_argument");
    const auto thrown = executor.invoke([] { throw std::runtime_error("boom"); },
        std::chrono::milliseconds(2000));
    check(thrown.code() == domain::ErrorCode::internal, "a throwing task becomes internal");
    check(executor.is_running(), "the worker survives a throwing task");

    // A delayed post runs, and pending() sees it while it waits.
    std::atomic<int> counter{0};
    const auto generation = executor.generation();
    executor.post_delayed(generation, [&] { counter.fetch_add(1); }, std::chrono::milliseconds(30));
    check(executor.pending() >= 1, "a delayed post is visible in pending()");
    check(executor.wait_for_idle(std::chrono::milliseconds(2000)), "the delayed task runs");
    check(counter.load() == 1, "the delayed task ran exactly once");

    // A task tagged with another generation is dropped silently.
    executor.post(generation + 1, [&] { counter.fetch_add(10); });
    check(executor.wait_for_idle(std::chrono::milliseconds(2000)), "the stale post is consumed");
    check(counter.load() == 1, "a task from another generation never runs");

    // shutdown() invalidates what is already queued and keeps the executor
    // usable for the next session.
    const auto stale = executor.generation();
    executor.post_delayed(stale, [&] { counter.fetch_add(100); }, std::chrono::milliseconds(50));
    const auto shutdown = executor.shutdown(std::chrono::milliseconds(2000));
    check(shutdown.is_ok(), "shutdown succeeds");
    check(executor.generation() != stale, "shutdown advances the generation");
    check(executor.wait_for_idle(std::chrono::milliseconds(2000)), "the queue is drained by shutdown");
    check(counter.load() == 1, "a task queued before shutdown never runs");

    const auto after_shutdown = executor.invoke([&] { counter.fetch_add(1); },
        std::chrono::milliseconds(2000));
    check(after_shutdown.is_ok() && counter.load() == 2, "the executor is reusable after shutdown");
}

std::filesystem::path temp_directory()
{
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() / ("voicetyper-linux-" + stamp);
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void best_effort_remove(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

void check_linux_startup()
{
    using namespace voicetyper::platform::linuxos;

    const auto root = temp_directory();
    std::error_code error;
    std::filesystem::create_directories(root, error);
    const std::filesystem::path autostart_dir = root / "autostart";
    const std::string executable = "/opt/voicetyper/voicetyper-qt-shell";

    StartupOptions options;
    options.autostart_directory = autostart_dir;
    options.executable_path = executable;
    const LinuxStartup startup(options);

    // A first run: nothing is registered, and that is a normal state.
    const auto absent = startup.read();
    check(absent.is_ok() && !absent.value().present, "an unregistered autostart entry reads as absent");
    check(!startup.is_enabled(), "is_enabled is false with no entry");

    // Enabling writes a freedesktop autostart entry beside the other ones.
    const auto enabled = startup.set_enabled(true, false);
    check(enabled.is_ok(), "enabling autostart succeeds");
    check(std::filesystem::exists(startup.file_path(), error) && !error, "the desktop entry exists");
    const std::string first_write = read_file(startup.file_path());
    check(first_write.find("Type=Application") != std::string::npos, "the entry declares an application");
    check(first_write.find("Exec=" + executable) != std::string::npos,
        "the entry runs the pinned executable");
    check(first_write.find("--start-minimized") == std::string::npos,
        "a normal start carries no minimized switch");

    const auto registered = startup.read();
    check(registered.is_ok() && registered.value().present, "the entry reads as present");
    check(registered.value().command_line == executable, "the stored command line is the executable");
    check(!registered.value().start_minimized, "the stored entry does not request a minimized start");
    check(startup.is_enabled(), "is_enabled is true after enabling");

    // Enabling twice stores the same bytes.
    check(startup.set_enabled(true, false).is_ok(), "a second enable succeeds");
    check(read_file(startup.file_path()) == first_write, "enabling twice is byte-identical");

    // start_minimized is part of the stored command line.
    check(startup.set_enabled(true, true).is_ok(), "enabling with a minimized start succeeds");
    const auto minimized = startup.read();
    check(minimized.is_ok() && minimized.value().start_minimized,
        "the stored entry requests a minimized start");
    check(minimized.value().command_line == executable + " --start-minimized",
        "the minimized start is spelled as the launch switch");

    // Disabling removes the entry and is idempotent.
    check(startup.set_enabled(false, false).is_ok(), "disabling autostart succeeds");
    check(!std::filesystem::exists(startup.file_path(), error), "the desktop entry is gone");
    check(startup.set_enabled(false, false).is_ok(), "disabling twice succeeds");
    const auto disabled = startup.read();
    check(disabled.is_ok() && !disabled.value().present, "a disabled entry reads as absent");

    // The pure helpers: quoting, parsing and the reconciliation rule.
    check(build_desktop_entry("/opt/Voice Typer/voicetyper", false).find("Exec=\"/opt/Voice Typer/voicetyper\"")
            != std::string::npos,
        "an executable with a space is quoted in Exec");
    check(carries_start_minimized(executable + " --start-minimized"),
        "the minimized switch is recognised in a stored command line");
    check(!carries_start_minimized(executable), "a plain command line carries no switch");
    check(is_start_minimized_switch("--start-minimized"), "the double-dash switch is recognised");
    check(is_start_minimized_switch("-start-minimized"), "the single-dash switch is recognised");
    check(!is_start_minimized_switch("start-minimized"), "a bare word is a file name, not a switch");

    const char* argv_minimized[] = {"voicetyper-qt-shell", "--start-minimized"};
    const char* argv_plain[] = {"voicetyper-qt-shell"};
    check(wants_start_minimized(2, const_cast<char**>(argv_minimized)), "the launch switch is honoured");
    check(!wants_start_minimized(1, const_cast<char**>(argv_plain)), "a plain launch is not minimized");

    // An entry that names another, still existing installation is left alone; a
    // dead target or our own path is repaired.
    check(!launch_may_rewrite("/other/voicetyper --start-minimized", executable, true),
        "an entry belonging to another live installation is not rewritten");
    check(launch_may_rewrite("/other/voicetyper", executable, false),
        "an entry naming a dead executable is repaired");
    check(launch_may_rewrite(executable, executable, true), "our own entry is rewritten");

    best_effort_remove(root);
}

} // namespace

int main()
{
    check_linux_app_path_roots();
    check_linux_executable_path();
    check_linux_executor();
    check_linux_startup();
    if (failures != 0) {
        std::cerr << "linux-platform-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "linux-platform-contract: OK\n";
    return 0;
}
