#include "domain/speech_segments.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace voicetyper::domain {
namespace {

/// Clamps to the recording, drops empty spans, sorts by start and merges
/// overlapping or touching ones. Every public entry point works on this shape,
/// so a padded or slightly out-of-order VAD result cannot produce a bad cut.
std::vector<SpeechSegment> normalize_segments(
    const std::vector<SpeechSegment>& input, double total_seconds)
{
    std::vector<SpeechSegment> spans;
    spans.reserve(input.size());
    for (const auto& segment : input) {
        if (!std::isfinite(segment.start_seconds) || !std::isfinite(segment.end_seconds)) {
            continue;
        }
        SpeechSegment clamped;
        clamped.start_seconds = std::clamp(segment.start_seconds, 0.0, total_seconds);
        clamped.end_seconds = std::clamp(segment.end_seconds, 0.0, total_seconds);
        if (clamped.end_seconds > clamped.start_seconds) {
            spans.push_back(clamped);
        }
    }
    std::sort(spans.begin(), spans.end(), [](const SpeechSegment& left, const SpeechSegment& right) {
        return left.start_seconds < right.start_seconds;
    });
    std::vector<SpeechSegment> merged;
    for (const auto& span : spans) {
        if (!merged.empty() && span.start_seconds <= merged.back().end_seconds) {
            merged.back().end_seconds = std::max(merged.back().end_seconds, span.end_seconds);
        } else {
            merged.push_back(span);
        }
    }
    return merged;
}

std::size_t sample_at(double seconds, std::size_t sample_rate)
{
    if (seconds <= 0.0) {
        return 0;
    }
    return static_cast<std::size_t>(seconds * static_cast<double>(sample_rate));
}

void append_silence(
    std::vector<float>& out,
    const std::vector<float>& samples,
    std::size_t begin,
    std::size_t end,
    std::size_t max_silence,
    std::size_t gap,
    std::size_t& compressed)
{
    if (end <= begin) {
        return;
    }
    const auto length = end - begin;
    const auto keep = length > max_silence ? gap : length;
    if (keep < length) {
        compressed += length - keep;
    }
    const auto copied = std::min(keep, length);
    out.insert(out.end(), samples.begin() + static_cast<std::ptrdiff_t>(begin),
               samples.begin() + static_cast<std::ptrdiff_t>(begin + copied));
}

/// The sample index with the lowest speech probability inside [lo, hi); the
/// earliest frame wins a tie so the plan is reproducible. Falls back to the
/// midpoint when no probabilities were handed over.
std::size_t lowest_probability_sample(
    const std::vector<float>& probabilities,
    double frame_seconds,
    std::size_t lo,
    std::size_t hi)
{
    if (hi <= lo) {
        return lo;
    }
    if (probabilities.empty() || frame_seconds <= 0.0) {
        return lo + (hi - lo) / 2;
    }
    const double frame_samples = frame_seconds * 16000.0;
    const auto first = static_cast<std::size_t>(static_cast<double>(lo) / frame_samples);
    const auto last = static_cast<std::size_t>(static_cast<double>(hi) / frame_samples);
    std::size_t best_frame = std::min(first, probabilities.size() - 1);
    float best_value = probabilities[best_frame];
    const auto upper = std::min(last + 1, probabilities.size());
    for (std::size_t frame = best_frame + 1; frame < upper; ++frame) {
        if (probabilities[frame] < best_value) {
            best_value = probabilities[frame];
            best_frame = frame;
        }
    }
    const auto sample = static_cast<std::size_t>(static_cast<double>(best_frame) * frame_samples);
    if (sample <= lo) {
        return lo;
    }
    return sample >= hi ? hi : sample;
}

} // namespace

TrimReport trim_silence_to_segments(
    const std::vector<float>& samples,
    const std::vector<SpeechSegment>& segments,
    std::size_t sample_rate,
    double margin_seconds,
    double max_silence_seconds,
    double gap_silence_seconds)
{
    TrimReport report;
    if (samples.empty() || sample_rate == 0) {
        report.samples = samples;
        return report;
    }

    const double total_seconds = static_cast<double>(samples.size()) / static_cast<double>(sample_rate);
    const auto spans = normalize_segments(segments, total_seconds);
    report.speech_segments = spans.size();
    if (spans.empty()) {
        // No speech detected: keep the audio. The engine returns blank for
        // silence, while dropping real dictation over an unsure VAD does not.
        report.samples = samples;
        return report;
    }

    const auto margin = static_cast<std::size_t>(std::max(0.0, margin_seconds) * static_cast<double>(sample_rate));
    const auto max_silence = static_cast<std::size_t>(std::max(0.0, max_silence_seconds) * static_cast<double>(sample_rate));
    const auto gap = static_cast<std::size_t>(std::max(0.0, gap_silence_seconds) * static_cast<double>(sample_rate));

    const auto first_speech = sample_at(spans.front().start_seconds, sample_rate);
    const auto last_speech = sample_at(spans.back().end_seconds, sample_rate);
    const auto begin = first_speech > margin ? first_speech - margin : 0;
    const auto end = std::min(samples.size(), last_speech + margin);
    if (end <= begin) {
        report.samples = samples;
        return report;
    }

    report.removed_leading = begin;
    report.removed_trailing = samples.size() - end;

    std::vector<float> out;
    out.reserve(end - begin);
    std::size_t cursor = begin;
    for (const auto& span : spans) {
        const auto span_begin = std::max(cursor, std::min(end, sample_at(span.start_seconds, sample_rate)));
        const auto span_end = std::max(span_begin, std::min(end, sample_at(span.end_seconds, sample_rate)));
        append_silence(out, samples, cursor, span_begin, max_silence, gap, report.compressed_pause_samples);
        out.insert(out.end(), samples.begin() + static_cast<std::ptrdiff_t>(span_begin),
                   samples.begin() + static_cast<std::ptrdiff_t>(span_end));
        cursor = span_end;
    }
    append_silence(out, samples, cursor, end, max_silence, gap, report.compressed_pause_samples);

    report.samples = std::move(out);
    return report;
}

