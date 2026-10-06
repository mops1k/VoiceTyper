#pragma once

// Win32 implementation of the frozen platform::Clock contract.
//
// Evidence and contract:
//   * src/platform/api/clock.hpp - monotonic now(), calendar wall_now(),
//     interruptible sleep, and a default CancellationToken that never
//     interrupts.
//   * docs/migration/cpp/compatibility-contracts.md §3 (80 ms paste delay),
//     §5 (250 ms VAD poll, 1200 ms silence threshold, 5 s no-speech stop).
//     Every one of those is a *behavioral* contract, so a sleep that cannot be
//     cancelled is a shutdown hang, not a slow path.
//
// Why QueryPerformanceCounter and not timeGetTime: QPC is the only Windows
// monotonic source with documented sub-microsecond resolution and it is the
// documented source for QueryPerformanceCounter-based timing; the FILETIME
// epoch (100 ns since 1601) is also monotonic, but it is only as good as the
// system timer. wall_now() uses GetSystemTimeAsFileTime, which is what the
// contract means by "calendar timestamp": the log line format owns those and
// the monotonic clock has no calendar representation.
//
// Cancellation design (the part that is easy to get wrong):
//   * A single WaitForSingleObject on the whole duration would return only when
//     the duration expires, so a token that fires at t+10ms of a 30 s wait
//     would still block for 30 s. Sleep therefore polls in slices of at most
//     kSleepPollSliceMs (20 ms) and re-checks the token between slices, so the
//     observed cancellation latency is bounded by one slice, never by the
//     requested duration.
//   * A non-positive duration returns success *after one cancellation check*,
//     exactly as clock.hpp requires, so a token that is already cancelled is
//     never missed by a zero-length wait.
//   * A default-constructed token (can_be_cancelled() == false) skips the
//     per-slice re-read and sleeps in one call, which is the "ordinary sleep"
//     case the contract describes.
//
// Platform boundary: this header is standard-C++20 and includes no Windows
// header, so a contract test can be compiled anywhere and can assert the
// "unavailable off Windows" behaviour without <windows.h>. The QPC / FILETIME
// calls live in win32_clock.cpp.
//
// Thread affinity: safe from any thread; the backend holds no state at all.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "platform/api/clock.hpp"

#include <chrono>

namespace voicetyper::platform {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::Status;

/// Longest single uninterruptible wait inside sleep_for/sleep_until,
/// milliseconds. It is the worst-case latency between a token firing and the
/// sleep returning ErrorCode::cancelled.
inline constexpr int kSleepPollSliceMs = 20;

/// The production clock. Stateless, so one instance can be shared by every
/// component without synchronization.
class Win32Clock final : public Clock {
public:
    /// Monotonic time from QueryPerformanceCounter. Non-decreasing across calls.
    [[nodiscard]] std::chrono::steady_clock::time_point now() const override;

    /// Wall-clock time from GetSystemTimeAsFileTime. Only the log line format
    /// consumes it; never use it to measure a duration.
    [[nodiscard]] std::chrono::system_clock::time_point wall_now() const override;

    /// now() - start, clamped at zero so a caller can never observe a negative
    /// duration even if the two values came from different clock instances.
    [[nodiscard]] std::chrono::steady_clock::duration elapsed_since(
        std::chrono::steady_clock::time_point start) const override;

    /// Interruptible wait. Returns ErrorCode::cancelled as soon as the token
    /// fires, in at most kSleepPollSliceMs of extra latency.
    Status sleep_for(std::chrono::milliseconds duration, const CancellationToken& cancellation) override;

    /// sleep_for() spelled as a deadline; the deadline is converted through
    /// now() so both spellings share one implementation.
    Status sleep_until(
        std::chrono::steady_clock::time_point deadline,
        const CancellationToken& cancellation) override;
};

} // namespace voicetyper::platform
