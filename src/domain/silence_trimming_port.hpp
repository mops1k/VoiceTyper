#pragma once

#include "domain/audio_wav.hpp"
#include "domain/recording_state_machine.hpp"
#include "domain/speech_segments.hpp"
#include "domain/vad.hpp"

#include <algorithm>
#include <cstddef>

namespace voicetyper::domain {

/// Drops the silence around a dictation and compresses long pauses before the
/// engine sees the audio.
///
/// It wraps any transcription port, exactly like TermsDictionaryPort, so every
/// engine is treated the same way and the trimming is testable without a model.
/// The segmenter is the same instance the machine uses for its VAD auto-stop
/// (Silero serializes the calls internally), and it is asked after recording
/// stopped, so resetting its streaming state here is safe.
class SilenceTrimmingPort final : public TranscriptionPort {
public:
    /// A recording shorter than this is not trimmed at all: the detection pass
    /// (measured 355 ms for 3 s, 1369 ms for 30 s) would cost more than the
    /// silence it could remove.
    static constexpr double kMinimumTrimSeconds = 4.0;
    static constexpr std::size_t kMinimumTrimSamples =
        static_cast<std::size_t>(kMinimumTrimSeconds * static_cast<double>(kTargetSampleRate));

    /// What the last call did, for the log and for tests.
    struct Report {
        std::size_t removed_leading = 0;
        std::size_t removed_trailing = 0;
        std::size_t compressed_pause_samples = 0;
        std::size_t speech_segments = 0;
        /// True when the engine was given the audio unchanged.
        bool unchanged = true;
        /// True when the recording was too short to be worth a detection pass.
        bool skipped_short = false;
    };

    SilenceTrimmingPort(TranscriptionPort& engine, SpeechSegmenter& segmenter)
        : engine_(engine)
        , segmenter_(segmenter)
    {
    }

    [[nodiscard]] const Report& last_report() const noexcept { return report_; }

    [[nodiscard]] Result<std::string> transcribe(const SampleBuffer& audio,
        const SessionOptions& options,
        const CancellationToken& cancellation) override
    {
        const auto& samples = audio.samples();
        if (samples.empty()) {
            report_ = Report{};
            return engine_.transcribe(audio, options, cancellation);
        }
        if (samples.size() < kMinimumTrimSamples) {
            // Below this length the detection pass costs more than the silence it
            // could remove (measured: 355 ms of detection for 3 s of audio), so the
            // recording goes through untouched. Nothing is lost: a clip this short
            // is inside every engine's window, so no engine runs its own detector
            // for it either.
            report_ = Report{};
            report_.skipped_short = true;
            return engine_.transcribe(audio, options, cancellation);
        }

        segmenter_.reset();
        const auto segments = segmenter_.detect_speech_no_reset(samples);
        const SpeechMap detected{segments, segmenter_.last_frame_probabilities(),
            segmenter_.probability_frame_seconds()};
        const TrimReport trimmed = trim_silence_to_segments(samples, segments, kTargetSampleRate);
        report_ = Report{trimmed.removed_leading, trimmed.removed_trailing,
            trimmed.compressed_pause_samples, trimmed.speech_segments,
            trimmed.samples.size() == samples.size()};

        // The detector's answer travels with the audio: the engine must not pay for
        // a second pass over the same dictation.
        SessionOptions forwarded = options;
        forwarded.speech_map =
            map_speech_map(detected, trimmed.kept_spans, trimmed.samples.size(), kTargetSampleRate);

        if (trimmed.samples.size() == samples.size()) {
            // Same length means nothing was dropped: hand the original buffer over
            // rather than a copy the engine would read twice.
            return engine_.transcribe(audio, forwarded, cancellation);
        }

        SampleBuffer buffer(trimmed.samples.size());
        if (buffer.append(trimmed.samples).is_error()) {
            // Trimming is an improvement, never a reason to lose a dictation.
            return engine_.transcribe(audio, forwarded, cancellation);
        }
        return engine_.transcribe(buffer, forwarded, cancellation);
    }

private:
    TranscriptionPort& engine_;
    SpeechSegmenter& segmenter_;
    Report report_;
};

} // namespace voicetyper::domain
