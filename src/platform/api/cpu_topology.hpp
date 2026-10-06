#pragma once

// CPU topology contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §5 ("physical cores
// clamped 1..16") and VoiceTyper.Core/Services/CpuCoreInfo.cs; the VAD thread
// clamp in VoiceTyper.Core/Audio/SileroSpeechSegmenter.cs.
//
// Why this matters behaviorally: whisper.cpp is faster with a thread count equal
// to the *physical* core count, because SMT siblings compete for the same
// execution units. The .NET build asks the OS for the physical core count and
// falls back to half the logical count when that is unavailable. Both the
// inference thread count and the VAD thread count are derived from it, so the
// derivation is a contract that belongs in a portable header rather than in each
// backend.
//
// Frozen rules:
//   * Whisper inference threads = physical cores, clamped to 1..16.
//   * VAD threads = logical processors / 2, clamped to 2..8.
//   * When the physical core count cannot be determined,
//     physical_fallback = max(1, logical_processors / 2), exactly like the .NET
//     fallback, and physical_cores_known is false so callers can log that the
//     number is an estimate.
//
// Thread affinity: detection is safe from any thread but should be called once at
// startup and cached by the caller, not by this interface.

#include "domain/error.hpp"

#include <cstdint>
#include <string_view>

namespace voicetyper::platform {

using domain::ErrorCode;
using domain::Result;

/// Lower bound of the Whisper inference thread clamp.
inline constexpr std::uint32_t kInferenceThreadsMin = 1;
/// Upper bound of the Whisper inference thread clamp.
inline constexpr std::uint32_t kInferenceThreadsMax = 16;
/// Lower bound of the VAD thread clamp.
inline constexpr std::uint32_t kVadThreadsMin = 2;
/// Upper bound of the VAD thread clamp.
inline constexpr std::uint32_t kVadThreadsMax = 8;

/// What the backend could determine about this machine.
struct CpuTopology {
    /// Logical processors visible to the process (hardware concurrency).
    std::uint32_t logical_processors = 0;
    /// Physical cores. Meaningful only when `physical_cores_known` is true.
    std::uint32_t physical_cores = 0;
    /// False when the OS could not report the physical core count and
    /// `physical_cores` holds the documented fallback estimate.
    bool physical_cores_known = false;
    /// Short description of the detection source, for diagnostics.
    std::string_view source = "unknown";
};

[[nodiscard]] constexpr std::uint32_t clamp_thread_count(
    std::uint32_t value, std::uint32_t low, std::uint32_t high) noexcept
{
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

/// The .NET fallback: half the logical processors, at least one.
[[nodiscard]] constexpr std::uint32_t physical_cores_fallback(std::uint32_t logical_processors) noexcept
{
    return logical_processors / 2 > 0 ? logical_processors / 2 : 1;
}

/// Threads to give Whisper inference for a detected topology.
[[nodiscard]] constexpr std::uint32_t inference_thread_count(const CpuTopology& topology) noexcept
{
    const std::uint32_t cores = topology.physical_cores_known ? topology.physical_cores
                                                              : physical_cores_fallback(topology.logical_processors);
    return clamp_thread_count(cores, kInferenceThreadsMin, kInferenceThreadsMax);
}

/// Threads to give the Silero VAD for a detected topology.
[[nodiscard]] constexpr std::uint32_t vad_thread_count(const CpuTopology& topology) noexcept
{
    return clamp_thread_count(topology.logical_processors / 2, kVadThreadsMin, kVadThreadsMax);
}

/// Detects the CPU topology once, at startup.
class CpuTopologyProvider {
public:
    virtual ~CpuTopologyProvider() = default;

    CpuTopologyProvider(const CpuTopologyProvider&) = delete;
    CpuTopologyProvider& operator=(const CpuTopologyProvider&) = delete;
    CpuTopologyProvider(CpuTopologyProvider&&) = delete;
    CpuTopologyProvider& operator=(CpuTopologyProvider&&) = delete;

    /// Queries the OS.
    ///
    /// Failure: unsupported (the platform exposes no topology at all). A
    /// platform that can report only the logical count still succeeds, with
    /// `physical_cores_known == false`.
    [[nodiscard]] virtual Result<CpuTopology> detect() const = 0;

protected:
    CpuTopologyProvider() = default;
};

} // namespace voicetyper::platform
