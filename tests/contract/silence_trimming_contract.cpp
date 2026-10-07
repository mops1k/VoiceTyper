// Contract for the silence-trimming decorator: the audio an engine receives when
// a dictation is surrounded by silence, when the detector finds nothing, and when
// the engine fails. Portable and model-free - the segmenter is a deterministic
// double, so the rule is checked in every configuration.
//
// Prints "silence-trimming-contract: OK" on success; CTest asserts that marker.

#include "domain/silence_trimming_port.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using voicetyper::domain::CancellationToken;
using voicetyper::domain::Result;
using voicetyper::domain::SampleBuffer;
using voicetyper::domain::SessionOptions;
using voicetyper::domain::SilenceTrimmingPort;
using voicetyper::domain::SpeechSegment;
using voicetyper::domain::SpeechSegmenter;
using voicetyper::domain::TranscriptionPort;

constexpr std::size_t kRate = 16000;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

/// A segmenter that answers with a fixed segment list: the decorator's rule is
/// what is under test, not the detector.
class FixedSegmenter final : public SpeechSegmenter {
public:
    explicit FixedSegmenter(std::vector<SpeechSegment> segments)
        : segments_(std::move(segments))
    {
    }

    [[nodiscard]] std::vector<SpeechSegment> detect_speech_no_reset(
        const std::vector<float>&) override
    {
        return segments_;
    }

    void reset() override { ++resets; }

    int resets = 0;

private:
    std::vector<SpeechSegment> segments_;
};

/// A port that records what it was handed and answers with a fixed text.
class RecordingPort final : public TranscriptionPort {
public:
    [[nodiscard]] Result<std::string> transcribe(const SampleBuffer& audio,
        const SessionOptions& options,
        const CancellationToken&) override
    {
        received = audio.samples();
        received_map = options.speech_map;
        return std::string("текст");
    }

    std::vector<float> received;
    voicetyper::domain::SpeechMap received_map;
};

class FailingPort final : public TranscriptionPort {
public:
    [[nodiscard]] Result<std::string> transcribe(const SampleBuffer&,
        const SessionOptions&,
        const CancellationToken&) override
    {
        return Result<std::string>::failure(
            voicetyper::domain::ErrorCode::engine_unavailable, "no engine");
    }
};

std::vector<float> silence(std::size_t samples)
{
    return std::vector<float>(samples, 0.0f);
}

std::vector<float> tone(std::size_t samples)
{
    std::vector<float> out(samples);
    for (std::size_t index = 0; index < samples; ++index) {
        out[index] = 0.2f;
    }
    return out;
}

SampleBuffer buffer_of(const std::vector<float>& samples)
{
    SampleBuffer buffer(samples.size());
    static_cast<void>(buffer.append(samples));
    return buffer;
}

/// Two seconds of silence, three seconds of speech, two seconds of silence: the
/// engine must receive the speech with the documented margins only. The clip is
/// deliberately longer than the short-recording threshold, below which the
/// detector is not asked at all.
void check_surrounding_silence_is_dropped()
{
    RecordingPort engine;
    FixedSegmenter segmenter({SpeechSegment{2.0, 5.0}});
    SilenceTrimmingPort trimming(engine, segmenter);

    std::vector<float> audio = silence(kRate * 2);
    const auto speech = tone(kRate * 3);
    audio.insert(audio.end(), speech.begin(), speech.end());
    const auto tail = silence(kRate * 2);
    audio.insert(audio.end(), tail.begin(), tail.end());

    const auto result = trimming.transcribe(buffer_of(audio), SessionOptions{}, CancellationToken{});
    check(result.is_ok(), "trimming: a successful transcription stays successful");
    check(segmenter.resets == 1, "trimming: the segmenter is reset once per call");
    // 0.25 s kept before the speech and 0.25 s after it: 3.5 s out of 7 s.
    check(engine.received.size() == static_cast<std::size_t>(3.5 * kRate),
        "trimming: the engine receives speech plus the documented margins");
    const auto& report = trimming.last_report();
    check(report.removed_leading == static_cast<std::size_t>(1.75 * kRate),
        "trimming: the leading silence is reported");
    check(report.removed_trailing == static_cast<std::size_t>(1.75 * kRate),
        "trimming: the trailing silence is reported");
    check(report.speech_segments == 1, "trimming: the speech segment count is reported");
    check(!report.unchanged, "trimming: the report says the audio was changed");
    check(!report.skipped_short, "trimming: a long recording is not skipped");
}

