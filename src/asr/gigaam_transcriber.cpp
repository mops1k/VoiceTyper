#include "asr/gigaam_transcriber.hpp"

#include "asr/engine_parameters.hpp"
#include "domain/audio_format.hpp"
#include "domain/audio_wav.hpp"
#include "domain/speech_segments.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace voicetyper::asr {
namespace {

/// Canonical WAV -> float samples. Same decoder as the other engines, so a bad
/// file fails identically everywhere.
domain::Result<std::vector<float>> samples_of(const domain::WavAudio& wav)
{
    if (!wav.has_samples()) {
        return domain::Result<std::vector<float>>::failure(
            domain::ErrorCode::invalid_argument, "the WAV carries no samples");
    }
    return domain::read_wav_pcm16(wav.file_view());
}

domain::Result<std::vector<float>> slices(
    const std::vector<float>& samples, std::size_t begin, std::size_t end)
{
    if (begin >= end || end > samples.size()) {
        return domain::Result<std::vector<float>>::failure(
            domain::ErrorCode::invalid_argument, "gigaam: invalid chunk range");
    }
    return std::vector<float>(samples.begin() + static_cast<std::ptrdiff_t>(begin),
                              samples.begin() + static_cast<std::ptrdiff_t>(end));
}

} // namespace

GigaamTranscriber::GigaamTranscriber(
    std::filesystem::path model_path,
    std::unique_ptr<TranscribeEngine> engine,
    domain::SpeechSegmenter* segmenter)
    : model_path_(std::move(model_path))
    , engine_(std::move(engine))
    , segmenter_(segmenter)
{
}

platform::EngineCapabilities GigaamTranscriber::capabilities() const noexcept
{
    // GigaAM-v3 e2e-rnnt takes no language hint, no prompt, no temperature, no
    // previous context and no candidate count: punctuation and casing come from
    // the model's own e2e head. Declaring this is what keeps "the engine ignored
    // my setting" a reported fact instead of a silent surprise.
    platform::EngineCapabilities capabilities;
    capabilities.supports_language_override = false;
    capabilities.supports_prompt = false;
    capabilities.supports_temperature = false;
    capabilities.supports_previous_context = false;
    capabilities.supports_best_of = false;
    capabilities.supports_streaming_partials = false;
    return capabilities;
}

double GigaamTranscriber::max_audio_seconds() const
{
    return engine_ == nullptr ? 0.0 : engine_->max_audio_seconds();
}

std::size_t GigaamTranscriber::last_chunk_count() const
{
    std::lock_guard<std::mutex> lock(inference_mutex_);
    return last_chunk_count_;
}

domain::Status GigaamTranscriber::warmup()
{
    if (engine_ == nullptr) {
        return domain::Status::failure(
            domain::ErrorCode::engine_unavailable, "gigaam: the runtime is not open");
    }
    if (engine_->is_ready()) {
        return domain::Status::success();
    }
    const domain::CancellationToken none;
    return engine_->load_model(model_path_, none);
}

domain::Status GigaamTranscriber::deep_warmup(const domain::CancellationToken& cancellation)
{
    if (const auto status = warmup(); status.is_error()) {
        return status;
    }
    // 0.3 s of silence, the shared warm-up shape of the other engines: the first
    // real dictation must not pay for graph/allocator warm-up.
    const auto wav = build_warmup_wav();
    if (wav.is_error()) {
        return wav.status();
    }
    const auto samples = samples_of(wav.value());
    if (samples.is_error()) {
        return samples.status();
    }
    const auto text = engine_->transcribe(samples.value(), cancellation);
    if (text.is_error()) {
        return text.status();
    }
    return domain::Status::success();
}

