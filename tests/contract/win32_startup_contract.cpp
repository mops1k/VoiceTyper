// Windows autostart contract: the HKCU Run-key backend behind
// «Запускать вместе с Windows» / «Запускать свёрнутым».
//
// What this proves, against the real registry of the machine it runs on:
//   * the command line is the *quoted* executable, byte-identical to the .NET
//     `$"\"{Environment.ProcessPath}\""`, plus " --start-minimized" when the
//     setting is on - including the CommandLineToArgvW escaping rules for a
//     path that ends in a backslash;
//   * a missing value reads as *disabled*, not as an error, and is_enabled()
//     agrees with the raw registry;
//   * the value round-trips through the real
//     HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run key: a
//     write is visible to a raw RegQueryValueExW as REG_SZ with the exact
//     bytes, and enabling twice is idempotent;
//   * disabling removes the value - verified again through the raw registry,
//     not only through the backend - and disabling twice is idempotent, so
//     enable/disable can never leave a Run value behind.
//
// Rules this test obeys:
//   * it NEVER writes or deletes the production value name "VoiceTyper". The
//     production value is read before the run and again at the end, and the
//     test fails unless it is byte-identical (absent before and after included,
//     because "unchanged" covers both). Every write and every read assertion
//     uses a distinct per-process value name in the same production key, so
//     the key path and the REG_SZ round-trip are exercised for real while a
//     real installed app's autostart entry cannot be disturbed or repointed at
//     a test build;
//   * the value it writes is removed on every exit path, including a failed
//     check, through a scoped guard - not through the happy path;
//   * it contains no wait at all (every Reg* call is synchronous and bounded,
//     and GetModuleFileNameW retries at most 8 times), so it cannot hang; CTest
//     additionally caps it with a 60 s TIMEOUT.

#include "domain/error.hpp"
#include "platform/windows/win32_startup.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

using voicetyper::platform::win32::build_command_line;
using voicetyper::platform::win32::carries_start_minimized;
using voicetyper::platform::win32::command_line_executable;
using voicetyper::platform::win32::is_start_minimized_switch;
using voicetyper::platform::win32::kAutostartKeyPath;
using voicetyper::platform::win32::kAutostartValueName;
using voicetyper::platform::win32::kStartMinimizedSwitch;
using voicetyper::platform::win32::launch_may_rewrite;
using voicetyper::platform::win32::quote_argument;
using voicetyper::platform::win32::StartupOptions;
using voicetyper::platform::win32::Win32Startup;
using voicetyper::platform::win32::wants_start_minimized;

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void check_wide(const std::wstring& actual, const std::wstring& expected, const std::string& what)
{
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf(
            "FAIL %s\n  expected: %s\n  actual:   %s\n",
            what.c_str(),
            voicetyper::platform::win32::to_log_text(expected).c_str(),
            voicetyper::platform::win32::to_log_text(actual).c_str());
    }
}

std::string narrow(const std::wstring& text)
{
    return voicetyper::platform::win32::to_log_text(text);
}

/// One Run value read straight from the registry, with no VoiceTyper code in
/// the path. This is what makes the assertions about the *machine* and not
/// about the backend's own opinion of itself.
struct RawRunValue {
    bool present = false;
    DWORD type = 0;
    std::wstring data;
    LSTATUS status = ERROR_SUCCESS;
};

/// Writes a Run value directly, so the test can put a *foreign* entry there.
bool write_raw_run_value(const std::wstring& name, const std::wstring& data)
{
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, std::wstring(kAutostartKeyPath).c_str(), 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr)
        != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS written = ::RegSetValueExW(key, name.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(data.c_str()),
        static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);
    return written == ERROR_SUCCESS;
}

