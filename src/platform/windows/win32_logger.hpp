#pragma once

// Win32 assembly of the two pieces the frozen logging contract is made of:
// platform::Logger (the interface) implemented over the portable
// domain::FileLogger (the .NET-compatible line format and rotation), fed by a
// platform::Clock for the timestamp and by domain::AppPaths for the directory.
//
// Evidence and contract:
//   * src/platform/api/logger.hpp - the frozen line format
//     "yyyy-MM-dd HH:mm:ss.fff [LEVEL] message", the INFO/WARN/ERROR tokens,
//     rotation before the append at 1,000,000 bytes, voiceTyper.1..5.log
//     archives, swallowed failures, tail() for the settings Log page.
//   * src/domain/file_logger.* - the portable implementation; it takes a
//     directory, options, an injectable `now` and a line ending.
//   * src/domain/app_paths.* - resolves %LOCALAPPDATA%\VoiceTyper\logs.
//   * docs/migration/cpp/compatibility-contracts.md §7 - local time, default
//     process encoding, current log cleared at startup, archives retained.
//
// What this backend adds, and only this:
//   * The timestamp comes from the injected platform::Clock, so a contract test
//     can freeze it exactly as the .NET suite freezes DateTime.Now.
//   * The line ending is "\r\n" without exception. FileLogger already defaults
//     to CRLF under _WIN32; passing it explicitly means the choice is a
//     documented decision here and not an #ifdef in the portable layer.
//   * Every domain::ErrorCode has a stable name (domain::error_code_name) and
//     write_status() puts exactly that name in the line. Diagnostics tooling
//     greps for these tokens, so the mapping must never be a localised or
//     invented string; "unknown" is the only value it may never produce, and the
//     contract test walks all eighteen codes to prove it.
//
// Why the clipboard/paste backends are not mentioned here: a failed paste is
// logged through write_status() with ErrorCode::permission_denied or
// ErrorCode::unavailable, which is how the UAC case becomes a visible
// clipboard_only diagnostic instead of a silent one.
//
// Platform boundary: this header is standard-C++20 and includes no Windows
// header. Win32Clock is the default time source, so the default-constructed
// logger is already Windows-correct without a #ifdef anywhere here.
//
// Thread affinity: safe from any thread; domain::FileLogger serializes its own
// writes. Ownership: the logger owns its FileLogger; an injected clock is
// borrowed and must outlive it (Win32Clock is stateless, so borrowing one is
// free).

#include "domain/app_paths.hpp"
#include "domain/error.hpp"
#include "domain/file_logger.hpp"
#include "platform/api/clock.hpp"
#include "platform/api/logger.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace voicetyper::platform {

using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// The line ending this backend always writes. Frozen: the .NET build writes
/// through the process default encoding on Windows, i.e. CRLF, and support
/// tooling parses it.
inline constexpr std::string_view kWin32LogLineEnding = "\r\n";

/// Formats `moment` as the frozen log timestamp: "yyyy-MM-dd HH:mm:ss.fff" in
/// *local* time. Exposed so a test can assert the format without reading a file.
[[nodiscard]] std::string format_log_timestamp(std::chrono::system_clock::time_point moment);

/// The stable diagnostic name of `code`, i.e. domain::error_code_name() as an
/// owned string. "unknown" is never a valid answer for a declared code.
[[nodiscard]] std::string error_code_name_of(ErrorCode code);

class Win32Logger final : public Logger {
public:
    /// Uses the internal Win32Clock (QueryPerformanceCounter plus
    /// GetSystemTimeAsFileTime) for the timestamp.
    explicit Win32Logger(std::filesystem::path log_directory, LoggerOptions options = {});

    /// Uses `clock` for the timestamp. The clock is borrowed and must outlive
    /// the logger, which is what lets a contract test freeze the line prefix.
    Win32Logger(std::filesystem::path log_directory, const Clock& clock, LoggerOptions options = {});

    ~Win32Logger() override;

    Win32Logger(const Win32Logger&) = delete;
    Win32Logger& operator=(const Win32Logger&) = delete;
    Win32Logger(Win32Logger&&) = delete;
    Win32Logger& operator=(Win32Logger&&) = delete;

    /// Builds a logger for the directory AppPaths resolves, i.e.
    /// %LOCALAPPDATA%\VoiceTyper\logs on Windows.
    [[nodiscard]] static std::unique_ptr<Win32Logger> for_paths(
        const domain::AppPaths& paths,
        LoggerOptions options = {},
        const Clock* clock = nullptr);

    [[nodiscard]] std::string log_directory() const override;
    [[nodiscard]] std::string log_file_path() const override;
    Status clear() override;
    Status write(LogLevel level, std::string_view message, std::string_view detail = {}) override;
    [[nodiscard]] Result<std::string> tail(std::uint32_t max_lines) const override;

    /// Appends one line whose text ends with " [<stable error name>]". A failing
    /// status puts its message on the detail line, so a log parser sees both the
    /// classification and the cause without a second format to teach.
    ///
    /// Success statuses log the literal token "ok", which keeps a caller from
    /// needing two different code paths for the same line.
    Status write_status(
        LogLevel level,
        std::string_view message,
        const Status& status,
        std::string_view detail = {});

    /// The clock this logger stamps its lines with. Never null.
    [[nodiscard]] const Clock& clock() const noexcept;

private:
    Win32Logger(
        std::filesystem::path log_directory,
        std::unique_ptr<Clock> owned_clock,
        const Clock* borrowed_clock,
        LoggerOptions options);

    std::unique_ptr<Clock> owned_clock_;
    const Clock* clock_ = nullptr;
    std::unique_ptr<domain::FileLogger> inner_;
};

} // namespace voicetyper::platform