domain::Result<std::string> GigaamTranscriber::transcribe(
    const domain::WavAudio& wav,
    const platform::TranscriptionRequest& request,
    const domain::CancellationToken& cancellation)
{
    static_cast<void>(request); // capabilities() declares every request field unsupported.

    if (engine_ == nullptr) {
        return domain::Result<std::string>::failure(
            domain::ErrorCode::engine_unavailable, "gigaam: the runtime is not open");
    }
    const auto samples = samples_of(wav);
    if (samples.is_error()) {
        return domain::Result<std::string>::failure(samples.code(), samples.error().message());
    }
    const auto& audio = samples.value();

    // Serialize inference: the native session is not thread-safe, exactly like
    // the other two engines.
    std::lock_guard<std::mutex> lock(inference_mutex_);

    if (!engine_->is_ready()) {
        return domain::Result<std::string>::failure(
            domain::ErrorCode::model_not_ready, "gigaam: warmup() has not completed");
    }
    if (auto cancelled = domain::check_cancelled(cancellation); cancelled.is_error()) {
        return domain::Result<std::string>::failure(cancelled.error().code(), cancelled.error().message());
    }

    const double window = engine_->max_audio_seconds();
    const double duration = static_cast<double>(audio.size()) / static_cast<double>(domain::kTargetSampleRate);

    // Within the model window (or with no declared window) the clip is decoded in
    // one pass: no chunking, no seam, no risk of a different result.
    if (window <= 0.0 || duration <= window) {
        const auto text = engine_->transcribe(audio, cancellation);
        last_chunk_count_ = text.is_error() ? 0 : 1;
        return text;
    }

    if (segmenter_ == nullptr) {
        // Cutting at an arbitrary sample would damage a word. Without a
        // segmenter the honest answer is a refusal, not a silent guess.
        return domain::Result<std::string>::failure(
            domain::ErrorCode::out_of_range,
            "gigaam: the dictation is longer than the model window ("
                + std::to_string(static_cast<int>(window))
                + " s) and no speech segmenter is bound to cut it at a pause");
    }

    // The chunk budget keeps a margin below the declared window so the cut itself
    // never lands on the limit.
    const double budget = std::max(1.0, window - 1.0);
    segmenter_->reset();
    const auto segments = segmenter_->detect_speech_no_reset(audio);
    // Probabilities are not part of the portable segmenter contract, so a forced
    // cut inside one over-long utterance uses the deterministic midpoint rather
    // than the quietest frame (frame_seconds 0 selects that fallback). Extending
    // the segmenter contract with probabilities is a recorded follow-up, not
    // something to fake here.
    const auto chunks = domain::plan_speech_chunks(
        segments, {}, 0.0, audio.size(), domain::kTargetSampleRate, budget);
    if (chunks.empty()) {
        return domain::Result<std::string>::failure(
            domain::ErrorCode::out_of_range,
            "gigaam: the dictation is longer than the model window and no chunk could be planned");
    }

    // Every planned chunk is decoded on its own, including the single-chunk case:
    // the planned slice is the speech span (plus no padding), so a long dictation
    // is also trimmed of the silence the segmenter did not call speech.
    std::vector<std::string> parts;
    parts.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        if (auto cancelled = domain::check_cancelled(cancellation); cancelled.is_error()) {
            return domain::Result<std::string>::failure(cancelled.error().code(), cancelled.error().message());
        }
        const auto slice = slices(audio, chunk.begin_sample, chunk.end_sample);
        if (slice.is_error()) {
            return domain::Result<std::string>::failure(slice.error().code(), slice.error().message());
        }
        const auto text = engine_->transcribe(slice.value(), cancellation);
        if (text.is_error()) {
            // A failed chunk fails the dictation: joining the rest would deliver
            // a transcript that silently lost a sentence.
            last_chunk_count_ = 0;
            return text;
        }
        parts.push_back(text.value());
    }
    last_chunk_count_ = parts.size();
    if (parts.size() == 1) {
        return parts.front();
    }
    return domain::join_transcripts(parts);
}

domain::Result<std::unique_ptr<platform::Transcriber>> make_gigaam_engine(
    const std::filesystem::path& dll_path,
    const std::filesystem::path& model_path,
    domain::SpeechSegmenter* segmenter)
{
    const domain::CancellationToken none;
    auto engine = open_transcribe_engine(dll_path, 0, none);
    if (engine.is_error()) {
        return domain::Result<std::unique_ptr<platform::Transcriber>>::failure(
            engine.error().code(), engine.error().message());
    }
    std::unique_ptr<platform::Transcriber> transcriber(
        new GigaamTranscriber(model_path, std::move(engine).value(), segmenter));
    return transcriber;
}

platform::EngineAvailability probe_gigaam_for_registry(
    domain::TranscriptionEngine engine,
    const std::filesystem::path& model_path,
    const std::filesystem::path& dll_path)
{
    platform::EngineAvailability state;
    state.engine = engine;
    const auto probe = probe_transcribe_runtime(dll_path);
    if (!probe.usable) {
        state.reason = probe.library_loaded
            ? platform::EngineAvailabilityReason::abi_mismatch
            : (dll_path.empty() || !std::filesystem::exists(dll_path))
                ? platform::EngineAvailabilityReason::platform_unsupported
                : platform::EngineAvailabilityReason::native_library_missing;
        return state;
    }
    if (!std::filesystem::is_regular_file(model_path)) {
        state.reason = platform::EngineAvailabilityReason::model_missing;
        return state;
    }
    state.available = true;
    state.reason = platform::EngineAvailabilityReason::available;
    return state;
}

} // namespace voicetyper::asr
