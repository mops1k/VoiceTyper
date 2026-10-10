#include "platform/linux/linux_startup.hpp"

#include "platform/linux/linux_paths.hpp"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace voicetyper::platform::linuxos {
namespace {

std::optional<std::string> process_environment(std::string_view name)
{
    const std::string key(name);
    const char* value = std::getenv(key.c_str());
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

Status errno_status(const char* what)
{
    const auto code = std::error_code(errno, std::generic_category());
    if (code.value() == EACCES || code.value() == EPERM || code.value() == EROFS) {
        return Status::failure(ErrorCode::permission_denied, std::string(what) + ": " + code.message());
    }
    if (code.value() == ENOENT) {
        return Status::failure(ErrorCode::not_found, std::string(what) + ": " + code.message());
    }
    return Status::failure(ErrorCode::io_failure, std::string(what) + ": " + code.message());
}

std::string trim(std::string_view text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(begin, end - begin + 1));
}

/// Whether the argument can be stored in an Exec value as it is. Anything with
/// whitespace, a quote, a backslash or a dollar sign has to be quoted, because
/// those are the characters the desktop-entry parser treats specially.
bool needs_quoting(std::string_view argument)
{
    return argument.find_first_of(" \t\n\"'\\$`") != std::string_view::npos;
}

/// The first argument of an Exec value, with its quoting resolved. Used for the
/// "which executable does this entry name" question; the rest of the command
/// line is irrelevant to it.
std::string first_argument(std::string_view command_line)
{
    const std::string trimmed = trim(command_line);
    if (trimmed.empty()) {
        return {};
    }
    if (trimmed.front() != '"') {
        const auto end = trimmed.find_first_of(" \t");
        return trimmed.substr(0, end);
    }

    std::string result;
    bool closed = false;
    for (std::size_t index = 1; index < trimmed.size(); ++index) {
        const char current = trimmed[index];
        if (current == '\\' && index + 1 < trimmed.size()) {
            // Inside double quotes the freedesktop rules allow \" \\ \$ \` and
            // require the backslash to be dropped for those; any other backslash
            // is kept as it is.
            const char next = trimmed[index + 1];
            if (next == '"' || next == '\\' || next == '$' || next == '`') {
                result.push_back(next);
                ++index;
                continue;
            }
            result.push_back(current);
            continue;
        }
        if (current == '"') {
            closed = true;
            break;
        }
        result.push_back(current);
    }
    if (!closed) {
        // An unterminated quote is not a usable entry; reporting it as "no
        // executable" lets the caller repair the entry instead of trusting a
        // truncated path.
        return {};
    }
    return result;
}

} // namespace

std::filesystem::path default_autostart_directory(const domain::EnvironmentLookup& environment)
{
    std::filesystem::path config;
    if (environment) {
        if (const auto xdg = environment("XDG_CONFIG_HOME"); xdg.has_value() && !xdg->empty()) {
            config = *xdg;
        } else if (const auto home = environment("HOME"); home.has_value() && !home->empty()) {
            config = std::filesystem::path(*home) / ".config";
        }
    }
    if (config.empty()) {
        return {};
    }
    return config / "autostart";
}

std::string quote_exec_argument(std::string_view argument)
{
    if (argument.empty()) {
        return {};
    }
    if (!needs_quoting(argument)) {
        return std::string(argument);
    }
    std::string quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back('"');
    for (const char current : argument) {
        if (current == '"' || current == '\\' || current == '$' || current == '`') {
            quoted.push_back('\\');
        }
        quoted.push_back(current);
    }
    quoted.push_back('"');
    return quoted;
}

std::string build_desktop_entry(std::string_view executable, bool start_minimized)
{
    if (executable.empty()) {
        return {};
    }
    std::string text;
    text += "[Desktop Entry]\n";
    text += "Type=Application\n";
    text += "Version=1.0\n";
    text += "Name=VoiceTyper\n";
    text += "Comment=Voice to text on a global hotkey\n";
    text += "Exec=";
    text += quote_exec_argument(executable);
    if (start_minimized) {
        text += ' ';
        text += kStartMinimizedSwitch;
    }
    text += "\n";
    text += "Terminal=false\n";
    // The key GNOME and KDE both honour; a desktop environment that ignores it
    // still reads the entry itself, so this only adds the "user enabled it"
    // signal.
    text += "X-GNOME-Autostart-enabled=true\n";
    return text;
}

std::string desktop_entry_command_line(std::string_view text)
{
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const auto line_end = text.find('\n', offset);
        const std::string_view line = text.substr(offset,
            line_end == std::string_view::npos ? std::string_view::npos : line_end - offset);
        if (line.rfind("Exec=", 0) == 0) {
            const std::string value = trim(line.substr(5));
            if (value.empty()) {
                return {};
            }
            // The value is one or more arguments; the stored command line is
            // reported with the first argument unquoted and the rest verbatim,
            // which is what the writer produced and what the launch parser reads.
            const std::string first = first_argument(value);
            if (first.empty()) {
                return {};
            }
            std::string rest = trim(value.substr(value.find(first) + first.size()));
            if (rest.empty()) {
                return first;
            }
            return first + " " + rest;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        offset = line_end + 1;
    }
    return {};
}

bool carries_start_minimized(std::string_view command_line)
{
    return command_line.find(kStartMinimizedSwitch) != std::string_view::npos;
}

