#pragma once

// Portable FileLogger implementation. It is deliberately independent of the
// OS logger backend: a platform backend supplies only the directory and the
// line-ending convention.

#include "platform/api/logger.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace voicetyper::domain {

class FileLogger final : public platform::Logger {
public:
    /// `now` is injectable for deterministic contract tests. When it is empty,
    /// the logger formats the current local system time. An empty line ending
    /// selects "\\r\\n" on Windows and "\\n" elsewhere.
    explicit FileLogger(
        std::filesystem::path directory,
        platform::LoggerOptions options = {},
        std::function<std::string()> now = {},
        std::string_view line_ending = {});

    [[nodiscard]] std::string log_directory() const override;
    [[nodiscard]] std::string log_file_path() const override;
    Status clear() override;
    Status write(
        platform::LogLevel level,
        std::string_view message,
        std::string_view detail = {}) override;
    [[nodiscard]] Result<std::string> tail(std::uint32_t max_lines) const override;

private:
    std::filesystem::path archive_path(std::uint32_t index) const;
    Status rotate_if_needed();
    std::string format_timestamp() const;

    std::filesystem::path directory_;
    platform::LoggerOptions options_;
    std::function<std::string()> now_;
    std::string line_ending_;
    mutable std::mutex mutex_;
};

} // namespace voicetyper::domain
