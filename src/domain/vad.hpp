#pragma once

// Portable VAD/auto-stop policy. A native Silero implementation is a later
// backend; this slice defines the deterministic segmenter seam and the exact
// C# auto-stop state machine.

#include "domain/error.hpp"

#include <functional>
#include <vector>

namespace voicetyper::domain {

struct SpeechSegment {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
};

/// The C# ISpeechSegmenter contract reduced to portable data. Implementations
/// keep their own context; reset() is called between recording sessions.
class SpeechSegmenter {
public:
    virtual ~SpeechSegmenter() = default;
    SpeechSegmenter(const SpeechSegmenter&) = delete;
    SpeechSegmenter& operator=(const SpeechSegmenter&) = delete;
    SpeechSegmenter(SpeechSegmenter&&) = delete;
    SpeechSegmenter& operator=(SpeechSegmenter&&) = delete;

    [[nodiscard]] virtual std::vector<SpeechSegment> detect_speech_no_reset(
        const std::vector<float>& samples) = 0;
    virtual void reset() = 0;

protected:
    SpeechSegmenter() = default;
};

/// Adapter for a cached/native model or a deterministic test double. The
/// callback is invoked for every chunk and may return chunk-relative or
/// absolute segment times, matching the C# normalization rule.
class CallbackSpeechSegmenter final : public SpeechSegmenter {
public:
    using Callback = std::function<std::vector<SpeechSegment>(const std::vector<float>&)>;

    explicit CallbackSpeechSegmenter(Callback callback);
    [[nodiscard]] std::vector<SpeechSegment> detect_speech_no_reset(
        const std::vector<float>& samples) override;
    void reset() override;

private:
    Callback callback_;
};

/// Energy-based speech segmenter, used when no neural VAD model is bound.
///
/// Scope, stated plainly: this is the approved D3 heuristic, NOT Silero. The
/// shipped ggml-silero-v6.2.0.bin is present on the machine but is not yet bound
/// to a native runtime, so the C++ app needs a working segmenter for VAD mode
/// rather than a recording that never auto-stops. It adapts its noise floor from
/// the stream itself, so it works on an unknown microphone and in a quiet room.
///
/// Known difference from the .NET build, which uses Silero: this detector can be
/// fooled by steady background noise, and it has no notion of a speaker. That is
/// the cost of not binding the model yet, and it is why a native Silero segmenter
/// is still owed.
class EnergySpeechSegmenter final : public SpeechSegmenter {
public:
    EnergySpeechSegmenter(
        double sample_rate = 16000,
        double frame_seconds = 0.03,
        double speech_over_noise_ratio = 3.0);

    [[nodiscard]] std::vector<SpeechSegment> detect_speech_no_reset(
        const std::vector<float>& samples) override;
    void reset() override;

    /// The adaptive floor the current decision used, for tests and diagnostics.
    [[nodiscard]] double noise_floor() const noexcept { return noise_floor_; }

private:
    std::size_t sample_rate_;
    std::size_t frame_samples_;
    double speech_over_noise_ratio_;
    double noise_floor_ = 0.0;
    bool floor_initialised_ = false;
    double pending_speech_start_seconds_ = 0.0;
    bool in_speech_ = false;
    double fed_seconds_ = 0.0;
};

enum class VadStopReason : std::uint8_t {
    none = 0,
    no_speech_idle = 1,
    trailing_silence = 2,
};

struct VadStopDecision {
    bool stop = false;
    VadStopReason reason = VadStopReason::none;
};

/// Port of SilenceAutoStopDetector.cs. It is final-only: it never produces a
/// streaming transcript or preview.
class SilenceAutoStopDetector final {
public:
    SilenceAutoStopDetector(
        SpeechSegmenter& segmenter,
        double silence_threshold_seconds = 1.2,
        std::size_t sample_rate = 16000);

    [[nodiscard]] VadStopDecision process(const std::vector<float>& samples);
    void reset() noexcept;

    [[nodiscard]] double total_fed_seconds() const noexcept { return total_fed_seconds_; }
    [[nodiscard]] double hold_seconds() const noexcept { return hold_seconds_; }

private:
    SpeechSegmenter& segmenter_;
    double silence_threshold_seconds_;
    double hold_seconds_;
    std::size_t sample_rate_;
    double total_fed_seconds_ = 0.0;
    double last_speech_end_seconds_ = 0.0;
    bool speech_detected_ = false;
    double noise_floor_ = 1e-5;
    int consecutive_inactive_chunks_ = 0;
};

} // namespace voicetyper::domain
