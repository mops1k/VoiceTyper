#include "domain/file_logger.hpp"

#include "platform/api/paths.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace voicetyper::domain {
namespace detail {

std::string default_line_ending()
{
#ifdef _WIN32
    return "\r\n";
#else
    return "\n";
#endif
}

std::string local_timestamp_now()
{
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3) << milliseconds.count();
    return output.str();
}

} // namespace detail

FileLogger::FileLogger(
    std::filesystem::path directory,
    platform::LoggerOptions options,
    std::function<std::string()> now,
    std::string_view line_ending)
    : directory_(std::move(directory))
    , options_(options)
    , now_(std::move(now))
    , line_ending_(line_ending.empty() ? detail::default_line_ending() : std::string(line_ending))
{
    if (!options_.clear_on_start) {
        return;
    }
    const auto path = directory_ / std::filesystem::path(std::string(platform::kLogFileName));
    std::error_code error;
    if (std::filesystem::exists(path, error) && !error) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
    }
}

std::string FileLogger::log_directory() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return directory_.string();
}

std::string FileLogger::log_file_path() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return (directory_ / std::filesystem::path(std::string(platform::kLogFileName))).string();
}

std::filesystem::path FileLogger::archive_path(std::uint32_t index) const
{
    return directory_ / ("voiceTyper." + std::to_string(index) + ".log");
}

Status FileLogger::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto path = directory_ / std::filesystem::path(std::string(platform::kLogFileName));
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return error
            ? Status::failure(ErrorCode::io_failure, "cannot inspect log file: " + error.message())
            : Status::success();
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Status::failure(ErrorCode::io_failure, "cannot clear log file");
    }
    return Status::success();
}

Status FileLogger::rotate_if_needed()
{
    if (options_.archive_count == 0 || options_.rotate_threshold_bytes == 0) {
        return Status::success();
    }
    const auto current = directory_ / std::filesystem::path(std::string(platform::kLogFileName));
    std::error_code error;
    if (!std::filesystem::exists(current, error) || error) {
        return error
            ? Status::failure(ErrorCode::io_failure, "cannot inspect log file for rotation: " + error.message())
            : Status::success();
    }
    const auto size = std::filesystem::file_size(current, error);
    if (error) {
        return Status::failure(ErrorCode::io_failure, "cannot read log size: " + error.message());
    }
    if (size < options_.rotate_threshold_bytes) {
        return Status::success();
    }

    for (std::uint32_t index = options_.archive_count - 1; index >= 1; --index) {
        const auto source = archive_path(index);
        const auto destination = archive_path(index + 1);
        if (!std::filesystem::exists(source, error) || error) {
            if (error) {
                return Status::failure(ErrorCode::io_failure, "cannot inspect log archive: " + error.message());
            }
            continue;
        }
        std::filesystem::remove(destination, error);
        if (error) {
            return Status::failure(ErrorCode::io_failure, "cannot replace log archive: " + error.message());
        }
        std::filesystem::rename(source, destination, error);
        if (error) {
            return Status::failure(ErrorCode::io_failure, "cannot rotate log archive: " + error.message());
        }
    }

    const auto first = archive_path(1);
    std::filesystem::remove(first, error);
    if (error) {
        return Status::failure(ErrorCode::io_failure, "cannot replace first log archive: " + error.message());
    }
    std::filesystem::rename(current, first, error);
    if (error) {
        return Status::failure(ErrorCode::io_failure, "cannot rotate current log: " + error.message());
    }
    return Status::success();
}

std::string FileLogger::format_timestamp() const
{
    if (now_) {
        return now_();
    }
    return detail::local_timestamp_now();
}

Status FileLogger::write(
    platform::LogLevel level,
    std::string_view message,
    std::string_view detail)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
        return Status::failure(ErrorCode::io_failure, "cannot create log directory: " + error.message());
    }
    const auto rotation = rotate_if_needed();
    if (rotation.is_error()) {
        return rotation;
    }

    const auto current = directory_ / std::filesystem::path(std::string(platform::kLogFileName));
    std::ofstream output(current, std::ios::binary | std::ios::app);
    if (!output) {
        return Status::failure(ErrorCode::io_failure, "cannot open log file");
    }
    output << format_timestamp() << " [" << platform::log_level_token(level) << "] " << message;
    if (!detail.empty()) {
        output << line_ending_ << detail;
    }
    output << line_ending_;
    if (!output) {
        return Status::failure(ErrorCode::io_failure, "cannot append log line");
    }
    return Status::success();
}

Result<std::string> FileLogger::tail(std::uint32_t max_lines) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto path = directory_ / std::filesystem::path(std::string(platform::kLogFileName));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<std::string>::failure(ErrorCode::io_failure, "cannot read log file");
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    if (max_lines == 0 || lines.empty()) {
        return std::string();
    }
    const auto count = std::min<std::size_t>(max_lines, lines.size());
    std::ostringstream output;
    for (std::size_t i = lines.size() - count; i < lines.size(); ++i) {
        output << lines[i] << '\n';
    }
    return output.str();
}

} // namespace voicetyper::domain