/// A detector that finds nothing must not cost the user a dictation.
void check_no_segments_keeps_the_audio()
{
    RecordingPort engine;
    FixedSegmenter segmenter({});
    SilenceTrimmingPort trimming(engine, segmenter);

    const auto audio = tone(kRate * 2);
    const auto result = trimming.transcribe(buffer_of(audio), SessionOptions{}, CancellationToken{});
    check(result.is_ok(), "no segments: the transcription still happens");
    check(engine.received.size() == audio.size(),
        "no segments: the engine receives the audio unchanged");
    check(trimming.last_report().unchanged, "no segments: the report says unchanged");
    check(trimming.last_report().speech_segments == 0, "no segments: zero segments reported");
}

/// A short recording is not trimmed at all: the detection pass would cost more
/// than the silence it could remove, and a clip this short is inside every
/// engine's window, so nobody else runs a detector for it either.
void check_short_recording_skips_the_detector()
{
    RecordingPort engine;
    FixedSegmenter segmenter({SpeechSegment{0.1, 0.2}});
    SilenceTrimmingPort trimming(engine, segmenter);

    const auto audio = tone(static_cast<std::size_t>(2.0 * kRate));
    const auto result = trimming.transcribe(buffer_of(audio), SessionOptions{}, CancellationToken{});
    check(result.is_ok(), "short: the transcription still happens");
    check(segmenter.resets == 0, "short: the detector is never asked");
    check(engine.received.size() == audio.size(), "short: the audio is passed through");
    check(trimming.last_report().skipped_short, "short: the report says it was skipped");
    check(trimming.last_report().unchanged, "short: the audio is reported unchanged");
}

/// The detector's answer travels with the audio, so the engine does not pay for a
/// second pass over the same dictation.
void check_the_speech_map_is_forwarded()
{
    RecordingPort engine;
    FixedSegmenter segmenter({SpeechSegment{2.0, 5.0}});
    SilenceTrimmingPort trimming(engine, segmenter);

    std::vector<float> audio = silence(kRate * 2);
    const auto speech = tone(kRate * 3);
    audio.insert(audio.end(), speech.begin(), speech.end());
    const auto tail = silence(kRate * 2);
    audio.insert(audio.end(), tail.begin(), tail.end());

    static_cast<void>(trimming.transcribe(buffer_of(audio), SessionOptions{}, CancellationToken{}));
    const auto& map = engine.received_map;
    check(!map.empty(), "map: the engine receives the detector's answer");
    check(map.segments.size() == 1, "map: one speech segment is forwarded");
    // 1.75 s of the leading silence is dropped, so the speech now starts at 0.25 s
    // and ends at 3.25 s of the 3.5 s the engine received.
    check(map.segments.size() == 1 && map.segments.front().start_seconds > 0.2
              && map.segments.front().start_seconds < 0.3,
        "map: the segment is mapped into the trimmed timeline");
    check(map.segments.size() == 1 && map.segments.front().end_seconds > 3.2
              && map.segments.front().end_seconds < 3.3,
        "map: the segment end is mapped as well");
}

/// An empty buffer is passed through untouched (nothing to trim).
void check_empty_audio_is_passed_through()
{
    RecordingPort engine;
    FixedSegmenter segmenter({SpeechSegment{0.0, 1.0}});
    SilenceTrimmingPort trimming(engine, segmenter);
    const auto result = trimming.transcribe(SampleBuffer(0), SessionOptions{}, CancellationToken{});
    check(result.is_ok(), "empty audio: the transcription still happens");
    check(engine.received.empty(), "empty audio: the engine receives nothing");
    check(segmenter.resets == 0, "empty audio: the detector is not asked at all");
}

/// A failure travels through the decorator unchanged.
void check_failure_is_preserved()
{
    FailingPort failing;
    FixedSegmenter segmenter({SpeechSegment{0.0, 1.0}});
    SilenceTrimmingPort trimming(failing, segmenter);
    const auto result = trimming.transcribe(buffer_of(tone(kRate)), SessionOptions{}, CancellationToken{});
    check(result.is_error(), "failure: a failure is still a failure");
    check(result.is_error() && result.error().code() == voicetyper::domain::ErrorCode::engine_unavailable,
        "failure: the code is preserved");
}

} // namespace

int main()
{
    check_surrounding_silence_is_dropped();
    check_no_segments_keeps_the_audio();
    check_short_recording_skips_the_detector();
    check_the_speech_map_is_forwarded();
    check_empty_audio_is_passed_through();
    check_failure_is_preserved();

    if (failures != 0) {
        std::cerr << "silence-trimming-contract: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "silence-trimming-contract: OK\n";
    return 0;
}
