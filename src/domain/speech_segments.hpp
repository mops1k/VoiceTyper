#pragma once

// Speech-segment policy: what the engine is allowed to see.
//
// Evidence: docs/migration/cpp/feature-parity.md rows "Silence trimming" and
// "ASR model lifecycle"; the .NET reference
// VoiceTyper.Core/Audio/SilenceTrimmer.cs (trim constants) and
// VoiceTyper.Core/Audio/SileroSpeechSegmenter.cs (segment source); the portable
// segmenter contract in src/domain/vad.hpp.
//
// Why this is portable: the *decisions* - which samples are speech, where a clip
// longer than the model window may be cut, how partial transcripts are joined -
// are pure functions over segment times and frame probabilities. They use no
// model, no whisper.cpp and no OS call, so they are contract-tested on every
// host, while the model that PRODUCES the segments lives in
// src/asr/silero_vad.*. That split is the same one used by the engine registry:
// policy here, native dependency behind a seam.
//
// Frozen behaviour:
//   * leading/trailing silence is removed with the .NET margins (0.25 s kept)
//     and internal silences longer than 0.6 s are compressed to 0.3 s, keeping
//     the FIRST samples of the run - exactly the choice the existing
//     RMS-based trim_silence() makes, so moving the detector from RMS to Silero
//     must not change the observable trim shape;
//   * a chunk never begins or ends inside a speech segment unless that single
//     segment is longer than the whole window; then it is cut at the frame with
//     the LOWEST speech probability inside the allowed range (earliest on ties),
//     which is the least damaging cut the data offers and is deterministic;
//   * chunks are contiguous, non-empty, ascending, and together cover every
//     speech sample of the recording;
//   * joining partial transcripts adds at most one space between two parts and
//     never invents punctuation.

#include "domain/vad.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace voicetyper::domain {

/// Trim constants of VoiceTyper.Core/Audio/SilenceTrimmer.cs as ported in
/// src/domain/audio_wav.cpp. Declared once so both detectors share them.
inline constexpr double kSilenceTrimMarginSeconds = 0.25;
inline constexpr double kSilenceTrimMaxSilenceSeconds = 0.6;
inline constexpr double kSilenceTrimGapSeconds = 0.3;

/// Window a single decode may see when the engine has a bounded input window
/// (GigaAM v3 is trained on ~25 s). Used as the default chunk budget.
inline constexpr double kDefaultMaxChunkSeconds = 25.0;

/// A trim never produces a chunk shorter than this while cutting inside speech;
/// it exists so a pathological probability vector cannot yield a 1-sample chunk.
inline constexpr double kMinChunkSeconds = 0.5;

/// Result of a segment-driven trim together with the numbers the log reports.
struct TrimReport {
    /// The trimmed audio. If `speech_segments` is 0 this is the input unchanged:
    /// dropping a whole dictation because the VAD was unsure is worse than
    /// sending untrimmed audio, and the engine answers blank for silence anyway.
    std::vector<float> samples;
    std::size_t removed_leading = 0;
    std::size_t removed_trailing = 0;
    std::size_t compressed_pause_samples = 0;
    std::size_t speech_segments = 0;
};

/// Trims `samples` to the span the segments describe: leading/trailing silence
/// beyond `margin_seconds` is dropped, and every silence run longer than
/// `max_silence_seconds` is compressed to `gap_silence_seconds`.
///
/// Segment times are seconds on the same timeline as `samples`. Unsorted,
/// overlapping or out-of-range segments are normalized (clamped, sorted, merged)
/// rather than rejected, because a VAD that pads or slightly overshoots must not
/// corrupt the audio.
[[nodiscard]] TrimReport trim_silence_to_segments(
    const std::vector<float>& samples,
    const std::vector<SpeechSegment>& segments,
    std::size_t sample_rate = 16000,
    double margin_seconds = kSilenceTrimMarginSeconds,
    double max_silence_seconds = kSilenceTrimMaxSilenceSeconds,
    double gap_silence_seconds = kSilenceTrimGapSeconds);

/// One contiguous slice of the recording, as sample indices into the original
/// buffer. The engine receives the slice as its own WAV.
struct SpeechChunk {
    std::size_t begin_sample = 0;
    std::size_t end_sample = 0;

    [[nodiscard]] std::size_t size() const noexcept { return end_sample - begin_sample; }
};

/// Plans the chunks of one recording for an engine whose input window is bounded
/// by `max_chunk_seconds`.
///
/// `segments` are speech spans (seconds); `probabilities` is the VAD's per-frame
/// speech probability, `frame_seconds` long each, and is used only to place a
/// forced cut inside one over-long speech segment. A recording with no speech
/// segments yields a single chunk covering all of it: the caller decides whether
/// that is worth sending.
///
/// Returns chunks in ascending order. An empty result means there was nothing to
/// plan (total_samples == 0).
[[nodiscard]] std::vector<SpeechChunk> plan_speech_chunks(
    const std::vector<SpeechSegment>& segments,
    const std::vector<float>& probabilities,
    double frame_seconds,
    std::size_t total_samples,
    std::size_t sample_rate = 16000,
    double max_chunk_seconds = kDefaultMaxChunkSeconds);

/// Joins the transcripts of consecutive chunks into the final text.
///
/// Each part is trimmed and internal runs of whitespace (including newlines and
/// tabs) collapse to a single space. Empty parts disappear. Two surviving parts
/// are separated by one space unless the left one already ends with a space or a
/// hyphen, or the right one starts with punctuation.
[[nodiscard]] std::string join_transcripts(const std::vector<std::string>& parts);

} // namespace voicetyper::domain
