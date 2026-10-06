#include "platform/windows/win32_logger.hpp"

#include "domain/file_logger.hpp"
#include "platform/windows/win32_clock.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace voicetyper::platform {

std::string format_log_timestamp(std::chrono::system_clock::time_point moment)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(moment);
    // Local time, not UTC: compatibility-contracts.md §7 says the .NET build
    // uses DateTime.Now, and a support bundle is read in the user's own zone.
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(moment.time_since_epoch()) % 1000;

    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &seconds) != 0) {
        // A broken local time zone must not stop logging. Falling back to UTC is
        // a visible, greppable value rather than a silent empty field; a tm left
        // zeroed would print a 1900 timestamp, which is obviously wrong to a
        // support reader.
        if (gmtime_s(&local, &seconds) != 0) {
            local = std::tm{};
        }
    }
#else
    localtime_r(&seconds, &local);
#endif

    std::ostringstream text;
    text << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setfill('0') << std::setw(3) << milliseconds.count();
    return text.str();
}

std::string error_code_name_of(ErrorCode code)
{
    const std::string_view name = domain::error_code_name(code);
    if (name.empty() || name == "unknown") {
        // A declared code always has a stable name; reaching this branch means
        // ErrorCode gained a value without a name, which is a VoiceTyper defect.
        // The literal keeps the line parseable instead of writing nothing.
        return "invalid_state";
    }
    return std::string(name);
}

Win32Logger::Win32Logger(std::filesystem::path log_directory, LoggerOptions options)
    : Win32Logger(
        std::move(log_directory),
        std::make_unique<Win32Clock>(),
        nullptr,
        std::move(options))
{
}

Win32Logger::Win32Logger(
    std::filesystem::path log_directory, const Clock& clock, LoggerOptions options)
    : Win32Logger(std::move(log_directory), nullptr, &clock, std::move(options))
{
}

Win32Logger::Win32Logger(
    std::filesystem::path log_directory,
    std::unique_ptr<Clock> owned_clock,
    const Clock* borrowed_clock,
    LoggerOptions options)
    : owned_clock_(std::move(owned_clock))
    , clock_(borrowed_clock != nullptr ? borrowed_clock : owned_clock_.get())
{
    if (clock_ == nullptr) {
        // Unreachable: one of the two is always set. Falling back to an owned
        // clock keeps the "never null" promise even if a future overload forgets.
        owned_clock_ = std::make_unique<Win32Clock>();
        clock_ = owned_clock_.get();
    }

    const Clock* source = clock_;
    inner_ = std::make_unique<domain::FileLogger>(
        std::move(log_directory),
        std::move(options),
        [source] { return format_log_timestamp(source->wall_now()); },
        kWin32LogLineEnding);
}

Win32Logger::~Win32Logger() = default;

std::unique_ptr<Win32Logger> Win32Logger::for_paths(
    const domain::AppPaths& paths, LoggerOptions options, const Clock* clock)
{
    if (clock != nullptr) {
        return std::make_unique<Win32Logger>(paths.logs_directory(), *clock, std::move(options));
    }
    return std::make_unique<Win32Logger>(paths.logs_directory(), std::move(options));
}

const Clock& Win32Logger::clock() const noexcept
{
    return *clock_;
}

std::string Win32Logger::log_directory() const
{
    return inner_->log_directory();
}

std::string Win32Logger::log_file_path() const
{
    return inner_->log_file_path();
}

Status Win32Logger::clear()
{
    return inner_->clear();
}

Status Win32Logger::write(LogLevel level, std::string_view message, std::string_view detail)
{
    return inner_->write(level, message, detail);
}

Result<std::string> Win32Logger::tail(std::uint32_t max_lines) const
{
    return inner_->tail(max_lines);
}

Status Win32Logger::write_status(
    LogLevel level, std::string_view message, const Status& status, std::string_view detail)
{
    std::string text(message);
    text += " [";
    text += error_code_name_of(status.code());
    text += ']';

    if (status.is_ok()) {
        return inner_->write(level, text, detail);
    }

    // The classification is already in the message token, so the cause goes on
    // the detail line - the second line the frozen format reserves for it.
    std::string cause(status.error().to_string());
    if (!detail.empty()) {
        cause += " | ";
        cause += detail;
    }
    return inner_->write(level, text, cause);
}

} // namespace voicetyper::platform
