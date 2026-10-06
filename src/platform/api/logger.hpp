#pragma once

// Diagnostic logging contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §7 "Logging";
// .NET reference VoiceTyper.Core/Abstractions/IAppLogger.cs and
// VoiceTyper.Core/Services/FileLogger.cs.
//
// Frozen observable contract (support tooling parses these):
//   * Log file: <logs>/voiceTyper.log.
//   * Line format: "yyyy-MM-dd HH:mm:ss.fff [LEVEL] message" in *local* time,
//     with the exception/detail text on the following line. "yyyy" is the
//     calendar year, "HH" the 24-hour hour, "mm" the minute and "ss" the second
//     — a logger must not swap in a 12-hour clock or a different date order.
//   * Level tokens: INFO, WARN, ERROR.
//   * Rotation happens *before* the append, when the current file is already at
//     least 1,000,000 bytes. Archives are voiceTyper.1.log .. voiceTyper.5.log;
//     the oldest is dropped.
//   * Logging failures are swallowed. A logger must never propagate an error to
//     its caller and must never take the application down. write() therefore
//     returns Status for diagnostics and tests, and production call sites ignore
//     it deliberately.
//
// Intent-parity decisions recorded here:
//   * The .NET build clears the current log at startup and keeps archives. That
//     is a no-op gap for crash diagnosis, so the contract exposes
//     LoggerOptions::clear_on_start instead of hard-coding it; the default keeps
//     the current behavior and changing it is an explicit, approved stability
//     decision with a test.
//   * No recognized audio text, no transcript, and no secrets (tokens, ids,
//     file contents) may appear in log lines. Recognized text in particular is a
//     privacy boundary, not a formatting preference.
//
// Thread affinity: safe from any thread; the backend serializes its own writes.
// Ownership: the backend owns the open file; the caller owns nothing.

#include "domain/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace voicetyper::platform {

using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// Level token written in the "[LEVEL]" field. Ordinals match the .NET LogLevel.
enum class LogLevel : std::uint8_t {
    info = 0,
    warn = 1,
    error = 2,
};

[[nodiscard]] constexpr std::string_view log_level_token(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::info: return "INFO";
    case LogLevel::warn: return "WARN";
    case LogLevel::error: return "ERROR";
    }
    return "INFO";
}

/// Rotate once the current log reaches this many bytes. Frozen at 1 MB decimal,
/// not 1 MiB.
inline constexpr std::uint64_t kLogRotateThresholdBytes = 1'000'000;

/// Number of retained archives: voiceTyper.1.log .. voiceTyper.5.log.
inline constexpr std::uint32_t kLogArchiveCount = 5;

/// Tail length the settings Log page shows.
inline constexpr std::uint32_t kLogViewTailLines = 200;
/// Poll interval of the settings Log page, milliseconds.
inline constexpr int kLogViewPollIntervalMs = 800;

/// Behavior knobs that the .NET build hard-codes. Exposed so a stability
/// improvement is an explicit choice rather than an accidental divergence.
struct LoggerOptions {
    /// Truncate the current log at startup. The .NET default is true; preserving
    /// logs across crashes requires an approved change and a regression test.
    bool clear_on_start = true;
    /// Rotation threshold; the frozen contract value is kLogRotateThresholdBytes.
    std::uint64_t rotate_threshold_bytes = kLogRotateThresholdBytes;
    /// Number of retained archives; the frozen contract value is 5.
    std::uint32_t archive_count = kLogArchiveCount;
};

class Logger {
public:
    virtual ~Logger() = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    /// Directory holding the log and its archives.
    [[nodiscard]] virtual std::string log_directory() const = 0;

    /// Current log file path.
    [[nodiscard]] virtual std::string log_file_path() const = 0;

    /// Truncates the current log file, keeping archives. Idempotent. Returns the
    /// I/O status for tests; production call sites ignore it.
    virtual Status clear() = 0;

    /// Appends one line at `level`. `detail` is written on the following line
    /// when nonempty, which is where an exception or stack trace belongs.
    ///
    /// A failure is reported but must be swallowed by the caller: no code path
    /// in the application may abort because logging failed.
    virtual Status write(LogLevel level, std::string_view message, std::string_view detail = {}) = 0;

    /// Convenience wrappers with the same contract.
    Status info(std::string_view message) { return write(LogLevel::info, message); }
    Status warn(std::string_view message) { return write(LogLevel::warn, message); }
    Status error(std::string_view message, std::string_view detail = {})
    {
        return write(LogLevel::error, message, detail);
    }

    /// Last `max_lines` lines of the current log file, oldest first. Used by the
    /// settings Log page. Returns io_failure when the file cannot be read; the
    /// page then shows a diagnostic instead of stale content.
    [[nodiscard]] virtual Result<std::string> tail(std::uint32_t max_lines) const = 0;

protected:
    Logger() = default;
};

} // namespace voicetyper::platform
