#pragma once

// Clock and interruptible sleep contract.
//
// Why this is an interface at all: the timeouts, poll intervals and VAD windows
// in this application are all behavioral contracts (250 ms VAD poll, 33 ms gamepad
// poll, 80 ms paste delay, 120 ms progress cadence, 1200 ms silence threshold,
// 5 s no-speech stop). Unit tests must be able to replace time with a fake, and
// every wait must be interruptible by a CancellationToken — a blocking sleep that
// cannot be cancelled is how shutdown hangs.
//
// Frozen rules:
//   * now() is monotonic (steady_clock). It is never used for wall-clock
//     timestamps; the log line format owns those, because a monotonic clock has
//     no calendar representation.
//   * sleep() is interruptible: it returns ErrorCode::cancelled as soon as the
//     token fires, and the caller must treat that as normal control flow.
//   * A default-constructed CancellationToken never interrupts, so a sleep with
//     no token is an ordinary sleep.
//
// Thread affinity: safe from any thread. Fake implementations are shared by
// tests and are not thread-safe unless documented otherwise.
//
// Ownership: stateless; a fake keeps whatever state its test gives it.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"

#include <chrono>
#include <string_view>

namespace voicetyper::platform {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::Status;

/// A monotonic time source.
class Clock {
public:
    virtual ~Clock() = default;

    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;

    /// Current monotonic time. Successive calls are non-decreasing.
    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const = 0;

    /// Wall-clock time, used only where a calendar timestamp is required (the
    /// log line). Never use it to measure a duration.
    [[nodiscard]] virtual std::chrono::system_clock::time_point wall_now() const = 0;

    /// Elapsed time since `start`. Non-negative; the same clock must be used.
    [[nodiscard]] virtual std::chrono::steady_clock::duration elapsed_since(
        std::chrono::steady_clock::time_point start) const = 0;

    /// Sleeps for `duration`, returning early with ErrorCode::cancelled when the
    /// token fires. A non-positive duration returns success immediately after
    /// one cancellation check, so a cancelled token is never missed.
    virtual Status sleep_for(std::chrono::milliseconds duration, const CancellationToken& cancellation) = 0;

    /// Sleeps until `deadline`, with the same cancellation semantics.
    virtual Status sleep_until(
        std::chrono::steady_clock::time_point deadline, const CancellationToken& cancellation) = 0;

protected:
    Clock() = default;
};

/// VAD poll cadence, milliseconds. Frozen from SileroSpeechSegmenter usage.
inline constexpr int kVadPollIntervalMs = 250;
/// Minimum speech duration before a VAD segment counts, milliseconds.
inline constexpr int kVadMinSpeechMs = 250;
/// No-speech auto-stop window in VAD mode, milliseconds.
inline constexpr int kVadNoSpeechStopMs = 5000;
/// Default silence threshold in VAD mode, milliseconds (settings default).
inline constexpr int kSilenceThresholdDefaultMs = 1200;

} // namespace voicetyper::platform
