#include "audio/speech_segments.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace voicetyper::audio {
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
    std::size_t& compressed,
    std::vector<KeptSpan>& kept_spans)
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
    if (copied > 0) {
        kept_spans.push_back(KeptSpan{begin, begin + copied, out.size()});
    }
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

/// RMS of every whole frame of `frame_samples`; the fractional tail is ignored.
std::vector<double> frame_energies(const std::vector<float>& samples, std::size_t frame_samples)
{
    if (frame_samples == 0) {
        return {};
    }
    const std::size_t frames = samples.size() / frame_samples;
    std::vector<double> energy(frames, 0.0);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        double sum = 0.0;
        const auto begin = frame * frame_samples;
        for (std::size_t index = begin; index < begin + frame_samples; ++index) {
            sum += static_cast<double>(samples[index]) * static_cast<double>(samples[index]);
        }
        energy[frame] = std::sqrt(sum / static_cast<double>(frame_samples));
    }
    return energy;
}

/// The noise floor of a recording, from the recording itself: the median of the
/// quietest third of its frames. A fixed threshold would break on a loud or a
/// quiet microphone; every caller that decides "this region is silence" derives
/// its floor here so the two answers cannot disagree.
///
/// It is capped at a tenth of the loudest frame, because a clip with almost no
/// room noise - a dictation that is speech from its first sample, the very case
/// "the beginning of the phrase is missing" is about - would otherwise report a
/// floor equal to the speech level and find no sound at all.
double noise_floor_of(std::vector<double> energy)
{
    if (energy.empty()) {
        return 0.0;
    }
    const double loudest = *std::max_element(energy.begin(), energy.end());
    std::sort(energy.begin(), energy.end());
    return std::min(energy[energy.size() / 3], loudest * 0.1);
}

} // namespace

TrimReport trim_silence_to_segments(
    const std::vector<float>& samples,
    const std::vector<SpeechSegment>& segments,
    std::size_t sample_rate,
    double margin_seconds,
    double max_silence_seconds,
    double gap_silence_seconds,
    double max_removed_leading_seconds,
    double max_removed_trailing_seconds)
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
    auto begin = first_speech > margin ? first_speech - margin : 0;
    auto end = std::min(samples.size(), last_speech + margin);
    // The cap comes before anything else: however wrong the detector is, a trim may
    // not remove more than the policy allows from either side. Keep-more is always the
    // safer direction - the engine ignores silence, the user cannot ignore a lost word.
    const auto leading_cap = static_cast<std::size_t>(
        std::max(0.0, max_removed_leading_seconds) * static_cast<double>(sample_rate));
    const auto trailing_cap = static_cast<std::size_t>(
        std::max(0.0, max_removed_trailing_seconds) * static_cast<double>(sample_rate));
    if (begin > leading_cap) {
        begin = leading_cap;
    }
    if (samples.size() > trailing_cap && end < samples.size() - trailing_cap) {
        end = samples.size() - trailing_cap;
    }
    if (end <= begin) {
        report.samples = samples;
        return report;
    }

    report.removed_leading = begin;
    report.removed_trailing = samples.size() - end;

    std::vector<float> out;
    out.reserve(end - begin);
    std::size_t cursor = begin;
    bool first_span = true;
    for (const auto& span : spans) {
        const auto span_begin = std::max(cursor, std::min(end, sample_at(span.start_seconds, sample_rate)));
        const auto span_end = std::max(span_begin, std::min(end, sample_at(span.end_seconds, sample_rate)));
        if (first_span) {
            // The leading edge is copied WHOLE, not compressed: it is the protective
            // margin the policy promises, and compressing it would quietly undo the
            // removal cap - the audio the cap deliberately kept would be squeezed out
            // again, which is exactly how a mistimed detector cost a user his phrase.
            if (span_begin > cursor) {
                report.kept_spans.push_back(KeptSpan{cursor, span_begin, out.size()});
                out.insert(out.end(), samples.begin() + static_cast<std::ptrdiff_t>(cursor),
                    samples.begin() + static_cast<std::ptrdiff_t>(span_begin));
            }
            first_span = false;
        } else {
            append_silence(out, samples, cursor, span_begin, max_silence, gap,
                report.compressed_pause_samples, report.kept_spans);
        }
        if (span_end > span_begin) {
            report.kept_spans.push_back(KeptSpan{span_begin, span_end, out.size()});
            out.insert(out.end(), samples.begin() + static_cast<std::ptrdiff_t>(span_begin),
                       samples.begin() + static_cast<std::ptrdiff_t>(span_end));
        }
        cursor = span_end;
    }
    // The trailing edge is copied whole for the same reason as the leading one; only
    // the pauses BETWEEN phrases are compressed.
    if (end > cursor) {
        report.kept_spans.push_back(KeptSpan{cursor, end, out.size()});
        out.insert(out.end(), samples.begin() + static_cast<std::ptrdiff_t>(cursor),
                   samples.begin() + static_cast<std::ptrdiff_t>(end));
    }

    report.samples = std::move(out);
    return report;
}

