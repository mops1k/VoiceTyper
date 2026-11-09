// Contract for the speech-segment policy: silence trimming, chunk planning and
// transcript joining. Portable and deterministic - no model, no VAD, no OS - so
// it runs in every configuration, including a GUI-off build without whisper.cpp.
//
// Prints "speech-segments-contract: OK" on success; CTest asserts that marker.

#include "audio/speech_segments.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace voicetyper::audio;

using voicetyper::audio::SpeechChunk;
using voicetyper::audio::SpeechSegment;
using voicetyper::audio::join_transcripts;
using voicetyper::audio::plan_speech_chunks;
using voicetyper::audio::trim_silence_to_segments;

constexpr std::size_t kRate = 16000;
/// One VAD frame of 512 samples at 16 kHz, the Silero frame the VAD reports.
constexpr double kFrameSeconds = 512.0 / 16000.0;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

std::vector<float> tone(std::size_t samples, float amplitude)
{
    return std::vector<float>(samples, amplitude);
}

void append(std::vector<float>& target, const std::vector<float>& part)
{
    target.insert(target.end(), part.begin(), part.end());
}

/// silence-speech-silence, the shape a dictation really has.
std::vector<float> speech_in_silence(std::size_t lead, std::size_t speech, std::size_t tail)
{
    std::vector<float> samples;
    append(samples, tone(lead, 0.0f));
    append(samples, tone(speech, 0.25f));
    append(samples, tone(tail, 0.0f));
    return samples;
}

void check_trim_margins()
{
    // 3 s total: 1 s silence, 1 s speech, 1 s silence. The trimmer keeps 0.5 s of
    // silence on each side of the speech and drops the rest (the margin was raised
    // from 0.25 s on 2026-10-07 so that a soft onset or a fading tail survives).
    const auto samples = speech_in_silence(16000, 16000, 16000);
    const std::vector<SpeechSegment> segments{{1.0, 2.0}};
    const auto report = trim_silence_to_segments(samples, segments);
    check(report.speech_segments == 1, "one speech segment is reported");
    check(report.removed_leading == 8000, "leading silence is cut to the 0.5 s margin");
    check(report.removed_trailing == 8000, "trailing silence is cut to the 0.5 s margin");
    check(report.samples.size() == 32000, "the trimmed buffer is speech plus two margins");
    check(report.compressed_pause_samples == 0, "no internal pause was compressed");
    check(report.samples.front() == 0.0f && report.samples.back() == 0.0f, "both margins are silence");
    check(report.samples[8000] == 0.25f, "the speech starts right after the leading margin");
}

void check_trim_compresses_a_long_pause()
{
    // 5 s: speech 0..1, a 3 s pause, speech 4..5. The pause is compressed to the
    // frozen 0.3 s gap, and the first samples of the run are kept.
    std::vector<float> samples;
    append(samples, tone(16000, 0.25f));
    append(samples, tone(48000, 0.0f));
    append(samples, tone(16000, 0.25f));
    const std::vector<SpeechSegment> segments{{0.0, 1.0}, {4.0, 5.0}};
    const auto report = trim_silence_to_segments(samples, segments);
    check(report.speech_segments == 2, "two speech segments are reported");
    check(report.compressed_pause_samples == 48000 - 4800, "the 3 s pause is compressed to 0.3 s");
    check(report.samples.size() == 16000 + 4800 + 16000, "the trimmed buffer holds both utterances and the gap");
    check(report.removed_leading == 0 && report.removed_trailing == 0, "nothing outside the speech span is removed here");
}

void check_trim_normalizes_bad_segments()
{
    const auto samples = speech_in_silence(16000, 16000, 16000);
    // Clamped, sorted and merged: the -5..-1 span disappears, and the spans that
    // reach past the speech merge into one 1.0..3.0 utterance.
    const std::vector<SpeechSegment> unsorted{{2.0, 3.5}, {1.0, 2.0}, {-5.0, -1.0}, {2.5, 99.0}};
    const std::vector<SpeechSegment> already_normalized{{1.0, 3.0}};
    const auto normalized = trim_silence_to_segments(samples, unsorted);
    const auto reference = trim_silence_to_segments(samples, already_normalized);
    check(normalized.samples == reference.samples,
        "out-of-range and unsorted segments normalize to the same trim");
    check(normalized.speech_segments == 1, "clamping merges the touching spans into one");
}

void check_trim_keeps_audio_without_segments()
{
    const auto samples = speech_in_silence(16000, 16000, 16000);
    const auto report = trim_silence_to_segments(samples, {});
    check(report.speech_segments == 0, "no segment is reported");
    check(report.samples == samples, "audio is kept when the detector found no speech");
    check(report.removed_leading == 0 && report.removed_trailing == 0, "nothing is reported as removed");
}

