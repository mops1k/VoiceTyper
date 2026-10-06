#include "platform/windows/win32_clock.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <thread>

namespace voicetyper::platform {
namespace {

#if defined(_WIN32)

/// 100 ns units, the resolution of FILETIME and the widest integer time unit
/// Windows offers before the counters overflow.
using HundredNanoseconds = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;

/// Seconds between the FILETIME epoch (1601-01-01) and the Unix epoch.
/// system_clock is not required to be FILETIME-based on every standard
/// library, so the conversion is explicit rather than assumed.
constexpr std::int64_t kFileTimeEpochOffsetSeconds = 11'644'473'600;

constexpr std::uint64_t kFileTimeTicksPerSecond = 10'000'000;

std::uint64_t performance_frequency() noexcept
{
    // QueryPerformanceFrequency is documented to return TRUE and a non-zero
    // value on every Windows version this product supports (Windows XP and
    // later). A zero frequency would mean the platform cannot answer at all,
    // in which case now() falls back to a 10 MHz assumption rather than
    // dividing by zero.
    static const std::uint64_t cached = []() noexcept -> std::uint64_t {
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
            return static_cast<std::uint64_t>(frequency.QuadPart);
        }
        return 10'000'000;
    }();
    return cached;
}

std::chrono::steady_clock::time_point time_point_from_100ns(HundredNanoseconds ticks) noexcept
{
    return std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(ticks));
}

/// Converts a QPC counter to a steady_clock point without any floating point.
/// The naive `counter * 10^7 / frequency` overflows int64 after roughly a year
/// of uptime, so the value is split into whole seconds and a remainder first.
HundredNanoseconds counter_to_ticks(std::uint64_t counter) noexcept
{
    const std::uint64_t frequency = performance_frequency();
    const std::uint64_t seconds = counter / frequency;
    const std::uint64_t remainder = counter % frequency;
    // remainder < frequency <= 2^63/10^7 in every real configuration, so the
    // product below cannot overflow.
    const std::uint64_t fraction = (remainder * kFileTimeTicksPerSecond) / frequency;
    return HundredNanoseconds(static_cast<std::int64_t>(seconds) * 10'000'000
        + static_cast<std::int64_t>(fraction));
}

std::chrono::steady_clock::time_point query_performance_now() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return time_point_from_100ns(counter_to_ticks(static_cast<std::uint64_t>(counter.QuadPart)));
}

std::chrono::system_clock::time_point query_wall_now() noexcept
{
    FILETIME file_time{};
    // GetSystemTimeAsFileTime always succeeds; the return value is ignored only
    // after the structure is written, and the structure is pre-zeroed.
    GetSystemTimeAsFileTime(&file_time);

    const std::uint64_t ticks = (static_cast<std::uint64_t>(file_time.dwHighDateTime) << 32)
        | static_cast<std::uint64_t>(file_time.dwLowDateTime);
    const std::int64_t unix_ticks = static_cast<std::int64_t>(ticks)
        - kFileTimeEpochOffsetSeconds * static_cast<std::int64_t>(kFileTimeTicksPerSecond);
    const std::chrono::system_clock::time_point base = std::chrono::system_clock::from_time_t(0);
    return base + HundredNanoseconds(unix_ticks);
}

void sleep_native(std::chrono::milliseconds duration) noexcept
{
    // Sleep() rounds up to the millisecond, which is the resolution every wait
    // in this application is specified in anyway (80 ms paste delay, 250 ms VAD
    // poll, 120 ms clipboard retry).
    const auto count = duration.count();
    if (count <= 0) {
        return;
    }
    ::Sleep(static_cast<DWORD>(count > 0x7FFFFFFF ? 0x7FFFFFFF : count));
}

#else

// Not compiled: the library target is WIN32-only. The branch exists so an
// accidental non-Windows compile reports an explicit "this platform has no
// Win32 clock" instead of failing to link, and so the contract test can assert
// the refusal path instead of crashing.
std::chrono::steady_clock::time_point query_performance_now() noexcept
{
    return std::chrono::steady_clock::now();
}

std::chrono::system_clock::time_point query_wall_now() noexcept
{
    return std::chrono::system_clock::now();
}

void sleep_native(std::chrono::milliseconds duration) noexcept
{
    if (duration.count() > 0) {
        std::this_thread::sleep_for(duration);
    }
}

#endif // _WIN32

} // namespace

std::chrono::steady_clock::time_point Win32Clock::now() const
{
    return query_performance_now();
}

std::chrono::system_clock::time_point Win32Clock::wall_now() const
{
    return query_wall_now();
}

std::chrono::steady_clock::duration Win32Clock::elapsed_since(
    std::chrono::steady_clock::time_point start) const
{
    const std::chrono::steady_clock::time_point current = query_performance_now();
    if (current <= start) {
        return std::chrono::steady_clock::duration::zero();
    }
    return current - start;
}

Status Win32Clock::sleep_for(std::chrono::milliseconds duration, const CancellationToken& cancellation)
{
    if (duration <= std::chrono::milliseconds::zero()) {
        // clock.hpp requires exactly this: one cancellation check even for a
        // non-positive duration, so a token that is already cancelled is never
        // missed by a zero wait.
        return domain::check_cancelled(cancellation);
    }
    return sleep_until(query_performance_now() + duration, cancellation);
}

Status Win32Clock::sleep_until(
    std::chrono::steady_clock::time_point deadline,
    const CancellationToken& cancellation)
{
    if (domain::check_cancelled(cancellation).is_error()) {
        return domain::check_cancelled(cancellation);
    }

    if (!cancellation.can_be_cancelled()) {
        // A default-constructed token can never fire, so one wait for the whole
        // remaining duration is correct and cheaper than polling.
        const std::chrono::steady_clock::time_point current = query_performance_now();
        if (deadline > current) {
            sleep_native(std::chrono::ceil<std::chrono::milliseconds>(deadline - current));
        }
        return Status::success();
    }

    while (true) {
        const std::chrono::steady_clock::time_point current = query_performance_now();
        if (deadline <= current) {
            return Status::success();
        }
        std::chrono::milliseconds slice =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - current);
        if (slice > std::chrono::milliseconds{kSleepPollSliceMs}) {
            slice = std::chrono::milliseconds{kSleepPollSliceMs};
        }
        if (slice <= std::chrono::milliseconds::zero()) {
            // A sub-millisecond remainder still has to yield; Sleep() has no
            // finer resolution, and one extra millisecond is harmless because
            // the loop re-checks the deadline immediately.
            slice = std::chrono::milliseconds{1};
        }
        sleep_native(slice);
        const Status cancelled = domain::check_cancelled(cancellation);
        if (cancelled.is_error()) {
            return cancelled;
        }
    }
}

} // namespace voicetyper::platform
