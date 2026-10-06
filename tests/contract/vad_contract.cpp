#include "domain/vad.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

std::vector<float> chunk(float value, std::size_t count = 4000)
{
    return std::vector<float>(count, value);
}

void check_no_speech()
{
    using namespace voicetyper::domain;
    int resets = 0;
    CallbackSpeechSegmenter segmenter([&resets](const std::vector<float>&) {
        (void)resets;
        return std::vector<SpeechSegment>{};
    });
    SilenceAutoStopDetector detector(segmenter, 1.2);
    VadStopDecision decision;
    for (int i = 0; i < 19; ++i) {
        decision = detector.process(chunk(0.0f));
        check(!decision.stop, "no-speech detector waits before 5 s");
    }
    decision = detector.process(chunk(0.0f));
    check(decision.stop && decision.reason == VadStopReason::no_speech_idle, "no-speech stop fires at 5 s");
    detector.reset();
    check(detector.total_fed_seconds() == 0.0, "reset clears fed time");
}

void check_trailing_silence()
{
    using namespace voicetyper::domain;
    int call = 0;
    CallbackSpeechSegmenter segmenter([&call](const std::vector<float>&) {
        if (call++ == 0) {
            return std::vector<SpeechSegment>{{0.0, 0.25}};
        }
        return std::vector<SpeechSegment>{};
    });
    SilenceAutoStopDetector detector(segmenter, 1.2);
    auto decision = detector.process(chunk(0.5f));
    check(!decision.stop && detector.hold_seconds() > 0.59, "speech chunk starts phrase and hold is 0.6 s");
    decision = detector.process(chunk(0.0f));
    check(!decision.stop, "short silence does not stop");
    for (int i = 0; i < 4; ++i) {
        decision = detector.process(chunk(0.0f));
    }
    check(decision.stop && decision.reason == VadStopReason::trailing_silence, "trailing silence stops after threshold");
}

void check_relative_absolute_and_quiet()
{
    using namespace voicetyper::domain;
    int call = 0;
    CallbackSpeechSegmenter segmenter([&call](const std::vector<float>&) {
        if (call++ == 0) {
            // Relative end is normalized to the absolute stream clock.
            return std::vector<SpeechSegment>{{0.0, 0.25}};
        }
        return std::vector<SpeechSegment>{};
    });
    SilenceAutoStopDetector detector(segmenter, 1.2);
    (void)detector.process(chunk(0.02f));
    check(detector.total_fed_seconds() == 0.25, "chunk duration is 250 ms");
    const auto decision = detector.process(chunk(0.0f));
    check(!decision.stop, "quiet chunk is not an immediate stop");

    CallbackSpeechSegmenter empty([](const std::vector<float>&) { return std::vector<SpeechSegment>{}; });
    SilenceAutoStopDetector low_threshold(empty, 0.0);
    check(low_threshold.hold_seconds() == 0.3, "hold clamps at 0.3 s");
    SilenceAutoStopDetector high_threshold(empty, 100.0);
    check(high_threshold.hold_seconds() == 0.8, "hold clamps at 0.8 s");
}

} // namespace

namespace {

/// One second of a chosen RMS, so the segmenter is exercised on a signal whose
/// level is known exactly.
std::vector<float> tone(double seconds, double amplitude, double frequency = 220.0)
{
    std::vector<float> samples(static_cast<std::size_t>(16000.0 * seconds));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(amplitude * std::sin(2.0 * 3.14159265358979 * frequency * static_cast<double>(i) / 16000.0));
    }
    return samples;
}

void check_energy_segmenter()
{
    using voicetyper::domain::EnergySpeechSegmenter;
    EnergySpeechSegmenter segmenter;

    check(segmenter.detect_speech_no_reset({}).empty(), "no samples produce no segments");

    // A quiet floor first, then loud speech: exactly what a real session looks
    // like, and the case the adaptive floor exists for.
    auto stream = tone(0.6, 0.001);
    const auto quiet = tone(0.8, 0.30);
    stream.insert(stream.end(), quiet.begin(), quiet.end());
    const auto tail = tone(0.4, 0.001);
    stream.insert(stream.end(), tail.begin(), tail.end());

    const auto segments = segmenter.detect_speech_no_reset(stream);
    check(!segments.empty(), "a loud burst after silence is detected as speech");
    if (!segments.empty()) {
        const auto& first = segments.front();
        check(first.start_seconds >= 0.5, "the segment starts after the quiet lead-in");
        check(first.end_seconds > first.start_seconds, "a segment has positive duration");
        check(first.end_seconds <= 1.8, "the segment does not claim the trailing silence");
    }
    check(segmenter.noise_floor() > 0.0, "the noise floor was established");

    segmenter.reset();
    check(segmenter.noise_floor() == 0.0, "reset clears the learned floor");

    // Constant loud input everywhere: there is no silence, so the whole buffer is
    // one open segment rather than a stream of false cuts.
    EnergySpeechSegmenter steady;
    const auto constant = steady.detect_speech_no_reset(tone(1.0, 0.25));
    check(constant.size() <= 1, "constant loud audio does not produce many segments");

    // Digital silence must not latch the floor at zero and then call everything
    // speech: the absolute escape hatch and the ratio both have to hold.
    EnergySpeechSegmenter silent;
    const auto nothing = silent.detect_speech_no_reset(std::vector<float>(16000, 0.0f));
    check(nothing.empty(), "digital silence produces no speech segments");
}

} // namespace

int main()
{
    check_no_speech();
    check_trailing_silence();
    check_relative_absolute_and_quiet();
    check_energy_segmenter();

    if (failures != 0) {
        std::cerr << "vad-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "vad-contract: OK\n";
    return 0;
}