void check_chunks_fit_one_window()
{
    const std::size_t total = 30 * kRate;
    const std::vector<SpeechSegment> segments{{2.0, 8.0}, {10.0, 12.0}};
    const auto chunks = plan_speech_chunks(segments, {}, kFrameSeconds, total, kRate, 25.0);
    check(chunks.size() == 1, "two close utterances fit one window");
    check(chunks[0].begin_sample == 2 * kRate && chunks[0].end_sample == 12 * kRate,
        "the single chunk spans the first speech start to the last speech end");
}

void check_chunks_split_at_a_pause()
{
    // Two utterances 15 s apart with a 10 s budget: they cannot share a chunk, so
    // the cut lands between them - never inside speech.
    const std::size_t total = 40 * kRate;
    const std::vector<SpeechSegment> segments{{1.0, 5.0}, {20.0, 24.0}};
    const auto chunks = plan_speech_chunks(segments, {}, kFrameSeconds, total, kRate, 10.0);
    check(chunks.size() == 2, "the two utterances need two chunks");
    check(chunks[0].begin_sample == 1 * kRate && chunks[0].end_sample == 5 * kRate,
        "the first chunk covers exactly the first utterance");
    check(chunks[1].begin_sample == 20 * kRate && chunks[1].end_sample == 24 * kRate,
        "the second chunk covers exactly the second utterance");
    for (const auto& chunk : chunks) {
        for (const auto& segment : segments) {
            const auto begin = static_cast<std::size_t>(segment.start_seconds * kRate);
            const auto end = static_cast<std::size_t>(segment.end_seconds * kRate);
            const bool whole = (begin >= chunk.begin_sample && end <= chunk.end_sample)
                || end <= chunk.begin_sample || begin >= chunk.end_sample;
            check(whole, "no chunk splits a speech segment");
        }
    }
}

void check_chunks_cut_one_long_utterance_at_the_quietest_frame()
{
    // A single 2.5 s utterance with a 1 s window. Frame 20 (sample 10240) is the
    // quietest point, so that is where the forced cut must land - deterministic,
    // inside the allowed range and never closer than kMinChunkSeconds to the start.
    const std::size_t total = 40000;
    const std::vector<SpeechSegment> segments{{0.0, 2.5}};
    std::vector<float> probabilities(80, 0.9f);
    probabilities[20] = 0.01f;
    const auto chunks = plan_speech_chunks(segments, probabilities, kFrameSeconds, total, kRate, 1.0);
    check(!chunks.empty(), "a long utterance still yields chunks");
    check(chunks.front().begin_sample == 0, "the first chunk starts at the utterance");
    check(chunks.front().end_sample == 20 * 512, "the forced cut lands on the quietest frame");
    check(chunks.front().size() <= kRate, "no chunk exceeds the window");
    for (const auto& chunk : chunks) {
        check(chunk.size() > 0, "no empty chunk is produced");
    }
    check(chunks.back().end_sample == total, "the chunks cover the utterance to its end");
}

void check_chunks_without_speech_cover_everything()
{
    const std::size_t total = 5 * kRate;
    const auto chunks = plan_speech_chunks({}, {}, kFrameSeconds, total, kRate, 25.0);
    check(chunks.size() == 1 && chunks[0].begin_sample == 0 && chunks[0].end_sample == total,
        "a recording without speech segments yields one chunk covering it");
    const auto zero = plan_speech_chunks({}, {}, kFrameSeconds, 0, kRate, 25.0);
    check(zero.empty(), "an empty recording yields no chunk");
}

void check_onset_protection()
{
    // Reported 09.11.2026: one or two words missing from the start of a dictation.
    // The shape that matters is a phrase that begins right after the recording starts,
    // with a breath (one sub-threshold frame) inside it, and a VAD that placed its
    // first boundary late inside the first word.
    const std::size_t lead = 13 * 480;      // 0.39 s of room silence
    const std::size_t speech1 = 20 * 480;   // 0.6 s of speech
    const std::size_t dip = 480;            // one quiet frame inside the phrase
    const std::size_t speech2 = 20 * 480;   // 0.6 s of speech
    const std::size_t tail = 33 * 480;
    std::vector<float> samples;
    append(samples, tone(lead, 0.0f));
    append(samples, tone(speech1, 0.25f));
    append(samples, tone(dip, 0.001f));
    append(samples, tone(speech2, 0.25f));
    append(samples, tone(tail, 0.0f));

    const std::vector<SpeechSegment> late_boundary{{1.02, 1.20}};
    const auto grown = extend_segments_with_energy(samples, late_boundary, kRate, 0.03);
    check(grown.size() == 1, "the guard answers one span per segment");
    check(grown.front().start_seconds <= 0.45, "a late VAD boundary cannot cut the phrase onset");
    check(grown.front().end_seconds >= 1.60, "one quiet frame inside a phrase does not end the span");

    // Whatever the detector says, the first sound of the recording stays protected:
    // this is the property "the beginning of the phrase is never dropped" rests on.
    const std::vector<SpeechSegment> wrong_boundary{{2.10, 2.40}};
    const auto rescued = extend_segments_with_energy(samples, wrong_boundary, kRate, 0.03);
    check(!rescued.empty() && rescued.front().start_seconds <= 0.45,
        "the first frame carrying energy is inside the protected span");
}