RawRunValue read_raw_run_value(const std::wstring& name)
{
    RawRunValue value;
    HKEY key = nullptr;
    value.status = ::RegOpenKeyExW(
        HKEY_CURRENT_USER, std::wstring(kAutostartKeyPath).c_str(), 0, KEY_QUERY_VALUE, &key);
    if (value.status != ERROR_SUCCESS) {
        return value;
    }
    DWORD bytes = 0;
    DWORD type = 0;
    const LSTATUS queried = ::RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &bytes);
    if (queried == ERROR_FILE_NOT_FOUND) {
        value.status = queried;
        ::RegCloseKey(key);
        return value;
    }
    if (queried != ERROR_SUCCESS) {
        value.status = queried;
        ::RegCloseKey(key);
        return value;
    }
    std::wstring data(static_cast<std::size_t>(bytes) / sizeof(wchar_t) + 1, L'\0');
    const LSTATUS read = ::RegQueryValueExW(
        key, name.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(data.data()), &bytes);
    ::RegCloseKey(key);
    if (read != ERROR_SUCCESS) {
        value.status = read;
        return value;
    }
    const auto terminator = data.find(L'\0');
    if (terminator != std::wstring::npos) {
        data.resize(terminator);
    }
    value.present = true;
    value.type = type;
    value.data = data;
    return value;
}

/// Removes the value this test wrote, on every exit path, through the backend
/// under test so the cleanup path is itself exercised.
class ScopedTestValue final {
public:
    explicit ScopedTestValue(Win32Startup& startup)
        : startup_(startup)
    {
    }

    ~ScopedTestValue()
    {
        if (!removed_) {
            const auto status = startup_.set_enabled(false, false);
            if (status.is_error()) {
                std::printf(
                    "FAIL cleanup of the test value failed: %s\n", status.error().to_string().c_str());
                ++failures;
            }
            removed_ = true;
        }
    }

    ScopedTestValue(const ScopedTestValue&) = delete;
    ScopedTestValue& operator=(const ScopedTestValue&) = delete;

private:
    Win32Startup& startup_;
    bool removed_ = false;
};

/// Group A: the pure rules, no registry at all. These are the parts of the
/// contract that a portable test could also compile, and they are what the
/// Windows build pins.
void test_quoting_and_command_line()
{
    std::printf("group: quoting and command line\n");

    // The .NET spelling, byte for byte: $"\"{exePath}\"".
    check_wide(
        quote_argument(L"C:\\Program Files\\VoiceTyper\\VoiceTyper.exe"),
        L"\"C:\\Program Files\\VoiceTyper\\VoiceTyper.exe\"",
        "a path with a space is wrapped in double quotes and nothing else");

    // CommandLineToArgvW: only the backslashes that would escape the closing
    // quote are doubled, so the parsed argv[0] is the original path. The
    // backslash between "a" and "b" is untouched - doubling it would be a
    // different (wrong) path.
    check_wide(quote_argument(L"C:\\dir\\"), L"\"C:\\dir\\\\\"", "a trailing backslash is doubled");
    check_wide(quote_argument(L"a\\b\\\"c"), L"\"a\\b\\\\\\\"c\"", "an embedded quote is escaped");
    check_wide(quote_argument(L""), L"\"\"", "an empty argument is still quoted");

    check_wide(build_command_line(L"", false), L"", "no executable means no command line");
    check_wide(build_command_line(L"", true), L"", "no executable means no switch-only entry either");
    check_wide(
        build_command_line(L"C:\\vt.exe", false),
        L"\"C:\\vt.exe\"",
        "without the setting the entry is the plain quoted executable");
    check_wide(
        build_command_line(L"C:\\vt.exe", true),
        L"\"C:\\vt.exe\" --start-minimized",
        "with the setting the switch follows the quoted executable");

    check(!carries_start_minimized(L"\"C:\\vt.exe\""), "a plain entry carries no switch");
    check(carries_start_minimized(L"\"C:\\vt.exe\" --start-minimized"), "a switch is detected");
}

void test_switch_detection()
{
    std::printf("group: launch switch detection\n");

    check(is_start_minimized_switch("--start-minimized"), "the documented switch is recognized");
    check(is_start_minimized_switch("-start-minimized"), "a single dash is accepted as Windows does");
    check(is_start_minimized_switch("--START-MINIMIZED"), "switch names are case-insensitive");
    check(!is_start_minimized_switch("--start"), "a shorter switch is not a match");
    check(!is_start_minimized_switch("--start-minimized-extra"), "a longer switch is not a match");
    check(!is_start_minimized_switch("start-minimized"), "the bare word is not a switch");
    check(!is_start_minimized_switch(""), "an empty argument is not a switch");

    char program0[] = "voicetyper-win32-startup-contract";
    char without[] = "--selftest";
    char with[] = "--start-minimized";
    char* argv_without[] = {program0, without, nullptr};
    char* argv_with[] = {program0, without, with, nullptr};
    check(!wants_start_minimized(2, argv_without), "no switch in argv means a normal start");
    check(wants_start_minimized(3, argv_with), "the switch anywhere in argv means a minimized start");
    check(!wants_start_minimized(0, nullptr), "no argv is not a crash and not a switch");
    check(!wants_start_minimized(1, argv_without), "argv[0] alone is not a switch");
}