std::vector<SpeechSegment> extend_segments_with_energy(
    const std::vector<float>& samples, const std::vector<SpeechSegment>& segments,
    std::size_t sample_rate, double frame_seconds, double absolute_speech_frame_rms)
{
    if (samples.empty() || segments.empty() || sample_rate == 0 || frame_seconds <= 0.0) {
        return segments;
    }
    const auto frame_samples = static_cast<std::size_t>(frame_seconds * static_cast<double>(sample_rate));
    if (frame_samples == 0) {
        return segments;
    }
    const std::vector<double> energy = frame_energies(samples, frame_samples);
    const std::size_t frames = energy.size();
    if (frames == 0) {
        return segments;
    }

    // The noise floor is the median of the quietest third of the frames: a floor
    // derived from the recording itself, not a fixed threshold that a loud or a
    // quiet microphone would break.
    const double floor = noise_floor_of(energy);
    // 2x the floor is "still silence"; anything above it is protected. The ratio is
    // deliberately generous: keeping half a second of room noise costs nothing,
    // dropping the first word of a phrase costs the user's trust (measured
    // 09.11.2026 on a -40 dBFS microphone array, where 3x the floor let a soft
    // onset look like silence).
    const double relative = floor * 2.0 + 1e-6;
    // A relative rule alone cannot protect a speaker who is quiet ALL the time: when
    // the voice itself is only a few dB above the room, the proportional threshold
    // lands inside the speech and the end of the phrase is dropped. Anything at or
    // above the absolute level is sound whatever the floor says - in the scale of
    // these samples, which is why the caller passes it instead of assuming 16 kHz
    // recording level.
    const double threshold = std::min(relative, std::max(0.0, absolute_speech_frame_rms));

    const auto frame_of = [frame_samples, frames](double seconds, std::size_t sample_rate_for) {
        const auto sample = static_cast<std::size_t>(std::max(0.0, seconds) * static_cast<double>(sample_rate_for));
        return std::min(frames - 1, sample / frame_samples);
    };

    // The first frame of the recording that carries sound. It is protected wherever a
    // detector placed its first boundary: a phrase that starts within the first few
    // hundred milliseconds is exactly the case "one or two words are missing" was
    // reported for.
    std::size_t first_energy = frames;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        if (energy[frame] > threshold) {
            first_energy = frame;
            break;
        }
    }

    std::vector<SpeechSegment> grown;
    grown.reserve(segments.size());
    for (const auto& segment : segments) {
        std::size_t begin = frame_of(segment.start_seconds, sample_rate);
        std::size_t end = frame_of(segment.end_seconds, sample_rate);
        // Walk outwards over the frames that carry sound. One sub-threshold frame is
        // stepped over when the frame beyond it is loud again: a breath or a plosive
        // closure inside a word must not end the protected span.
        while (begin > 0) {
            if (energy[begin - 1] > threshold) {
                --begin;
                continue;
            }
            if (begin >= 2 && energy[begin - 2] > threshold) {
                begin -= 2;
                continue;
            }
            break;
        }
        while (end + 1 < frames) {
            if (energy[end + 1] > threshold) {
                ++end;
                continue;
            }
            if (end + 2 < frames && energy[end + 2] > threshold) {
                end += 2;
                continue;
            }
            break;
        }
        if (first_energy < begin) {
            begin = first_energy;
        }
        grown.push_back(SpeechSegment{static_cast<double>(begin * frame_samples) / static_cast<double>(sample_rate),
            static_cast<double>((end + 1) * frame_samples) / static_cast<double>(sample_rate)});
    }
    return grown;
}

