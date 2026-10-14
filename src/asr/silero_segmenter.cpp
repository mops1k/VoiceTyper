#include "asr/silero_segmenter.hpp"

#include "domain/error.hpp"

// The only translation unit that knows whisper.cpp's VAD ABI, exactly like
// whisper_native.cpp is the only one that knows its decoding ABI.
#include "whisper.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace voicetyper::asr {

namespace {

/// One Silero frame is 512 samples at 16 kHz (31.9 ms) in whisper.cpp.
constexpr double kFrameSamples = 512.0;
constexpr double kSampleRate = 16000.0;

std::string path_text(const std::filesystem::path& path)
{
    return path.string();
}

} // namespace

struct SileroSegmenter::Impl {
    whisper_vad_context* context = nullptr;
    whisper_vad_params params{};
    std::vector<float> probabilities;
    /// The context is not thread-safe; the machine streams from a worker and the
    /// trimming decorator runs after the stop, so the calls are serialized here
    /// rather than relying on the callers.
    std::mutex mutex;
};

SileroSegmenter::SileroSegmenter(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

SileroSegmenter::~SileroSegmenter()
{
    if (impl_ != nullptr && impl_->context != nullptr) {
        whisper_vad_free(impl_->context);
    }
}

domain::Result<std::unique_ptr<SileroSegmenter>> SileroSegmenter::open(
    const std::filesystem::path& model_path)
{
    if (model_path.empty()) {
        return domain::Result<std::unique_ptr<SileroSegmenter>>::failure(
            domain::ErrorCode::invalid_argument, "silero: empty model path");
    }
    std::error_code error;
    if (!std::filesystem::exists(model_path, error) || std::filesystem::is_directory(model_path, error)) {
        return domain::Result<std::unique_ptr<SileroSegmenter>>::failure(
            domain::ErrorCode::not_found, "silero: model not found: " + path_text(model_path));
    }

    auto impl = std::make_unique<Impl>();
    whisper_vad_context_params context_params = whisper_vad_default_context_params();
    context_params.use_gpu = false;
    // ONE thread, deliberately. The VAD graph is one 512-sample frame wide, so the
    // arithmetic per frame is tiny and the per-op thread synchronization dominates
    // the cost. Measured 2026-10-14 on the target machine (i7-1165G7, 4 vCPU):
    // one pass over 30 s of audio took 10264 ms with n_threads =
    // hardware_concurrency - 938 chunks, about 11 ms of wall time per 512-sample
    // chunk, which no amount of arithmetic explains. This file used to claim
    // 1369 ms for the same 30 s. The cost is also latency-bound, so it exploded
    // under CPU contention: a 26.35 s dictation paid 72025 ms here and left the
    // user waiting more than a minute, while the recognizer itself needed 4.9 s.
    // The silero-vad contract prints the per-pass cost, so a regression is visible.
    context_params.n_threads = 1;
    impl->context = whisper_vad_init_from_file_with_params(path_text(model_path).c_str(), context_params);
    if (impl->context == nullptr) {
        return domain::Result<std::unique_ptr<SileroSegmenter>>::failure(
            domain::ErrorCode::io_failure, "silero: could not load the model: " + path_text(model_path));
    }
    // Documented defaults: threshold 0.50, speech >= 250 ms, silence >= 100 ms,
    // 30 ms padding. max_speech_duration_s stays at its "no forced split" value:
    // this segmenter answers "where is speech", the chunk policy is the caller's.
    impl->params = whisper_vad_default_params();
    // The detector pads each speech span by 30 ms by default. 150 ms is deliberate:
    // a soft onset or a fading tail must fall inside the span, because the trimming
    // step is not allowed to cut a phrase (Alexander, 2026-10-07).
    impl->params.speech_pad_ms = 150;

    return domain::Result<std::unique_ptr<SileroSegmenter>>(
        std::unique_ptr<SileroSegmenter>(new SileroSegmenter(std::move(impl))));
}

std::vector<audio::SpeechSegment> SileroSegmenter::detect_speech_no_reset(
    const std::vector<float>& samples)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->probabilities.clear();
    if (impl_->context == nullptr || samples.empty()) {
        return {};
    }
    if (!whisper_vad_detect_speech_no_reset(
            impl_->context, samples.data(), static_cast<int>(samples.size()))) {
        return {};
    }

    const int count = whisper_vad_n_probs(impl_->context);
    const float* probabilities = whisper_vad_probs(impl_->context);
    if (count > 0 && probabilities != nullptr) {
        impl_->probabilities.assign(probabilities, probabilities + count);
    }

    whisper_vad_segments* segments = whisper_vad_segments_from_probs(impl_->context, impl_->params);
    if (segments == nullptr) {
        return {};
    }
    const int n_segments = whisper_vad_segments_n_segments(segments);
    std::vector<audio::SpeechSegment> result;
    if (n_segments > 0) {
        result.reserve(static_cast<std::size_t>(n_segments));
    }
    for (int index = 0; index < n_segments; ++index) {
        // whisper.cpp reports centiseconds, the seam speaks seconds.
        const double start =
            static_cast<double>(whisper_vad_segments_get_segment_t0(segments, index)) / 100.0;
        const double end =
            static_cast<double>(whisper_vad_segments_get_segment_t1(segments, index)) / 100.0;
        if (end > start) {
            result.push_back(audio::SpeechSegment{start, end});
        }
    }
    whisper_vad_free_segments(segments);
    return result;
}

void SileroSegmenter::reset()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->probabilities.clear();
    if (impl_->context != nullptr) {
        whisper_vad_reset_state(impl_->context);
    }
}

std::vector<float> SileroSegmenter::last_frame_probabilities() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->probabilities;
}

double SileroSegmenter::probability_frame_seconds() const noexcept
{
    return kFrameSamples / kSampleRate;
}

} // namespace voicetyper::asr
