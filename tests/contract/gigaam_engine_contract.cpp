// Contract for the GigaAM engine policy. Everything here runs against the
// portable TranscribeEngine seam with a fake, so it needs no DLL, no model and
// no network - on any host, including a GUI-off Linux build.
//
// What it proves is the part that is OURS rather than the library's:
//   * the model window is read from the engine, not hardcoded, and a dictation
//     inside it is decoded in exactly one pass;
//   * a longer dictation is cut at a pause and the parts are joined; a chunk that
//     fails fails the whole dictation instead of silently losing a sentence;
//   * without a speech segmenter a too-long dictation is REFUSED, never cut at an
//     arbitrary sample;
//   * no lazy load: transcribe() before warmup() is model_not_ready and touches
//     no native call;
//   * warmup() loads weights, deep_warmup() runs one short silence inference;
//   * capabilities() declares every request field unsupported, so "the engine
//     ignored my setting" stays visible.
//
// Prints "gigaam-engine-contract: OK" on success; CTest asserts that marker.

#include "asr/gigaam_transcriber.hpp"

#include "domain/audio_format.hpp"
#include "domain/audio_wav.hpp"
#include "domain/cancellation.hpp"
#include "domain/vad.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using voicetyper::asr::GigaamTranscriber;
using voicetyper::asr::TranscribeEngine;
using voicetyper::domain::ErrorCode;
using voicetyper::domain::WavAudio;

constexpr std::size_t kRate = 16000;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

WavAudio wav_of(std::size_t samples)
{
    const std::vector<float> audio(samples, 0.1f);
    std::string bytes;
    const auto status = voicetyper::domain::write_wav_pcm16(audio, bytes);
    if (status.is_error()) {
        ++failures;
        std::cerr << "FAIL could not build a WAV fixture\n";
    }
    std::vector<std::byte> data(bytes.size());
    std::memcpy(data.data(), bytes.data(), bytes.size());
    return WavAudio(std::move(data));
}

/// Records every call so the test can assert what the engine was asked to do.
class FakeEngine final : public TranscribeEngine {
public:
    bool ready = false;
    double window = 25.0;
    std::string text = "распознанный текст";
    std::filesystem::path loaded_path;
    int load_calls = 0;
    int transcribe_calls = 0;
    std::vector<std::size_t> call_sizes;
    /// 1-based call index that returns a failure / a cancellation; 0 = never.
    int fail_on_call = 0;
    int cancel_on_call = 0;

    [[nodiscard]] bool is_ready() const noexcept override { return ready; }
    [[nodiscard]] double max_audio_seconds() const override { return window; }
    [[nodiscard]] std::string last_error() const override { return {}; }

    voicetyper::domain::Status load_model(
        const std::filesystem::path& gguf_path, const voicetyper::domain::CancellationToken&) override
    {
        ++load_calls;
        loaded_path = gguf_path;
        ready = true;
        return voicetyper::domain::Status::success();
    }

    voicetyper::domain::Result<std::string> transcribe(
        const std::vector<float>& samples, const voicetyper::domain::CancellationToken& cancellation) override
    {
        ++transcribe_calls;
        call_sizes.push_back(samples.size());
        if (cancel_on_call == transcribe_calls) {
            return voicetyper::domain::Result<std::string>::failure(
                ErrorCode::cancelled, "fake cancellation");
        }
        if (fail_on_call == transcribe_calls) {
            return voicetyper::domain::Result<std::string>::failure(
                ErrorCode::unavailable, "fake failure");
        }
        if (cancellation.is_cancellation_requested()) {
            return voicetyper::domain::Result<std::string>::failure(
                ErrorCode::cancelled, "token was cancelled before the run");
        }
        return text + std::to_string(transcribe_calls);
    }

    void free_model() noexcept override { ready = false; }
};

struct Built {
    std::unique_ptr<GigaamTranscriber> transcriber;
    FakeEngine* engine = nullptr; // owned by the transcriber
};

Built make_transcriber(double window, voicetyper::domain::SpeechSegmenter* segmenter)
{
    auto engine = std::make_unique<FakeEngine>();
    engine->window = window;
    FakeEngine* raw = engine.get();
    Built built;
    built.transcriber = std::make_unique<GigaamTranscriber>(
        std::filesystem::path("gigaam-v3-e2e-rnnt-Q8_0.gguf"), std::move(engine), segmenter);
    built.engine = raw;
    return built;
}

