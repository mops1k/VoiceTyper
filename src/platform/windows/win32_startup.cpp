#include "platform/windows/win32_startup.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>

namespace voicetyper::platform::win32 {
namespace {

/// Upper bound on the data a single Run value may hold. A real entry is well
/// under 500 bytes; anything larger is either a corrupt value or a value
/// something else wrote, and neither should be copied into our own entry.
constexpr DWORD kMaxRunValueBytes = 64u * 1024u;

/// Upper bound on the executable path buffer. MAX_PATH is 260, the long-path
/// limit is 32767 wide characters; the growth below stops at the latter, so a
/// pathological GetModuleFileNameW can never spin.
constexpr DWORD kMaxModulePathChars = 32768u;

ErrorCode registry_error_code(LSTATUS status)
{
    switch (status) {
    case ERROR_SUCCESS:
        return ErrorCode::ok;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        // Not an error for this backend: an absent value means disabled.
        return ErrorCode::not_found;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return ErrorCode::permission_denied;
    case ERROR_NOT_ENOUGH_MEMORY:
        return ErrorCode::resource_exhausted;
    default:
        return ErrorCode::io_failure;
    }
}

/// "<what>: LSTATUS=<n>". The portable Status/Result factories take a message
/// and no separate detail field, so the fact that makes a registry failure
/// diagnosable travels inside the message text - and it names the exact call
/// that refused, which is what a bare "access denied" never tells a reader.
std::string status_message(LSTATUS status, std::string_view message, std::string_view what)
{
    std::string text(message);
    text += " (";
    text += std::string(what);
    text += ": LSTATUS=";
    text += std::to_string(static_cast<unsigned long>(status));
    text += ')';
    return text;
}

/// Owns one HKEY and closes it on every path. The .NET `using` statement had
/// the same guarantee; doing it by hand is what keeps an early return from
/// leaking a handle into a long-running app.
class KeyHandle final {
public:
    KeyHandle() = default;
    ~KeyHandle()
    {
        if (handle_ != nullptr) {
            ::RegCloseKey(handle_);
        }
    }

    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;