std::string command_line_executable(std::string_view command_line)
{
    return first_argument(command_line);
}

bool is_start_minimized_switch(std::string_view argument)
{
    if (argument.size() < 2 || argument.front() != '-') {
        return false;
    }
    std::string_view name = argument.substr(1);
    if (!name.empty() && name.front() == '-') {
        name = name.substr(1);
    }
    return name == "start-minimized";
}

bool launch_may_rewrite(std::string_view stored_command_line,
    std::string_view this_executable, bool target_exists)
{
    if (stored_command_line.empty()) {
        return true;
    }
    const std::string stored_executable = first_argument(stored_command_line);
    if (stored_executable.empty()) {
        // Unparsable: repairing it is the cutover case.
        return true;
    }
    if (stored_executable == this_executable) {
        return true;
    }
    // An entry naming another executable that still exists belongs to another
    // installation and is left alone.
    return !target_exists;
}

bool wants_start_minimized(int argc, char** argv)
{
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && is_start_minimized_switch(argv[index])) {
            return true;
        }
    }
    return false;
}

LinuxStartup::LinuxStartup()
    : LinuxStartup(StartupOptions{
          default_autostart_directory(&process_environment),
          std::string(kAutostartFileName),
          std::nullopt,
      })
{
}

LinuxStartup::LinuxStartup(StartupOptions options)
    : options_(std::move(options))
{
    if (!options_.autostart_directory.empty() && !options_.file_name.empty()) {
        file_path_ = options_.autostart_directory / std::filesystem::path(options_.file_name);
    }
}

Result<StartupEntry> LinuxStartup::read() const
{
    if (file_path_.empty()) {
        return Result<StartupEntry>::failure(ErrorCode::invalid_argument,
            "no autostart directory: neither XDG_CONFIG_HOME nor HOME is set");
    }

    std::error_code exists_error;
    const bool exists = std::filesystem::exists(file_path_, exists_error);
    if (exists_error) {
        return Result<StartupEntry>::failure(ErrorCode::io_failure,
            "cannot stat " + file_path_.string() + ": " + exists_error.message());
    }
    if (!exists) {
        return StartupEntry{};
    }

    std::ifstream input(file_path_, std::ios::binary);
    if (!input) {
        return Result<StartupEntry>::failure(ErrorCode::permission_denied,
            "cannot read " + file_path_.string());
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    StartupEntry entry;
    entry.present = true;
    entry.file_path = file_path_;
    entry.command_line = desktop_entry_command_line(text);
    entry.start_minimized = carries_start_minimized(entry.command_line);
    return entry;
}

bool LinuxStartup::is_enabled() const
{
    const auto entry = read();
    return entry.is_ok() && entry.value().present;
}

Result<std::filesystem::path> LinuxStartup::executable_path() const
{
    if (options_.executable_path.has_value() && !options_.executable_path->empty()) {
        return *options_.executable_path;
    }
    const auto own = executable_file_path();
    if (own.empty()) {
        return Result<std::filesystem::path>::failure(ErrorCode::not_found,
            "cannot resolve the running executable (/proc/self/exe)");
    }
    return own;
}

Status LinuxStartup::set_enabled(bool enabled, bool start_minimized) const
{
    if (!enabled) {
        return remove_entry();
    }
    if (file_path_.empty()) {
        return Status::failure(ErrorCode::invalid_argument,
            "no autostart directory: neither XDG_CONFIG_HOME nor HOME is set");
    }

    const auto executable = executable_path();
    if (executable.is_error()) {
        return Status::failure(executable.error());
    }
    const std::string text = build_desktop_entry(executable.value().string(), start_minimized);
    if (text.empty()) {
        return Status::failure(ErrorCode::invalid_argument, "an empty executable cannot be registered");
    }

    std::error_code directory_error;
    std::filesystem::create_directories(options_.autostart_directory, directory_error);
    if (directory_error) {
        return Status::failure(ErrorCode::io_failure,
            "cannot create " + options_.autostart_directory.string() + ": " + directory_error.message());
    }

    // Written through a temporary file and renamed, so a crash in the middle
    // cannot leave a truncated entry the session would try to execute.
    const std::filesystem::path temporary = file_path_.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return errno_status("cannot write the autostart entry");
        }
        output << text;
        output.flush();
        if (!output) {
            static_cast<void>(std::filesystem::remove(temporary, directory_error));
            return errno_status("cannot write the autostart entry");
        }
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary, file_path_, rename_error);
    if (rename_error) {
        static_cast<void>(std::filesystem::remove(temporary, directory_error));
        return Status::failure(ErrorCode::io_failure,
            "cannot replace " + file_path_.string() + ": " + rename_error.message());
    }
    return Status::success();
}

Status LinuxStartup::remove_entry() const
{
    if (file_path_.empty()) {
        return Status::failure(ErrorCode::invalid_argument,
            "no autostart directory: neither XDG_CONFIG_HOME nor HOME is set");
    }
    std::error_code error;
    const bool removed = std::filesystem::remove(file_path_, error);
    if (error) {
        return Status::failure(ErrorCode::io_failure,
            "cannot remove " + file_path_.string() + ": " + error.message());
    }
    // An absent entry is the state the caller asked for.
    static_cast<void>(removed);
    return Status::success();
}

} // namespace voicetyper::platform::linuxos