double leading_onset_seconds(const std::vector<float>& samples, std::size_t sample_rate,
    double frame_seconds, double floor_ratio)
{
    if (samples.empty() || sample_rate == 0 || frame_seconds <= 0.0) {
        return -1.0;
    }
    const auto frame_samples = static_cast<std::size_t>(frame_seconds * static_cast<double>(sample_rate));
    if (frame_samples == 0) {
        return -1.0;
    }
    const std::vector<double> energy = frame_energies(samples, frame_samples);
    if (energy.empty()) {
        return -1.0;
    }
    const double threshold = noise_floor_of(energy) * std::max(0.0, floor_ratio) + 1e-6;
    for (std::size_t frame = 0; frame < energy.size(); ++frame) {
        if (energy[frame] > threshold) {
            return static_cast<double>(frame) * frame_seconds;
        }
    }
    return -1.0;
}

SpeechMap map_speech_map(const SpeechMap& source, const std::vector<KeptSpan>& kept_spans,
    std::size_t output_samples, std::size_t sample_rate)
{
    SpeechMap mapped;
    mapped.frame_seconds = source.frame_seconds;
    if (kept_spans.empty()) {
        return mapped;
    }
    const double rate = sample_rate == 0 ? 1.0 : static_cast<double>(sample_rate);

    const auto forward = [&kept_spans](std::size_t source_sample) -> std::size_t {
        for (const auto& span : kept_spans) {
            if (source_sample >= span.source_begin && source_sample < span.source_end) {
                return span.output_begin + (source_sample - span.source_begin);
            }
        }
        if (source_sample < kept_spans.front().source_begin) {
            return kept_spans.front().output_begin;
        }
        return kept_spans.back().output_begin + kept_spans.back().source_size();
    };
    const auto backward = [&kept_spans](std::size_t output_sample) -> std::size_t {
        for (const auto& span : kept_spans) {
            if (output_sample >= span.output_begin && output_sample < span.output_begin + span.source_size()) {
                return span.source_begin + (output_sample - span.output_begin);
            }
        }
        return kept_spans.back().source_begin + kept_spans.back().source_size();
    };

    for (const auto& segment : source.segments) {
        const auto begin = static_cast<std::size_t>(std::max(0.0, segment.start_seconds) * rate);
        const auto end = static_cast<std::size_t>(std::max(0.0, segment.end_seconds) * rate);
        const double mapped_begin = static_cast<double>(forward(begin)) / rate;
        const double mapped_end = static_cast<double>(forward(end)) / rate;
        if (mapped_end > mapped_begin) {
            mapped.segments.push_back(SpeechSegment{mapped_begin, mapped_end});
        }
    }

    if (source.frame_seconds > 0.0 && !source.probabilities.empty() && output_samples > 0) {
        const auto frame_samples = static_cast<std::size_t>(source.frame_seconds * rate);
        if (frame_samples > 0) {
            const std::size_t frames = output_samples / frame_samples;
            mapped.probabilities.reserve(frames);
            for (std::size_t index = 0; index < frames; ++index) {
                const std::size_t output_sample = index * frame_samples + frame_samples / 2;
                const std::size_t source_frame = backward(output_sample) / frame_samples;
                mapped.probabilities.push_back(source_frame < source.probabilities.size()
                        ? source.probabilities[source_frame]
                        : source.probabilities.back());
            }
        }
    }
    return mapped;
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

} // namespace voicetyper::audio
