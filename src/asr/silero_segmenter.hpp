#pragma once

#include "domain/vad.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace voicetyper::asr {

/// Silero VAD through the pinned whisper.cpp (`whisper_vad_*`).
///
/// No second runtime and no second ggml: the symbols already live in the
/// whisper library the product links, and the documented model is exactly
/// `ggml-silero-v6.2.0.bin`, which the models page already installs.
///
/// Contract of the seam it implements (domain::SpeechSegmenter): segments are
/// absolute seconds from the start of the buffer the implementation was fed, and
/// reset() is called between recording sessions. On top of that it reports the
/// per-frame speech probabilities, which is what lets the GigaAM chunk planner
/// cut an over-long utterance at the quietest frame instead of its midpoint.
///
/// `whisper.h` is included only in the .cpp, exactly like whisper_native.cpp: the
/// ABI stays in one translation unit and the seam stays portable.
class SileroSegmenter final : public domain::SpeechSegmenter {
public:
    /// Opens the model file. A missing file is an explicit not_found error, never
    /// a silent switch to another detector.
    [[nodiscard]] static domain::Result<std::unique_ptr<SileroSegmenter>> open(
        const std::filesystem::path& model_path);

    ~SileroSegmenter() override;
    SileroSegmenter(const SileroSegmenter&) = delete;
    SileroSegmenter& operator=(const SileroSegmenter&) = delete;
    SileroSegmenter(SileroSegmenter&&) = delete;
    SileroSegmenter& operator=(SileroSegmenter&&) = delete;

    [[nodiscard]] std::vector<domain::SpeechSegment> detect_speech_no_reset(
        const std::vector<float>& samples) override;
    void reset() override;

    [[nodiscard]] std::vector<float> last_frame_probabilities() const override;
    [[nodiscard]] double probability_frame_seconds() const noexcept override;

private:
    struct Impl;
    explicit SileroSegmenter(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace voicetyper::asr