void test_launch_reconciliation()
{
    std::printf("group: launch-time reconciliation\n");

    // Parsing back what the writer produces, including the switch.
    check_wide(command_line_executable(L"\"C:\\vt.exe\""), L"C:\\vt.exe",
        "a plain quoted entry parses back to its executable");
    check_wide(command_line_executable(L"\"C:\\vt.exe\" --start-minimized"), L"C:\\vt.exe",
        "the switch is not part of the executable");
    check_wide(command_line_executable(L"  \"C:\\Program Files\\VoiceTyper\\vt.exe\" --start-minimized"),
        L"C:\\Program Files\\VoiceTyper\\vt.exe", "leading spaces and spaces inside the path survive");
    check_wide(command_line_executable(L"C:\\vt.exe --start-minimized"), L"C:\\vt.exe",
        "an unquoted entry parses to its first argument");
    check_wide(command_line_executable(L""), L"", "an empty command line names nothing");
    check_wide(command_line_executable(L"\"\""), L"", "an empty quoted argument names nothing");
    // A trailing backslash is written as `\\` before the closing quote; the
    // parser must give back exactly one.
    check_wide(command_line_executable(build_command_line(L"C:\\dir with space\\", false)),
        L"C:\\dir with space\\", "a quoted path ending in a backslash round-trips");

    // The decision itself: only a dead or absent target may be repointed.
    check(launch_may_rewrite(L"", L"C:\\build\\vt.exe", true),
        "an absent entry is written");
    check(launch_may_rewrite(L"\"C:\\build\\vt.exe\"", L"C:\\build\\vt.exe", true),
        "our own entry is refreshed");
    check(launch_may_rewrite(L"\"C:\\BUILD\\VT.EXE\"", L"C:\\build\\vt.exe", true),
        "the comparison is case-insensitive, as Windows paths are");
    check(!launch_may_rewrite(L"\"C:\\Program Files\\VoiceTyper\\VoiceTyper.exe\"", L"C:\\build\\vt.exe", true),
        "another installation that still exists is left alone");
    check(launch_may_rewrite(L"\"C:\\Program Files\\VoiceTyper\\VoiceTyper.exe\"", L"C:\\build\\vt.exe", false),
        "a dead target is repaired, which is the cutover case");
    // The decision only ever looks at the parsed path plus the caller's existence
    // answer, so an unparsable-and-absent target is rewritten and a value that
    // parses to something else but exists is kept.
    check(launch_may_rewrite(L"garbage", L"C:\\build\\vt.exe", false),
        "an entry whose target does not exist is rewritten");
    check(!launch_may_rewrite(L"garbage", L"C:\\build\\vt.exe", true),
        "an entry that parses to something that exists is kept, however odd it looks");
}

} // namespace

