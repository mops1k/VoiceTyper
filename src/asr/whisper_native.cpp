// whisper.cpp binding. This is the only VoiceTyper translation unit that
// includes the upstream <whisper.h> (from the commit pinned in
// docs/migration/cpp/native-dependencies.json); every other file sees the
// standard-C++20 surface declared in whisper_native.hpp.
//
// No logging, no allocation policy and no engine choice happen here: the
// wrapper maps whisper.cpp's C API onto domain::Result/Error and nothing else.

#include "asr/whisper_native.hpp"

#include <whisper.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

namespace voicetyper::asr {
namespace {

/// Polls the caller's token from whisper.cpp's abort callback. Returning true
/// asks whisper.cpp to abandon the current compute, which surfaces as a
/// non-zero return from whisper_full().
bool on_compute_abort(void* user_data) noexcept
{
    if (user_data == nullptr) {
        return false;
    }
    const auto* token = static_cast<const CancellationToken*>(user_data);
    return token->is_cancellation_requested();
}

bool is_blank(const std::string& text) noexcept
{
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
}

std::string trimmed(std::string text)
{
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    if (first == text.end()) {
        return {};
    }
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    return std::string(first, last);
}

} // namespace

std::string_view whisper_pinned_commit() noexcept
{
    return VOICETYPER_WHISPER_PINNED_COMMIT;
}

std::string_view whisper_upstream_version() noexcept
{
    return VOICETYPER_WHISPER_UPSTREAM_VERSION;
}

std::string_view whisper_license() noexcept
{
    return VOICETYPER_WHISPER_LICENSE;
}

std::string_view whisper_repository() noexcept
{
    return VOICETYPER_WHISPER_REPOSITORY;
}

std::string_view whisper_archive_sha256() noexcept
{
    return VOICETYPER_WHISPER_ARCHIVE_SHA256;
}

int clamp_whisper_threads(int requested) noexcept
{
    if (requested <= 0) {
        // 0 means "unspecified"; the .NET engine derives this from the physical
        // core count, which lives in the CpuTopology seam. Until the caller
        // supplies that value, the smallest legal count is the honest default.
        return kWhisperMinThreads;
    }
    return std::clamp(requested, kWhisperMinThreads, kWhisperMaxThreads);
}

struct WhisperNativeContext::Impl {
    struct whisper_context* context = nullptr;
    int threads = kWhisperMinThreads;
    std::filesystem::path model_path;
};

WhisperNativeContext::WhisperNativeContext()
    : impl_(std::make_unique<Impl>())
{
}

WhisperNativeContext::~WhisperNativeContext()
{
    free_context();
}

bool WhisperNativeContext::is_ready() const noexcept
{
    return impl_ != nullptr && impl_->context != nullptr;
}

void WhisperNativeContext::free_context() noexcept
{
    if (impl_ != nullptr && impl_->context != nullptr) {
        whisper_free(impl_->context);
        impl_->context = nullptr;
    }
}

Result<std::unique_ptr<WhisperNativeContext>> WhisperNativeContext::load(
    const std::filesystem::path& model_path,
    const WhisperContextOptions& options,
    const CancellationToken& cancellation)
{
    if (auto cancelled = domain::check_cancelled(cancellation); cancelled.is_error()) {
        return Result<std::unique_ptr<WhisperNativeContext>>(cancelled.error());
    }
    if (model_path.empty()) {
        return Result<std::unique_ptr<WhisperNativeContext>>::failure(
            ErrorCode::invalid_argument, "whisper: empty model path");
    }

    std::error_code ec;
    const bool exists = std::filesystem::is_regular_file(model_path, ec);
    if (ec || !exists) {
        return Result<std::unique_ptr<WhisperNativeContext>>::failure(
            ErrorCode::not_found, "whisper: model file not found: " + model_path.string());
    }

    std::unique_ptr<WhisperNativeContext> owned(new WhisperNativeContext());
    owned->impl_->threads = clamp_whisper_threads(options.threads);
    owned->impl_->model_path = model_path;

    struct whisper_context_params context_params = whisper_context_default_params();
    // Offline CPU recognition only (parity row PRIV-01): no GPU backend, no
    // flash attention, and the thread count stays inside the .NET clamp.
    context_params.use_gpu = false;
    context_params.flash_attn = false;
    context_params.gpu_device = 0;
    context_params.dtw_token_timestamps = false;

    std::string model_text = model_path.string();
    struct whisper_context* context = whisper_init_from_file_with_params(model_text.c_str(), context_params);
    if (context == nullptr) {
        return Result<std::unique_ptr<WhisperNativeContext>>::failure(
            ErrorCode::corrupt_data, "whisper: model could not be loaded: " + model_text);
    }
    owned->impl_->context = context;

    if (auto cancelled = domain::check_cancelled(cancellation); cancelled.is_error()) {
        owned->free_context();
        return Result<std::unique_ptr<WhisperNativeContext>>(cancelled.error());
    }

    return owned;
}

Result<std::string> WhisperNativeContext::transcribe(
    const float* samples,
    std::size_t sample_count,
    const WhisperDecodeOptions& options,
    const CancellationToken& cancellation)
{
    if (!is_ready()) {
        return Result<std::string>::failure(
            ErrorCode::model_not_ready, "whisper: no model loaded");
    }
    if (samples == nullptr || sample_count == 0) {
        return Result<std::string>::failure(
            ErrorCode::invalid_argument, "whisper: empty PCM buffer");
    }
    if (auto cancelled = domain::check_cancelled(cancellation); cancelled.is_error()) {
        return Result<std::string>(cancelled.error());
    }

    struct whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    // Frozen .NET decoder configuration: greedy search, no temperature
    // fallback, no entropy/logprob fallback, no-speech threshold 0.6.
    params.n_threads = impl_->threads;
    params.strategy = WHISPER_SAMPLING_GREEDY;
    params.temperature = options.temperature;
    params.temperature_inc = kWhisperTemperatureIncrement;
    params.entropy_thold = kWhisperEntropyThreshold;
    params.logprob_thold = kWhisperLogProbThreshold;
    params.no_speech_thold = kWhisperNoSpeechThreshold;
    params.greedy.best_of = std::max(1, options.best_of);

    // contextOnPreviousText: the .NET engine only ever calls WithNoContext();
    // it stores no previous transcript, so neither does this wrapper.
    params.no_context = options.no_context;

    // Language: nullptr/""/"auto" is whisper.cpp's language detection, which is
    // what .NET RecognitionLanguage.Auto maps to.
    params.language = options.language;
    params.detect_language = false;

    if (!is_blank(options.prompt)) {
        params.initial_prompt = options.prompt.c_str();
        params.carry_initial_prompt = options.carry_initial_prompt;
    } else {
        params.initial_prompt = nullptr;
        params.carry_initial_prompt = false;
    }

    // The product never shows a streaming preview: nothing is printed, and all
    // output is read back through the segment API.
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.print_special = false;
    params.new_segment_callback = nullptr;
    params.new_segment_callback_user_data = nullptr;
    params.progress_callback = nullptr;
    params.progress_callback_user_data = nullptr;

    // Cooperative cancellation: whisper.cpp asks this before every ggml
    // computation, so cancelling during a long buffer stops the work.
    params.abort_callback = on_compute_abort;
    params.abort_callback_user_data = const_cast<CancellationToken*>(&cancellation);

    const int result = whisper_full(impl_->context, params, samples, static_cast<int>(sample_count));
    const bool was_cancelled = cancellation.is_cancellation_requested();
    // No whisper_free_params() here on purpose: that function does `delete` on
    // its argument and is only valid for the pointer returned by
    // whisper_full_default_params_by_ref(). The by-value default owns nothing -
    // initial_prompt is a borrowed pointer to options.prompt - so there is
    // nothing to release, and calling the free would corrupt the heap.

    if (result != 0) {
        if (was_cancelled) {
            return Result<std::string>::failure(
                ErrorCode::cancelled, "whisper: transcription cancelled");
        }
        return Result<std::string>::failure(
            ErrorCode::internal, "whisper: whisper_full failed with code " + std::to_string(result));
    }

    const int segment_count = whisper_full_n_segments(impl_->context);
    std::string text;
    for (int segment = 0; segment < segment_count; ++segment) {
        const char* segment_text = whisper_full_get_segment_text(impl_->context, segment);
        if (segment_text != nullptr) {
            text += segment_text;
        }
    }
    return trimmed(std::move(text));
}

} // namespace voicetyper::asr
