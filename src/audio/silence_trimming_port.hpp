#pragma once

#include "audio/audio_wav.hpp"
#include "audio/transcription_port.hpp"
#include "audio/speech_segments.hpp"
#include "audio/vad.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

namespace voicetyper::audio {

using domain::CancellationToken;
using domain::Result;

/// Drops the silence around a dictation and compresses long pauses before the
/// engine sees the audio.
///
/// It wraps any transcription port, exactly like TermsDictionaryPort, so every
/// engine is treated the same way and the trimming is testable without a model.
/// The segmenter is the same instance the machine uses for its VAD auto-stop
/// (Silero serializes the calls internally), and it is asked after recording
/// stopped, so resetting its streaming state here is safe.
///
/// Level first, then silence. Evidence (Alexander, 09.11.2026): this microphone
/// delivers speech at about -58 dBFS RMS, so the recording is peak-normalised here,
/// BEFORE the detector and the engine, because both make decisions against that
/// level. The gain is capped and never attenuates.
class SilenceTrimmingPort final : public TranscriptionPort {
public:
    /// A recording shorter than this is not trimmed at all: the detection pass
    /// (measured 355 ms for 3 s, 1369 ms for 30 s) would cost more than the
    /// silence it could remove. It is still levelled: the engine sees the same
    /// level for every dictation.
    static constexpr double kMinimumTrimSeconds = 4.0;
    static constexpr std::size_t kMinimumTrimSamples =
        static_cast<std::size_t>(kMinimumTrimSeconds * static_cast<double>(kTargetSampleRate));

    /// Peak a normalised dictation is brought up to: -10 dBFS, a level every
    /// recogniser on this machine was measured working well at.
    static constexpr double kTargetPeak = 0.3;
    /// Never amplify more than this: below it the audio is noise, and amplifying
    /// noise does not create words.
    static constexpr double kMaxGain = 16.0;
    /// How coarse the optional audio profile is (see Report::profile).
    static constexpr double kProfileBucketSeconds = 0.25;

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
        /// Seconds at which the recording first carries energy above its own floor,
        /// -1 when it never does. A value near 0 means the clip starts inside a word:
        /// the phrase began before the capture did, and no post-processing can bring
        /// it back.
        double onset_seconds = -1.0;
        /// True when the trim dropped audio that lies before that onset.
        bool onset_trimmed = false;
        /// Peak gain applied to bring the dictation to Report::peak_after.
        double gain = 1.0;
        /// Peak of the recording before and after that gain.
        double peak_before = 0.0;
        double peak_after = 0.0;
        /// Share of samples that are exactly zero in the whole recording (0..1). A
        /// high value means the capture, not the detector, produced silence - the
        /// distinction "the words were never recorded" and "the trimmer cut them"
        /// rests on this number.
        double zero_fraction = 0.0;
        /// The same measurements for the regions the trim removed, so an over-eager
        /// cut can be recognised from the log alone.
        double leading_rms = 0.0;
        double leading_peak = 0.0;
        double trailing_rms = 0.0;
        double trailing_peak = 0.0;
        double trailing_zero_fraction = 0.0;
        /// Wall time of the detection + energy-guard + trim pass, in milliseconds.
        /// 0 when the pass was skipped.
        long long detect_ms = 0;
        /// Coarse RMS profile, one bucket per kProfileBucketSeconds, formatted as a
        /// comma-separated list. Filled only when VOICETYPER_PROFILE_AUDIO is set:
        /// "why was the end of this phrase cut" has to be answerable from numbers,
        /// and storing the user's audio to answer it is not an option.
        std::string profile;
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
        const auto& source = audio.samples();
        if (source.empty()) {
            report_ = Report{};
            return engine_.transcribe(audio, options, cancellation);
        }

        // Level the dictation first: every decision below - the detector's, the energy
        // guard's, the engine's - is made against a usable level, and this microphone
        // delivers about -58 dBFS RMS.
        std::vector<float> samples = source;
        report_ = Report{};
        report_.gain = normalise_peak(samples, kTargetPeak, kMaxGain);
        report_.peak_before = peak_of(source);
        report_.peak_after = peak_of(samples);
        report_.zero_fraction = zero_fraction_of(source);
        report_.onset_seconds = leading_onset_seconds(samples, kTargetSampleRate);
        const bool want_profile = std::getenv("VOICETYPER_PROFILE_AUDIO") != nullptr;
        if (want_profile) {
            report_.profile = format_profile(samples);
        }

        if (samples.size() < kMinimumTrimSamples) {
            // Below this length the detection pass costs more than the silence it
            // could remove (measured: 355 ms of detection for 3 s of audio), so the
            // recording goes through untouched - apart from the level.
            report_.skipped_short = true;
            return deliver(audio, samples, report_.gain == 1.0, options, cancellation);
        }