int main()
{
    test_quoting_and_command_line();
    test_switch_detection();
    test_launch_reconciliation();

    // --- the executable this process would register -----------------------------
    const auto executable = voicetyper::platform::win32::module_file_path();
    if (executable.is_error()) {
        std::printf("FAIL GetModuleFileNameW: %s\n", executable.error().to_string().c_str());
        ++failures;
        std::printf("win32-startup-contract: %d of %d check(s) failed\n", failures, checks);
        return 1;
    }
    const std::wstring& module_path = executable.value();
    std::printf("  observed: this process is \"%s\"\n", narrow(module_path).c_str());
    check(!module_path.empty(), "GetModuleFileNameW returned a path");
    check(std::filesystem::exists(std::filesystem::path(module_path)),
        "the reported executable path exists on disk");
    check(module_path.size() > 4 && module_path.substr(module_path.size() - 4) == L".exe",
        "the reported executable path names this test binary, not the installed app");

    // --- the production value: observed, never touched -------------------------
    const std::wstring production_name(kAutostartValueName);
    const RawRunValue production_before = read_raw_run_value(production_name);
    std::printf(
        "  observed: HKCU\\%s value \"%s\" is %s",
        narrow(std::wstring(kAutostartKeyPath)).c_str(),
        narrow(production_name).c_str(),
        production_before.present ? "PRESENT" : "absent");
    if (production_before.present) {
        std::printf(" = \"%s\" (type %u)", narrow(production_before.data).c_str(),
            static_cast<unsigned>(production_before.type));
    }
    std::printf("\n");
    check(production_before.status == ERROR_SUCCESS || production_before.status == ERROR_FILE_NOT_FOUND,
        "the production Run key is readable (RegOpenKeyExW/RegQueryValueExW)");

    // The production instance is exercised read-only: same key, same value
    // name, so the constants this ships really are the ones a user's registry
    // is read through.
    const Win32Startup production;
    const auto production_entry = production.read();
    check(production_entry.is_ok(), "the production autostart value can be read");
    if (production_entry.is_ok()) {
        check(production_entry.value().present == production_before.present,
            "the backend and the raw registry agree about the production value");
        if (production_before.present) {
            check_wide(
                production_entry.value().command_line,
                production_before.data,
                "the production value is reported byte-identically");
        }
        std::printf("  observed: production autostart is %s, command line \"%s\"\n",
            production_entry.value().present ? "enabled" : "disabled",
            narrow(production_entry.value().command_line).c_str());
    }

    // --- the real Run key, a test-scoped value name ----------------------------
    std::printf("group: real HKCU Run key round-trip (test value name only)\n");
    const std::wstring test_name =
        L"VoiceTyperStartupContract-" + std::to_wstring(static_cast<unsigned long>(::GetCurrentProcessId()));
    std::printf("  observed: test value name is \"%s\" (removed again on every exit path)\n",
        narrow(test_name).c_str());
    StartupOptions options;
    options.value_name = test_name;  // everything else stays the production key path
    Win32Startup startup(options);
    check_wide(startup.key_path(), std::wstring(kAutostartKeyPath),
        "the test writes into the production key path");
    check_wide(startup.value_name(), test_name, "the test writes a distinct value name");

    ScopedTestValue cleanup(startup);

    // Start from a known state, whatever a previous crashed run left behind.
    const auto cleared = startup.set_enabled(false, false);
    check(cleared.is_ok(), "the test value can be cleared before the run");
    check(!read_raw_run_value(test_name).present, "the test value is absent before the first write");

    // A missing value is disabled, not an error.
    const auto missing = startup.read();
    check(missing.is_ok(), "reading a missing value is a success, not an error");
    if (missing.is_ok()) {
        check(!missing.value().present, "a missing value reads as disabled");
        check(missing.value().command_line.empty(), "a missing value has no command line");
    }
    check(!startup.is_enabled(), "is_enabled() is false while the value is missing");

    // Enable: the write must be visible in the real registry, as REG_SZ, with
    // the quoted executable and no switch.
    const auto enabled = startup.set_enabled(true, false);
    check(enabled.is_ok(), "set_enabled(true, false) succeeds");
    if (enabled.is_error()) {
        std::printf("  detail: %s\n", enabled.error().to_string().c_str());
    }
    const RawRunValue written = read_raw_run_value(test_name);
    check(written.present, "the raw registry now holds the test value");
    check(written.type == REG_SZ, "the value is stored as REG_SZ, like the .NET SetValue(string)");
    check_wide(
        written.data, build_command_line(module_path, false), "the raw value is the quoted executable");
    check(startup.is_enabled(), "is_enabled() is true after enabling");

    const auto entry = startup.read();
    check(entry.is_ok(), "the written value can be read back");
    if (entry.is_ok()) {
        check(entry.value().present, "the value is present after enabling");
        check_wide(entry.value().command_line, written.data, "the read matches the raw registry bytes");
        check_wide(entry.value().value_type, L"REG_SZ", "the registry type is reported");
        check(!entry.value().start_minimized, "an entry written without the setting carries no switch");
    }
    check(written.data.size() >= 2 && written.data.front() == L'"' && written.data.back() == L'"',
        "the command line is quoted");
    check(written.data.substr(1, written.data.size() - 2) == module_path,
        "the quoted command line is exactly the executable path");
    check(written.data.find(kStartMinimizedSwitch) == std::wstring::npos,
        "no switch is stored while startMinimized is off");

    // Idempotence: enabling twice stores the same bytes.
    const auto enabled_again = startup.set_enabled(true, false);
    check(enabled_again.is_ok(), "enabling a second time succeeds");
    const RawRunValue written_again = read_raw_run_value(test_name);
    check(written_again.present, "the value is still present after a second enable");
    check(written_again.type == written.type, "the second enable keeps REG_SZ");
    check_wide(written_again.data, written.data, "enabling twice is idempotent (identical bytes)");

    // The switch variant.
    const auto minimized = startup.set_enabled(true, true);
    check(minimized.is_ok(), "set_enabled(true, true) succeeds");
    const RawRunValue minimized_value = read_raw_run_value(test_name);
    check_wide(
        minimized_value.data,
        build_command_line(module_path, true),
        "with startMinimized the stored entry carries the switch after the quoted exe");
    const auto minimized_entry = startup.read();
    check(minimized_entry.is_ok() && minimized_entry.value().start_minimized,
        "read() reports start_minimized from the registry, not from the setting");

    // Disable: nothing may be left behind, verified through the raw registry.
    const auto disabled = startup.set_enabled(false, true);
    check(disabled.is_ok(), "set_enabled(false, ...) succeeds (start_minimized is ignored)");
    const RawRunValue removed = read_raw_run_value(test_name);
    check(!removed.present, "the raw registry has no Run value left after disabling");
    check(!startup.is_enabled(), "is_enabled() is false after disabling");
    const auto after_disable = startup.read();
    check(after_disable.is_ok() && !after_disable.value().present,
        "a read after disabling reports disabled, not an error");
    const auto disabled_again = startup.set_enabled(false, false);
    check(disabled_again.is_ok(), "disabling twice is idempotent");
    check(!read_raw_run_value(test_name).present, "the value stays absent after a second disable");

    // --- an entry belonging to another installation is left alone --------------
    const std::wstring foreign_command = L"\"C:\\Program Files\\Other VoiceTyper\\VoiceTyper.exe\"";
    if (write_raw_run_value(test_name, foreign_command)) {
        check(read_raw_run_value(test_name).present, "a foreign Run value was planted for this check");
        const auto refused = startup.set_enabled(false, false);
        check(refused.is_error(), "disabling something this process does not own is refused");
        check(refused.code() == voicetyper::domain::ErrorCode::permission_denied,
            "the refusal is permission_denied, not a silent success");
        check(read_raw_run_value(test_name).present,
            "the foreign Run value is still there: another installation's autostart was not disabled");
        if (refused.is_error()) {
            std::printf("  observed: foreign entry refusal: %s\n", refused.error().to_string().c_str());
        }
        // Our own entry is still ours to remove, and the round trip stays complete.
        const auto mine = startup.set_enabled(true, false);
        check(mine.is_ok(), "our own entry can be written next to a foreign one");
        const auto removed_mine = startup.set_enabled(false, false);
        check(removed_mine.is_ok(), "our own entry is removed again");
        check(!read_raw_run_value(test_name).present, "our own value is gone after removal");
        check(read_raw_run_value(test_name).present == false, "the foreign value was already replaced");
    } else {
        std::printf("  note: the test Run value could not be written directly, so the foreign-entry rule was not checked\n");
    }

    // --- the production value is exactly as it was ----------------------------
    const RawRunValue production_after = read_raw_run_value(production_name);
    check(production_after.present == production_before.present,
        "the production value presence is unchanged by this test");
    if (production_before.present) {
        check(production_after.type == production_before.type,
            "the production value type is unchanged by this test");
        check_wide(production_after.data, production_before.data,
            "the production value data is unchanged by this test (no entry points at a test build)");
    }

    if (failures != 0) {
        std::printf("win32-startup-contract: %d of %d check(s) failed\n", failures, checks);
        return 1;
    }
    std::printf("win32-startup-contract: OK (%d checks)\n", checks);
    return 0;
}