std::unique_ptr<voicetyper::domain::CallbackSpeechSegmenter> segmenter_of(
    const std::vector<voicetyper::domain::SpeechSegment>& segments)
{
    return std::make_unique<voicetyper::domain::CallbackSpeechSegmenter>(
        [segments](const std::vector<float>&) { return segments; });
}

void check_capabilities()
{
    auto built = make_transcriber(25.0, nullptr);
    const auto capabilities = built.transcriber->capabilities();
    check(!capabilities.supports_language_override, "the language hint is not claimed");
    check(!capabilities.supports_prompt, "the terms dictionary prompt is not claimed");
    check(!capabilities.supports_temperature, "temperature is not claimed");
    check(!capabilities.supports_previous_context, "previous context is not claimed");
    check(!capabilities.supports_best_of, "bestOf is not claimed");
    check(!capabilities.supports_streaming_partials, "no streaming preview is claimed");
    check(built.transcriber->engine() == voicetyper::domain::TranscriptionEngine::gigaam,
        "the transcriber reports the gigaam engine");
}

void check_no_lazy_load()
{
    auto built = make_transcriber(25.0, nullptr);
    const voicetyper::platform::TranscriptionRequest request;
    const auto result = built.transcriber->transcribe(wav_of(2 * kRate), request, {});
    check(result.is_error() && result.error().code() == ErrorCode::model_not_ready,
        "transcribe before warmup is model_not_ready");
    check(built.engine->transcribe_calls == 0, "no native inference happened before warmup");
    check(built.engine->load_calls == 0, "transcribe did not lazily load the model");
}

void check_warmup_and_deep_warmup()
{
    auto built = make_transcriber(25.0, nullptr);
    const auto warm = built.transcriber->warmup();
    check(warm.is_ok(), "warmup loads the model");
    check(built.engine->load_calls == 1, "warmup called load_model exactly once");
    check(built.transcriber->is_ready(), "the engine is ready after warmup");
    const auto again = built.transcriber->warmup();
    check(again.is_ok() && built.engine->load_calls == 1, "a second warmup does not reload");
    const auto deep = built.transcriber->deep_warmup({});
    check(deep.is_ok(), "deep warmup succeeds");
    check(built.engine->transcribe_calls == 1 && built.engine->call_sizes.front() == 4800,
        "deep warmup runs one 0.3 s silence inference");
}

void check_within_window_is_one_pass()
{
    auto built = make_transcriber(25.0, nullptr);
    built.transcriber->warmup();
    const voicetyper::platform::TranscriptionRequest request;
    const auto result = built.transcriber->transcribe(wav_of(5 * kRate), request, {});
    check(result.is_ok(), "a clip inside the window succeeds");
    check(built.engine->transcribe_calls == 1, "a clip inside the window is one pass");
    check(built.engine->call_sizes.front() == 5 * kRate, "the whole clip is handed over unchanged");
    check(built.transcriber->last_chunk_count() == 1, "one chunk is reported");
}

void check_long_dictation_is_cut_at_pauses_and_joined()
{
    // Window 5 s -> 4 s budget. Two utterances 5 s apart cannot share a chunk.
    auto segmenter = segmenter_of({{1.0, 3.0}, {8.0, 10.0}});
    auto built = make_transcriber(5.0, segmenter.get());
    built.transcriber->warmup();
    const voicetyper::platform::TranscriptionRequest request;
    const auto result = built.transcriber->transcribe(wav_of(12 * kRate), request, {});
    check(result.is_ok(), "a longer dictation succeeds by chunking");
    check(built.engine->transcribe_calls == 2, "two chunks are decoded");
    check(built.engine->call_sizes.size() == 2
            && built.engine->call_sizes[0] == 2 * kRate && built.engine->call_sizes[1] == 2 * kRate,
        "each chunk covers exactly one utterance, without the surrounding silence");
    check(built.transcriber->last_chunk_count() == 2, "two chunks are reported");
    check(result.value() == "распознанный текст1 распознанный текст2",
        "the partial transcripts are joined with one space");
}