        const auto detect_started = std::chrono::steady_clock::now();
        segmenter_.reset();
        const auto detected_segments = segmenter_.detect_speech_no_reset(samples);
        const SpeechMap detected{detected_segments, segmenter_.last_frame_probabilities(),
            segmenter_.probability_frame_seconds()};
        // The detector's word alone does not authorise dropping a region: spans grow
        // outwards while the audio carries energy, so a soft onset or a fading tail
        // survives even when the VAD missed it. One pass, no model.
        const auto segments = extend_segments_with_energy(samples, detected_segments, kTargetSampleRate,
            0.03, kAbsoluteSpeechFrameRms * std::max(1.0, report_.gain));
        const TrimReport trimmed = trim_silence_to_segments(samples, segments, kTargetSampleRate);
        report_.detect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - detect_started).count();
        report_.removed_leading = trimmed.removed_leading;
        report_.removed_trailing = trimmed.removed_trailing;
        report_.compressed_pause_samples = trimmed.compressed_pause_samples;
        report_.speech_segments = trimmed.speech_segments;
        report_.unchanged = trimmed.samples.size() == samples.size();
        report_.onset_trimmed = report_.onset_seconds >= 0.0
            && trimmed.removed_leading
                > static_cast<std::size_t>(report_.onset_seconds * static_cast<double>(kTargetSampleRate));
        report_.leading_rms = rms_of(samples, 0, trimmed.removed_leading);
        report_.leading_peak = peak_of(samples, 0, trimmed.removed_leading);
        const auto tail_begin = samples.size() - trimmed.removed_trailing;
        report_.trailing_rms = rms_of(samples, tail_begin, samples.size());
        report_.trailing_peak = peak_of(samples, tail_begin, samples.size());
        report_.trailing_zero_fraction = zero_fraction_of(samples, tail_begin, samples.size());

        // The detector's answer travels with the audio: the engine must not pay for
        // a second pass over the same dictation. The map is in the timeline of the
        // trimmed audio, exactly like the samples handed over below.
        SessionOptions forwarded = options;
        forwarded.speech_map =
            map_speech_map(detected, trimmed.kept_spans, trimmed.samples.size(), kTargetSampleRate);
        const bool untouched = report_.gain == 1.0 && trimmed.samples.size() == samples.size();
        return deliver(audio, trimmed.samples, untouched, forwarded, cancellation);
    }

private:
    /// One path to the engine: either the caller's own buffer (nothing was changed)
    /// or a buffer holding the levelled/trimmed audio.
    [[nodiscard]] Result<std::string> deliver(const SampleBuffer& original,
        const std::vector<float>& samples,
        bool same_as_original,
        const SessionOptions& options,
        const CancellationToken& cancellation)
    {
        if (same_as_original) {
            return engine_.transcribe(original, options, cancellation);
        }
        SampleBuffer buffer(samples.size());
        if (buffer.append(samples).is_error()) {
            // Levelling and trimming are improvements, never a reason to lose a
            // dictation: without a buffer the engine gets what came in.
            return engine_.transcribe(original, options, cancellation);
        }
        return engine_.transcribe(buffer, options, cancellation);
    }

    static double peak_of(const std::vector<float>& samples, std::size_t from = 0,
        std::size_t to = static_cast<std::size_t>(-1))
    {
        const auto end = std::min(to, samples.size());
        double peak = 0.0;
        for (std::size_t index = from; index < end; ++index) {
            const double magnitude = samples[index] < 0.0F ? -static_cast<double>(samples[index])
                                                           : static_cast<double>(samples[index]);
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        return peak;
    }

    static double rms_of(const std::vector<float>& samples, std::size_t from, std::size_t to)
    {
        const auto end = std::min(to, samples.size());
        if (end <= from) {
            return 0.0;
        }
        double sum = 0.0;
        for (std::size_t index = from; index < end; ++index) {
            sum += static_cast<double>(samples[index]) * static_cast<double>(samples[index]);
        }
        return std::sqrt(sum / static_cast<double>(end - from));
    }

    static double zero_fraction_of(const std::vector<float>& samples, std::size_t from = 0,
        std::size_t to = static_cast<std::size_t>(-1))
    {
        const auto end = std::min(to, samples.size());
        if (end <= from) {
            return 0.0;
        }
        std::size_t zeros = 0;
        for (std::size_t index = from; index < end; ++index) {
            if (samples[index] == 0.0F) {
                ++zeros;
            }
        }
        return static_cast<double>(zeros) / static_cast<double>(end - from);
    }

    static std::string format_profile(const std::vector<float>& samples)
    {
        const auto bucket = static_cast<std::size_t>(kProfileBucketSeconds * static_cast<double>(kTargetSampleRate));
        if (bucket == 0) {
            return {};
        }
        std::string out;
        for (std::size_t start = 0; start < samples.size(); start += bucket) {
            const auto end = std::min(samples.size(), start + bucket);
            if (!out.empty()) {
                out.push_back(',');
            }
            out += std::to_string(static_cast<int>(std::lround(rms_of(samples, start, end) * 10000.0)));
        }
        return out;
    }

    TranscriptionPort& engine_;
    SpeechSegmenter& segmenter_;
    Report report_;
};

} // namespace voicetyper::audio
