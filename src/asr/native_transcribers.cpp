#include "asr/native_transcribers.hpp"

#include "asr/engine_parameters.hpp"
#include "domain/audio_wav.hpp"

#include <utility>

namespace voicetyper::asr {
namespace {

/// Canonical WAV -> float samples. One decoder for both engines, so a bad file
/// fails the same way regardless of which engine is selected.
domain::Result<std::vector<float>> samples_of(const domain::WavAudio& wav)
{
    if (!wav.has_samples()) {
        return domain::Result<std::vector<float>>::failure(
            domain::ErrorCode::invalid_argument, "the WAV carries no samples");
    }
    return domain::read_wav_pcm16(wav.file_view());
}

} // namespace

// ---------------------------------------------------------------------------
// Whisper
// ---------------------------------------------------------------------------

WhisperTranscriber::WhisperTranscriber(
    std::filesystem::path model_path, std::unique_ptr<WhisperNativeContext> context)
    : model_path_(std::move(model_path)), context_(std::move(context))
{
}

platform::EngineCapabilities WhisperTranscriber::capabilities() const noexcept
{
    return whisper_capabilities();
}

domain::Status WhisperTranscriber::warmup()
{
    // The context is already loaded: construction is the warm-up, exactly as in
    // the .NET engine where Warmup() is the factory call. A second call is a
    // no-op instead of loading a second context, which is one of the C# races.
    return domain::Status::success();
}

domain::Status WhisperTranscriber::deep_warmup(const domain::CancellationToken& cancellation)
{
    if (cancellation.is_cancellation_requested()) {
        return domain::Status::failure(domain::ErrorCode::cancelled, "cancelled before warm-up");
    }
    const auto wav = build_warmup_wav();
    if (wav.is_error()) {
        return wav.status();
    }
    const auto samples = samples_of(wav.value());
    if (samples.is_error()) {
        return samples.status();
    }
    WhisperDecodeOptions options;
    options.temperature = 0.0f;
    options.best_of = 1;
    options.no_context = true;
    options.carry_initial_prompt = false;
    std::lock_guard<std::mutex> lock(inference_mutex_);
    const auto warm = context_->transcribe(
        samples.value().data(), samples.value().size(), options, cancellation);
    if (warm.is_error() && warm.code() == domain::ErrorCode::cancelled) {
        return warm.status();
    }
    return domain::Status::success(); // a warm-up transcript is never delivered
}

domain::Result<std::string> WhisperTranscriber::transcribe(
    const domain::WavAudio& wav,
    const platform::TranscriptionRequest& request,
    const domain::CancellationToken& cancellation)
{
    if (cancellation.is_cancellation_requested()) {
        return domain::Result<std::string>::failure(domain::ErrorCode::cancelled, "cancelled before decoding");
    }
    if (!context_->is_ready()) {
        return domain::Result<std::string>::failure(domain::ErrorCode::model_not_ready, "the model is not loaded");
    }
    const auto samples = samples_of(wav);
    if (samples.is_error()) {
        return domain::Result<std::string>::failure(samples.code(), samples.error().message());
    }
    const auto normalized = normalize(request);
    if (normalized.is_error()) {
        return domain::Result<std::string>::failure(normalized.code(), normalized.error().message());
    }

    WhisperDecodeOptions options;
    options.language = normalized.value().language_code.c_str();
    options.prompt = normalized.value().initial_prompt;
    options.carry_initial_prompt = normalized.value().carry_initial_prompt;
    options.temperature = normalized.value().temperature;
    options.best_of = normalized.value().best_of;
    options.no_context = normalized.value().no_context;

    std::lock_guard<std::mutex> lock(inference_mutex_);
    auto result = context_->transcribe(
        samples.value().data(), samples.value().size(), options, cancellation);
    if (cancellation.is_cancellation_requested()) {
        return domain::Result<std::string>::failure(domain::ErrorCode::cancelled, "transcription cancelled");
    }
    return result;
}

// ---------------------------------------------------------------------------
// Parakeet
// ---------------------------------------------------------------------------

ParakeetTranscriber::ParakeetTranscriber(
    std::filesystem::path model_path, std::unique_ptr<platform::ParakeetRuntime> runtime)
    : model_path_(std::move(model_path)), runtime_(std::move(runtime))
{
}

platform::EngineCapabilities ParakeetTranscriber::capabilities() const noexcept
{
    return parakeet_capabilities();
}

domain::Status ParakeetTranscriber::warmup()
{
    if (!runtime_->is_open()) {
        return domain::Status::failure(domain::ErrorCode::engine_unavailable, "the runtime is not open");
    }
    if (runtime_->is_ready()) {
        return domain::Status::success();
    }
    const domain::CancellationToken none;
    return runtime_->load_model(model_path_, none);
}

domain::Status ParakeetTranscriber::deep_warmup(const domain::CancellationToken& cancellation)
{
    if (cancellation.is_cancellation_requested()) {
        return domain::Status::failure(domain::ErrorCode::cancelled, "cancelled before warm-up");
    }
    const auto warm = warmup();
    if (warm.is_error()) {
        return warm;
    }
    const auto wav = build_warmup_wav();
    if (wav.is_error()) {
        return wav.status();
    }
    const auto samples = samples_of(wav.value());
    if (samples.is_error()) {
        return samples.status();
    }
    std::lock_guard<std::mutex> lock(inference_mutex_);
    // D1: this call is not interruptible; the token is polled around it only.
    const auto warm_result = runtime_->transcribe_pcm(
        samples.value().data(),
        samples.value().size(),
        16000,
        platform::kParakeetDecoderDefault,
        std::string(),
        cancellation);
    if (warm_result.is_error() && warm_result.code() == domain::ErrorCode::cancelled) {
        return warm_result.status();
    }
    return domain::Status::success();
}

domain::Result<std::string> ParakeetTranscriber::transcribe(
    const domain::WavAudio& wav,
    const platform::TranscriptionRequest& request,
    const domain::CancellationToken& cancellation)
{
    if (cancellation.is_cancellation_requested()) {
        return domain::Result<std::string>::failure(domain::ErrorCode::cancelled, "cancelled before decoding");
    }
    if (!runtime_->is_ready()) {
        return domain::Result<std::string>::failure(domain::ErrorCode::model_not_ready, "the model is not loaded");
    }
    const auto samples = samples_of(wav);
    if (samples.is_error()) {
        return domain::Result<std::string>::failure(samples.code(), samples.error().message());
    }
    // Parakeet applies no request field; the normalized request is still built
    // so an out-of-range parameter is rejected identically for both engines.
    const auto normalized = normalize(request);
    if (normalized.is_error()) {
        return domain::Result<std::string>::failure(normalized.code(), normalized.error().message());
    }
    const auto parakeet_request = normalize_parakeet(request);

    std::lock_guard<std::mutex> lock(inference_mutex_);
    auto result = runtime_->transcribe_pcm(
        samples.value().data(),
        samples.value().size(),
        parakeet_request.sample_rate,
        parakeet_request.decoder,
        parakeet_request.target_language,
        cancellation);
    if (cancellation.is_cancellation_requested()) {
        return domain::Result<std::string>::failure(domain::ErrorCode::cancelled, "transcription cancelled");
    }
    return result;
}

// ---------------------------------------------------------------------------
// Factories for NativeEngineRegistry
// ---------------------------------------------------------------------------

domain::Result<std::unique_ptr<platform::Transcriber>> make_whisper_engine(
    const std::filesystem::path& model_path, int requested_threads)
{
    WhisperContextOptions context_options;
    context_options.threads = clamp_whisper_threads(requested_threads);
    const domain::CancellationToken none;
    auto context = WhisperNativeContext::load(model_path, context_options, none);
    if (context.is_error()) {
        return domain::Result<std::unique_ptr<platform::Transcriber>>::failure(
            context.code(), context.error().message());
    }
    return std::unique_ptr<platform::Transcriber>(
        new WhisperTranscriber(model_path, std::move(context.value())));
}

domain::Result<std::unique_ptr<platform::Transcriber>> make_parakeet_engine(
    const std::filesystem::path& dll_path, const std::filesystem::path& model_path)
{
    const domain::CancellationToken none;
    auto runtime = platform::ParakeetRuntime::open(dll_path, none);
    if (runtime.is_error()) {
        return domain::Result<std::unique_ptr<platform::Transcriber>>::failure(
            runtime.code(), runtime.error().message());
    }
    return std::unique_ptr<platform::Transcriber>(
        new ParakeetTranscriber(model_path, std::move(runtime.value())));
}

platform::EngineAvailability probe_parakeet_for_registry(
    domain::TranscriptionEngine engine, const std::filesystem::path& model_path)
{
    const auto probe = platform::probe_parakeet_runtime(platform::parakeet_library_beside_executable());
    platform::EngineAvailability state;
    state.engine = engine;
    state.abi_version = probe.abi_version > 0 ? std::optional<int>(probe.abi_version) : std::nullopt;
    if (state.abi_version.has_value() && *state.abi_version != platform::kParakeetAbiVersion) {
        state.reason = platform::EngineAvailabilityReason::abi_mismatch;
        return state;
    }
    state.reason = probe.reason;
    if (!probe.usable) {
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
