// Contract for the Silero VAD segmenter: the seam's behaviour that does not need
// the model is always checked, and the parts that do need it are opt-in through
// VOICETYPER_VAD_MODEL (a missing model skips them with an explicit SKIP line
// instead of passing silently).
//
// Prints "silero-vad-contract: OK" on success; CTest asserts that marker.

#include "asr/silero_segmenter.hpp"
#include "domain/audio_wav.hpp"
#include "domain/speech_segments.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using voicetyper::asr::SileroSegmenter;
using voicetyper::domain::ErrorCode;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

std::string environment(const char* name)
{
    const auto* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

/// A model that cannot be opened is an error with a precise reason - never a
/// silent fall back to another detector.
void check_missing_model_is_reported()
{
    const auto empty = SileroSegmenter::open({});
    check(empty.is_error() && empty.error().code() == ErrorCode::invalid_argument,
        "an empty model path is invalid_argument");

    const auto missing = SileroSegmenter::open("/nonexistent/ggml-silero-v6.2.0.bin");
    check(missing.is_error() && missing.error().code() == ErrorCode::not_found,
        "a missing model file is not_found");
}

/// Silence must not invent speech, and the frame data the chunk planner needs has
/// to be reported (and cleared by reset).
void check_silence_and_frame_data(const std::string& model)
{
    auto segmenter = SileroSegmenter::open(model);
    check(segmenter.is_ok(), "the shipped model opens");
    if (segmenter.is_error()) {
        std::cerr << "  reason: " << segmenter.error().message() << '\n';
        return;
    }

    // Exact zeros are a degenerate input no microphone produces; the realistic
    // case is a very quiet noise floor (-60 dBFS), which is what trimming meets.
    std::vector<float> quiet(16000 * 3, 0.0f);
    for (std::size_t index = 0; index < quiet.size(); ++index) {
        quiet[index] = 0.001f * static_cast<float>((index % 17) - 8) / 8.0f;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto segments = segmenter.value()->detect_speech_no_reset(quiet);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    std::cout << "  quiet floor: " << segments.size() << " segment(s) in " << elapsed.count()
              << " ms for 3.0 s of audio\n";
    check(segments.empty(), "a quiet noise floor contains no speech");

    const auto probabilities = segmenter.value()->last_frame_probabilities();
    check(!probabilities.empty(), "per-frame probabilities are reported");
    check(segmenter.value()->probability_frame_seconds() > 0.0, "the frame length is reported");
    if (!probabilities.empty()) {
        check(segmenter.value()->probability_frame_seconds() > 0.02
                  && segmenter.value()->probability_frame_seconds() < 0.05,
            "the frame length matches the documented 512 samples at 16 kHz");
    }

    segmenter.value()->reset();
    check(segmenter.value()->last_frame_probabilities().empty(),
        "reset clears the frame probabilities of the previous session");

    // Cost of one pass, for the latency budget: the product runs the detector once
    // for trimming and (on GigaAM) a second time for chunk planning.
    for (const double seconds : {3.0, 10.0, 30.0}) {
        std::vector<float> quiet(static_cast<std::size_t>(seconds * 16000), 0.0f);
        for (std::size_t index = 0; index < quiet.size(); ++index) {
            quiet[index] = 0.001f * static_cast<float>((index % 17) - 8) / 8.0f;
        }
        segmenter.value()->reset();
        const auto pass_start = std::chrono::steady_clock::now();
        static_cast<void>(segmenter.value()->detect_speech_no_reset(quiet));
        const auto pass_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pass_start);
        std::cout << "  one pass over " << seconds << " s of audio: " << pass_ms.count() << " ms\n";
    }
}

/// With a real speech clip the segmenter must find speech, and every segment must
/// be a positive interval in seconds.
void check_real_speech_is_found(const std::string& model, const std::string& fixture)
{
    const auto audio = voicetyper::domain::read_wav_file(fixture);
    if (audio.is_error()) {
        std::cout << "silero-vad-contract: SKIP fixture unreadable (" << fixture << ")\n";
        return;
    }
    auto segmenter = SileroSegmenter::open(model);
    if (segmenter.is_error()) {
        check(false, "the model opens for the fixture run");
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto segments = segmenter.value()->detect_speech_no_reset(audio.value());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    std::cout << "  speech clip: " << segments.size() << " segment(s) in " << elapsed.count()
              << " ms for " << (static_cast<double>(audio.value().size()) / 16000.0) << " s of audio\n";
    check(!segments.empty(), "a real speech clip yields at least one speech segment");
    for (const auto& segment : segments) {
        check(segment.end_seconds > segment.start_seconds, "a segment has a positive duration");
    }
    check(!segmenter.value()->last_frame_probabilities().empty(),
        "a real clip reports per-frame probabilities");
}

} // namespace

int main()
{
    check_missing_model_is_reported();

    const std::string model = environment("VOICETYPER_VAD_MODEL");
    if (model.empty()) {
        std::cout << "silero-vad-contract: SKIP (set VOICETYPER_VAD_MODEL to ggml-silero-v6.2.0.bin)\n";
    } else {
        check_silence_and_frame_data(model);
        const std::string fixture = environment("VOICETYPER_VAD_FIXTURE");
        if (!fixture.empty()) {
            check_real_speech_is_found(model, fixture);
        }
        // S4 evidence: trim the fixture with the very code the product runs and
        // write the result out, so the same clip can be transcribed with and
        // without trimming and the two transcripts compared.
        const std::string trimmed_out = environment("VOICETYPER_VAD_TRIMMED_OUT");
        if (!fixture.empty() && !trimmed_out.empty()) {
            const auto audio = voicetyper::domain::read_wav_file(fixture);
            auto segmenter = SileroSegmenter::open(model);
            if (audio.is_ok() && segmenter.is_ok()) {
                const auto detected = segmenter.value()->detect_speech_no_reset(audio.value());
                const auto grown = voicetyper::domain::extend_segments_with_energy(
                    audio.value(), detected, 16000);
                const auto trimmed = voicetyper::domain::trim_silence_to_segments(
                    audio.value(), grown, 16000);
                std::string bytes;
                const auto written = voicetyper::domain::write_wav_pcm16(trimmed.samples, bytes);
                if (written.is_ok()) {
                    std::ofstream out(trimmed_out, std::ios::binary);
                    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                    std::cout << "  trimmed: segments=" << trimmed.speech_segments
                              << " removed_leading=" << trimmed.removed_leading
                              << " removed_trailing=" << trimmed.removed_trailing
                              << " out_seconds="
                              << (static_cast<double>(trimmed.samples.size()) / 16000.0) << "\n";
                } else {
                    check(false, "S4: the trimmed audio could be written");
                }
            }
        }
    }

    if (failures != 0) {
        std::cerr << "silero-vad-contract: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "silero-vad-contract: OK\n";
    return 0;
}