void check_long_dictation_without_segmenter_is_refused()
{
    auto built = make_transcriber(5.0, nullptr);
    built.transcriber->warmup();
    const voicetyper::platform::TranscriptionRequest request;
    const auto result = built.transcriber->transcribe(wav_of(12 * kRate), request, {});
    check(result.is_error() && result.error().code() == ErrorCode::out_of_range,
        "without a segmenter a too-long dictation is refused, not cut blindly");
    check(built.engine->transcribe_calls == 0, "no native inference ran for the refused dictation");
}

void check_window_is_read_from_the_engine()
{
    // A model that declares no window must never be chunked.
    auto segmenter = segmenter_of({{0.0, 12.0}});
    auto built = make_transcriber(0.0, segmenter.get());
    built.transcriber->warmup();
    const voicetyper::platform::TranscriptionRequest request;
    const auto result = built.transcriber->transcribe(wav_of(12 * kRate), request, {});
    check(result.is_ok() && built.engine->transcribe_calls == 1,
        "an engine without a declared window gets the clip in one pass");
    check(built.transcriber->max_audio_seconds() == 0.0, "the unbounded window is reported as 0");
}

void check_failure_and_cancellation()
{
    {
        auto segmenter = segmenter_of({{1.0, 3.0}, {8.0, 10.0}});
        auto built = make_transcriber(5.0, segmenter.get());
        built.transcriber->warmup();
        built.engine->fail_on_call = 2;
        const auto result = built.transcriber->transcribe(
            wav_of(12 * kRate), voicetyper::platform::TranscriptionRequest{}, {});
        check(result.is_error() && result.error().code() == ErrorCode::unavailable,
            "a failed chunk fails the whole dictation");
        check(built.transcriber->last_chunk_count() == 0, "no chunk count is published for a failed dictation");
    }
    {
        auto segmenter = segmenter_of({{1.0, 3.0}, {8.0, 10.0}});
        auto built = make_transcriber(5.0, segmenter.get());
        built.transcriber->warmup();
        built.engine->cancel_on_call = 2;
        const auto result = built.transcriber->transcribe(
            wav_of(12 * kRate), voicetyper::platform::TranscriptionRequest{}, {});
        check(result.is_error() && result.error().code() == ErrorCode::cancelled,
            "a cancelled chunk surfaces as cancelled");
    }
    {
        auto built = make_transcriber(25.0, nullptr);
        built.transcriber->warmup();
        voicetyper::domain::CancellationSource source;
        source.request_cancellation();
        const auto result = built.transcriber->transcribe(
            wav_of(2 * kRate), voicetyper::platform::TranscriptionRequest{}, source.token());
        check(result.is_error() && result.error().code() == ErrorCode::cancelled,
            "a pre-cancelled token is observed before any native call");
        check(built.engine->transcribe_calls == 0, "no native inference ran for a cancelled request");
    }
}

void check_empty_wav_is_rejected()
{
    auto built = make_transcriber(25.0, nullptr);
    built.transcriber->warmup();
    const WavAudio empty;
    const auto result = built.transcriber->transcribe(
        empty, voicetyper::platform::TranscriptionRequest{}, {});
    check(result.is_error() && result.error().code() == ErrorCode::invalid_argument,
        "a WAV without samples is invalid_argument");
}

void check_missing_library_is_reported_not_substituted()
{
    // The factory must fail with a reason instead of returning another engine.
    auto engine = voicetyper::asr::make_gigaam_engine(
        std::filesystem::path("no-such-directory/libtranscribe.dll"),
        std::filesystem::path("gigaam-v3-e2e-rnnt-Q8_0.gguf"),
        nullptr);
    check(engine.is_error(), "a missing runtime library fails engine creation");
    if (engine.is_error()) {
        const auto code = engine.error().code();
        check(code == ErrorCode::unavailable || code == ErrorCode::unsupported,
            "the failure is unavailable (missing library) or unsupported (wrong host)");
    }
}

} // namespace

int main()
{
    check_capabilities();
    check_no_lazy_load();
    check_warmup_and_deep_warmup();
    check_within_window_is_one_pass();
    check_long_dictation_is_cut_at_pauses_and_joined();
    check_long_dictation_without_segmenter_is_refused();
    check_window_is_read_from_the_engine();
    check_failure_and_cancellation();
    check_empty_wav_is_rejected();
    check_missing_library_is_reported_not_substituted();

    if (failures != 0) {
        std::cerr << "gigaam-engine-contract: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "gigaam-engine-contract: OK\n";
    return 0;
}