    [[nodiscard]] HKEY get() const noexcept { return handle_; }
    HKEY* put() noexcept { return &handle_; }

private:
    HKEY handle_ = nullptr;
};

std::wstring registry_type_name(DWORD type)
{
    switch (type) {
    case REG_SZ: return L"REG_SZ";
    case REG_EXPAND_SZ: return L"REG_EXPAND_SZ";
    case REG_MULTI_SZ: return L"REG_MULTI_SZ";
    case REG_DWORD: return L"REG_DWORD";
    case REG_QWORD: return L"REG_QWORD";
    case REG_BINARY: return L"REG_BINARY";
    default: break;
    }
    return L"REG_TYPE_" + std::to_wstring(static_cast<unsigned long>(type));
}

} // namespace

std::wstring quote_argument(std::wstring_view argument)
{
    // The CommandLineToArgvW rules, which are the rules CreateProcess uses for
    // argv[0]: everything is wrapped in double quotes, an embedded quote is
    // escaped with a backslash, and every run of backslashes that would touch
    // a quote is doubled. A path with no quote and no trailing backslash - the
    // normal case - reduces to the two-literal-quote form the .NET build wrote.
    std::wstring quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');
    std::size_t pending_backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++pending_backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(pending_backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            pending_backslashes = 0;
            continue;
        }
        quoted.append(pending_backslashes, L'\\');
        pending_backslashes = 0;
        quoted.push_back(ch);
    }
    // Backslashes immediately before the closing quote must be doubled or the
    // quote itself would be escaped.
    quoted.append(pending_backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring build_command_line(std::wstring_view executable, bool start_minimized)
{
    if (executable.empty()) {
        // An entry pointing at nothing would start nothing and look enabled.
        return {};
    }
    std::wstring command = quote_argument(executable);
    if (start_minimized) {
        command.push_back(L' ');
        command.append(kStartMinimizedSwitch);
    }
    return command;
}

bool carries_start_minimized(std::wstring_view command_line)
{
    return command_line.find(kStartMinimizedSwitch) != std::wstring_view::npos;
}

bool is_start_minimized_switch(std::string_view argument)
{
    // A switch always has a dash. A bare word is a file name or a value, and
    // treating it as the switch would make a path argument start the app
    // hidden.
    if (argument.size() < 2 || argument.front() != '-') {
        return false;
    }
    std::string_view name = argument;
    name.remove_prefix(1);
    if (name.front() == '-') {
        name.remove_prefix(1);
    }
    // The stored switch is "--start-minimized"; the compared part is the name
    // after the one or two leading dashes, so the length must be the stored
    // length minus exactly the two dash characters. (Comparing against
    // size() - 1 here is an off-by-one that rejects the switch it defines -
    // caught by the very contract test that pins this function.)
    constexpr std::size_t kDashPrefixLength = 2;
    if (name.size() + kDashPrefixLength != kStartMinimizedSwitch.size()) {
        return false;
    }
    for (std::size_t index = 0; index < name.size(); ++index) {
        const auto left = static_cast<unsigned char>(name[index]);
        const auto right = static_cast<unsigned char>(kStartMinimizedSwitch[index + kDashPrefixLength]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

/// Windows paths are case-insensitive, and the two spellings of the same install
/// differ only in case in practice.
bool paths_equal(std::wstring_view left, std::wstring_view right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

std::wstring command_line_executable(std::wstring_view command_line)
{
    std::size_t position = 0;
    while (position < command_line.size()
        && (command_line[position] == L' ' || command_line[position] == L'\t')) {
        ++position;
    }

    // The writer always quotes the executable, so the quoted case is the one
    // that matters. Backslashes are only special in front of a quote: an even run
    // leaves them literal and the quote closes the argument, an odd run means
    // half the backslashes plus a literal quote.
    if (position < command_line.size() && command_line[position] == L'"') {
        ++position;
        std::wstring result;
        std::size_t backslashes = 0;
        while (position < command_line.size()) {
            const wchar_t character = command_line[position];
            if (character == L'\\') {
                ++backslashes;
                ++position;
                continue;
            }
            if (character == L'"') {
                if (backslashes % 2 == 1) {
                    result.append(backslashes / 2, L'\\');
                    result.push_back(L'"');
                    backslashes = 0;
                    ++position;
                    continue;
                }
                // 2n backslashes before the closing quote mean n literal
                // backslashes (n=1 for a path that ends in a separator: the
                // writer doubled it so the quote still closes the argument).
                result.append(backslashes / 2, L'\\');
                return result;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
            ++position;
        }
        // Unterminated quote: best effort, never an empty string for a real path.
        result.append(backslashes, L'\\');
        return result;
    }

    std::size_t end = position;
    while (end < command_line.size() && command_line[end] != L' ' && command_line[end] != L'\t') {
        ++end;
    }
    return std::wstring(command_line.substr(position, end - position));
}

namespace {

/// Windows paths are case-insensitive; comparing the strings is enough here
/// because both sides come from the same kind of source (the registry and
/// GetModuleFileNameW) and never carry a trailing separator.
bool same_executable(std::wstring_view left, std::wstring_view right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        wchar_t a = left[index];
        wchar_t b = right[index];
        if (a >= L'A' && a <= L'Z') {
            a = static_cast<wchar_t>(a - L'A' + L'a');
        }
        if (b >= L'A' && b <= L'Z') {
            b = static_cast<wchar_t>(b - L'A' + L'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

} // namespace

bool launch_may_rewrite(
    std::wstring_view stored_command_line, std::wstring_view this_executable, bool target_exists)
{
    const std::wstring stored = command_line_executable(stored_command_line);
    if (stored.empty()) {
        return true; // nothing usable is registered
    }
    if (same_executable(stored, this_executable)) {
        return true; // our own entry: refreshing the switch is the point
    }
    return !target_exists; // another installation: only a dead target is repaired
}

bool wants_start_minimized(int argc, char** argv)
{    if (argv == nullptr) {
        return false;
    }
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && is_start_minimized_switch(argv[index])) {
            return true;
        }
    }
    return false;
}

Result<std::wstring> module_file_path()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 8 && buffer.size() < kMaxModulePathChars; ++attempt) {
        const auto copied = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            const auto last = ::GetLastError();
            return Result<std::wstring>::failure(
                registry_error_code(static_cast<LSTATUS>(last)),
                status_message(static_cast<LSTATUS>(last),
                    "GetModuleFileNameW failed for this process", "GetModuleFileNameW"));
        }
        if (copied < buffer.size()) {
            // Less than the buffer means the whole path was written and the
            // result is NUL terminated; the size is the length without the NUL.
            buffer.resize(copied);
            return buffer;
        }
        // Truncated (or exactly full, which Windows uses as the "it did not
        // fit" answer): grow and ask again. Bounded by kMaxModulePathChars.
        buffer.resize(buffer.size() * 2);
    }
    return Result<std::wstring>::failure(
        ErrorCode::resource_exhausted,
        "the executable path did not fit in a bounded buffer");
}

std::string to_log_text(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<std::size_t>(INT_MAX)) {
        return "<path too long to log>";
    }
    const int required = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        // A log line must never be the reason a failure is unreportable.
        return "<unprintable>";
    }
    std::string narrow(static_cast<std::size_t>(required), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), required, nullptr, nullptr);
    if (written != required) {
        return "<unprintable>";
    }
    return narrow;
}

Win32Startup::Win32Startup()
    : Win32Startup(StartupOptions{})
{
}

Win32Startup::Win32Startup(StartupOptions options)
    : options_(std::move(options))
{
}

Result<StartupEntry> Win32Startup::read() const
{
    if (options_.key_path.empty() || options_.value_name.empty()) {
        return Result<StartupEntry>::failure(
            ErrorCode::invalid_argument, "the autostart key path and value name must not be empty");
    }

    KeyHandle key;
    const LSTATUS opened = ::RegOpenKeyExW(
        HKEY_CURRENT_USER, options_.key_path.c_str(), 0, KEY_QUERY_VALUE, key.put());
    if (opened == ERROR_FILE_NOT_FOUND) {
        // A profile without the Run key behaves exactly like one without our
        // value: nothing will autostart. That is `present == false`, not a
        // failure, and it is what the .NET `key is null` branch produced.
        return StartupEntry{};
    }
    if (opened != ERROR_SUCCESS) {
        return Result<StartupEntry>::failure(
            registry_error_code(opened),
            status_message(opened, "the autostart value could not be read", "RegOpenKeyExW/KEY_QUERY_VALUE"));
    }

    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS queried = ::RegQueryValueExW(
        key.get(), options_.value_name.c_str(), nullptr, &type, nullptr, &bytes);
    if (queried == ERROR_FILE_NOT_FOUND) {
        return StartupEntry{};
    }
    if (queried != ERROR_SUCCESS) {
        return Result<StartupEntry>::failure(
            registry_error_code(queried),
            status_message(queried, "the autostart value could not be queried", "RegQueryValueExW/size"));
    }
    if (bytes > kMaxRunValueBytes) {
        return Result<StartupEntry>::failure(
            ErrorCode::corrupt_data,
            "the autostart value is larger than this backend will read (bytes="
                + std::to_string(static_cast<unsigned long>(bytes)) + ')');
    }

    // One extra wchar_t so the buffer is always NUL terminated even if Windows
    // ever hands back unterminated data.
    std::wstring data(static_cast<std::size_t>(bytes) / sizeof(wchar_t) + 1, L'\0');
    queried = ::RegQueryValueExW(
        key.get(),
        options_.value_name.c_str(),
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(data.data()),
        &bytes);
    if (queried == ERROR_SUCCESS) {
        const auto terminator = data.find(L'\0');
        if (terminator != std::wstring::npos) {
            data.resize(terminator);
        }
    } else if (queried != ERROR_FILE_NOT_FOUND) {
        return Result<StartupEntry>::failure(
            registry_error_code(queried),
            status_message(queried, "the autostart value could not be read", "RegQueryValueExW/data"));
    } else {
        return StartupEntry{};
    }

    StartupEntry entry;
    entry.present = true;
    entry.command_line = data;
    entry.value_type = registry_type_name(type);
    entry.start_minimized = carries_start_minimized(entry.command_line);
    return entry;
}

bool Win32Startup::is_enabled() const
{
    const auto entry = read();
    // The .NET method had exactly one answer for every outcome, and existence
    // is the whole question. read() is there for callers that need the reason.
    return entry.is_ok() && entry.value().present;
}

Result<std::wstring> Win32Startup::executable_path() const
{
    if (options_.executable_path.has_value()) {
        if (options_.executable_path->empty()) {
            return Result<std::wstring>::failure(
                ErrorCode::invalid_argument, "the pinned autostart executable path is empty");
        }
        return *options_.executable_path;
    }
    return module_file_path();
}

Status Win32Startup::set_enabled(bool enabled, bool start_minimized) const
{
    if (options_.key_path.empty() || options_.value_name.empty()) {
        return Status::failure(
            ErrorCode::invalid_argument, "the autostart key path and value name must not be empty");
    }
    if (!enabled) {
        return remove_entry();
    }

    const auto executable = executable_path();
    if (executable.is_error()) {
        return Status::failure(
            executable.error().code(),
            "the autostart entry cannot name an executable: " + executable.error().message());
    }
    const std::wstring command = build_command_line(executable.value(), start_minimized);
    if (command.empty()) {
        return Status::failure(
            ErrorCode::invalid_argument, "the autostart command line is empty");
    }

    KeyHandle key;
    LSTATUS opened = ::RegOpenKeyExW(
        HKEY_CURRENT_USER, options_.key_path.c_str(), 0, KEY_SET_VALUE, key.put());
    if (opened == ERROR_FILE_NOT_FOUND) {
        // Intent-parity decision: create the key. The .NET code opened it
        // read/write and returned silently when it was absent, which turned
        // the checkbox into a no-op on a profile whose Run key had been
        // removed. Creating it here costs one HKCU key and makes the user's
        // explicit choice work; it never needs elevation.
        DWORD disposition = 0;
        opened = ::RegCreateKeyExW(
            HKEY_CURRENT_USER,
            options_.key_path.c_str(),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            key.put(),
            &disposition);
        if (opened != ERROR_SUCCESS) {
            return Status::failure(
                registry_error_code(opened),
                status_message(opened, "the autostart key could not be created", "RegCreateKeyExW/KEY_SET_VALUE"));
        }
    } else if (opened != ERROR_SUCCESS) {
        return Status::failure(
            registry_error_code(opened),
            status_message(opened, "the autostart value could not be opened for writing", "RegOpenKeyExW/KEY_SET_VALUE"));
    }

    const auto bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    const LSTATUS written = ::RegSetValueExW(
        key.get(),
        options_.value_name.c_str(),
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        bytes);
    if (written != ERROR_SUCCESS) {
        return Status::failure(
            registry_error_code(written),
            status_message(written, "the autostart value could not be written", "RegSetValueExW/REG_SZ"));
    }
    return Status::success();
}

Status Win32Startup::remove_entry() const
{
    if (options_.key_path.empty() || options_.value_name.empty()) {
        return Status::failure(
            ErrorCode::invalid_argument, "the autostart key path and value name must not be empty");
    }

    KeyHandle key;
    const LSTATUS opened = ::RegOpenKeyExW(
        HKEY_CURRENT_USER, options_.key_path.c_str(), 0, KEY_SET_VALUE, key.put());
    if (opened == ERROR_FILE_NOT_FOUND) {
        // Nothing to remove, and the user asked for nothing to autostart.
        return Status::success();
    }
    if (opened != ERROR_SUCCESS) {
        return Status::failure(
            registry_error_code(opened),
            status_message(opened, "the autostart value could not be opened for removal", "RegOpenKeyExW/KEY_SET_VALUE"));
    }

    // Only an entry that names *this* executable is ours to remove. The value name
    // is shared with the installed build of the product, and deleting it blindly
    // disabled autostart for an installation this process does not own - measured
    // 2026-10-01: the C++ build, run with startWithWindows=false, removed the
    // installed .NET application's Run value.
    const auto current = read();
    if (current.is_ok() && current.value().present) {
        const std::wstring stored = command_line_executable(current.value().command_line);
        const auto own = executable_path();
        if (own.is_ok() && !stored.empty() && !paths_equal(stored, own.value())) {
            return Status::failure(
                ErrorCode::permission_denied,
                "the autostart entry names another installation and was left alone: "
                    + std::string("it points at an executable this process did not register"));
        }
    }

    const LSTATUS deleted = ::RegDeleteValueW(key.get(), options_.value_name.c_str());
    if (deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND) {
        // The second case is what makes disabling idempotent: a value that is
        // already gone is the state the caller asked for.
        return Status::success();
    }
    return Status::failure(
        registry_error_code(deleted),
        status_message(deleted, "the autostart value could not be removed", "RegDeleteValueW"));
}

} // namespace voicetyper::platform::win32