void check_trim_caps()
{
    // 10 s of room, 1 s of speech, 10 s of room. Reported 09.11.2026: a quiet
    // microphone plus an unsure detector cost the user five seconds of a phrase. The
    // policy cap means no detector's mistake can cost more than a second per side.
    std::vector<float> samples;
    append(samples, tone(10 * kRate, 0.0f));
    append(samples, tone(kRate, 0.25f));
    append(samples, tone(10 * kRate, 0.0f));
    const auto report = trim_silence_to_segments(samples, {SpeechSegment{10.0, 11.0}}, kRate);
    const std::size_t cap = static_cast<std::size_t>(kMaxTrimLeadingSeconds * static_cast<double>(kRate)) + 2 * 480;
    check(report.removed_leading <= cap, "the leading removal is capped");
    check(report.removed_trailing <= cap, "the trailing removal is capped");
    // The caps bound what a wrong detector can take; the long kept room still
    // compresses to the documented gap, so the result is speech plus that gap.
    check(report.samples.size() > static_cast<std::size_t>(1.0 * static_cast<double>(kRate)),
        "the speech survives the caps");
}

void check_quiet_speech_is_protected()
{
    // The speaker is quiet throughout and the room is not silent: the relative floor
    // lands inside the voice, so without the absolute rule the span would end at the
    // VAD's boundary and the rest of the phrase would be dropped.
    std::vector<float> samples;
    append(samples, tone(2 * kRate, 0.0035f)); // quiet voice, about -49 dBFS
    append(samples, tone(2 * kRate, 0.0020f)); // room noise loud enough to fool a ratio
    const auto grown = extend_segments_with_energy(samples, {SpeechSegment{0.2, 0.6}}, kRate, 0.03);
    check(!grown.empty() && grown.front().end_seconds >= 3.9,
        "a quiet phrase is not cut where a relative floor would stop");
}

void check_leading_onset()
{
    // A clip that starts inside a word: onset 0 is the signature of "the phrase began
    // before the capture did", the case no post-processing can repair.
    const auto starts_with_speech = tone(kRate, 0.25f);
    check(std::abs(leading_onset_seconds(starts_with_speech, kRate) - 0.0) < 1e-6,
        "audio that starts inside a word reports onset 0");

    const auto after_a_second = speech_in_silence(kRate, kRate / 2, kRate / 2);
    check(std::abs(leading_onset_seconds(after_a_second, kRate) - 1.0) < 1e-6,
        "the onset is the first block above the floor");

    const auto room_only = tone(2 * kRate, 0.0f);
    check(leading_onset_seconds(room_only, kRate) < 0.0, "silence alone reports no onset");
    check(leading_onset_seconds({}, kRate) < 0.0, "an empty recording reports no onset");
}

void check_join()
{
    check(join_transcripts({"привет", "мир"}) == "привет мир", "two parts join with one space");
    check(join_transcripts({"привет", "", "  ", "мир"}) == "привет мир", "empty parts disappear");
    check(join_transcripts({"слово-", "ещё"}) == "слово-ещё", "a hyphen seam gets no space");
    check(join_transcripts({"конец.", ",снова"}) == "конец.,снова", "punctuation seam gets no space");
    check(join_transcripts({"первая\n\tстрока", "вторая"}) == "первая строка вторая",
        "internal whitespace collapses to one space");
    check(join_transcripts({"  обрезано  "}) == "обрезано", "a single part is trimmed");
    check(join_transcripts({}) == "", "no parts yield an empty transcript");
}

} // namespace

int main()
{
    check_trim_margins();
    check_trim_compresses_a_long_pause();
    check_trim_normalizes_bad_segments();
    check_trim_keeps_audio_without_segments();
    check_chunks_fit_one_window();
    check_chunks_split_at_a_pause();
    check_chunks_cut_one_long_utterance_at_the_quietest_frame();
    check_chunks_without_speech_cover_everything();
    check_onset_protection();
    check_leading_onset();
    check_trim_caps();
    check_quiet_speech_is_protected();
    check_join();

    if (failures != 0) {
        std::cerr << "speech-segments-contract: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "speech-segments-contract: OK\n";
    return 0;
}