std::vector<SpeechChunk> plan_speech_chunks(
    const std::vector<SpeechSegment>& segments,
    const std::vector<float>& probabilities,
    double frame_seconds,
    std::size_t total_samples,
    std::size_t sample_rate,
    double max_chunk_seconds)
{
    std::vector<SpeechChunk> chunks;
    if (total_samples == 0) {
        return chunks;
    }
    const double total_seconds = static_cast<double>(total_samples) / static_cast<double>(sample_rate == 0 ? 1 : sample_rate);
    const auto spans = normalize_segments(segments, total_seconds);
    const auto max_samples = static_cast<std::size_t>(
        std::max(0.0, max_chunk_seconds) * static_cast<double>(sample_rate == 0 ? 1 : sample_rate));
    if (spans.empty() || sample_rate == 0 || max_samples == 0) {
        chunks.push_back(SpeechChunk{0, total_samples});
        return chunks;
    }

    const auto min_chunk = static_cast<std::size_t>(kMinChunkSeconds * static_cast<double>(sample_rate));
    // The window budget is measured from the START OF SPEECH, never from the end
    // of the previous chunk: a two-second utterance must not be cut just because
    // the speaker paused for twenty seconds before it. The silence between two
    // chunks is therefore not decoded at all, which is also why chunks are
    // ascending and speech-complete but not necessarily adjacent.
    std::size_t index = 0;
    std::size_t span_cursor = std::min(total_samples, sample_at(spans.front().start_seconds, sample_rate));
    while (index < spans.size()) {
        const auto seg_end = std::min(total_samples, sample_at(spans[index].end_seconds, sample_rate));
        if (seg_end > span_cursor && seg_end - span_cursor > max_samples) {
            // One speech segment is longer than the whole window: cut inside it
            // at the quietest frame the data offers, never closer than
            // kMinChunkSeconds to the chunk start.
            const auto hard_end = std::min(seg_end, span_cursor + max_samples);
            const auto earliest = std::min(hard_end, span_cursor + std::max<std::size_t>(1, min_chunk));
            const auto cut = lowest_probability_sample(probabilities, frame_seconds, earliest, hard_end);
            const auto safe_cut = cut <= span_cursor ? hard_end : cut;
            chunks.push_back(SpeechChunk{span_cursor, safe_cut});
            span_cursor = safe_cut;
            continue;
        }

        std::size_t candidate_end = seg_end;
        std::size_t next = index + 1;
        while (next < spans.size()) {
            const auto next_end = std::min(total_samples, sample_at(spans[next].end_seconds, sample_rate));
            if (next_end - span_cursor > max_samples) {
                break;
            }
            candidate_end = next_end;
            ++next;
        }
        chunks.push_back(SpeechChunk{span_cursor, candidate_end});
        index = next;
        if (index < spans.size()) {
            span_cursor = std::min(total_samples, sample_at(spans[index].start_seconds, sample_rate));
        }
    }

    // Degenerate spans can still produce a zero-length chunk; it is dropped here
    // rather than handed to an engine as an empty WAV.
    std::vector<SpeechChunk> result;
    result.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        if (chunk.size() > 0) {
            result.push_back(chunk);
        }
    }
    return result;
}

std::string join_transcripts(const std::vector<std::string>& parts)
{
    std::string joined;
    for (const auto& part : parts) {
        std::string normalized;
        normalized.reserve(part.size());
        bool pending_space = false;
        for (const char character : part) {
            const bool is_space = character == ' ' || character == '\t' || character == '\n' || character == '\r';
            if (is_space) {
                pending_space = !normalized.empty();
                continue;
            }
            if (pending_space) {
                normalized.push_back(' ');
                pending_space = false;
            }
            normalized.push_back(character);
        }
        if (normalized.empty()) {
            continue;
        }
        if (joined.empty()) {
            joined = std::move(normalized);
            continue;
        }
        const char previous = joined.back();
        const char first = normalized.front();
        // ASCII punctuation only: a multi-byte UTF-8 ellipsis would need a
        // sequence comparison, and guessing at one is worse than a space.
        const bool no_space = previous == ' ' || previous == '-' || previous == '\''
            || first == ',' || first == '.' || first == ';' || first == ':' || first == '!'
            || first == '?' || first == ')' || first == ']';
        if (!no_space) {
            joined.push_back(' ');
        }
        joined += normalized;
    }
    return joined;
}

} // namespace voicetyper::domain
