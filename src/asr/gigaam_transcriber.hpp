#pragma once

// GigaAM-v3 engine behind the frozen platform::Transcriber contract.
//
// Evidence: src/platform/api/transcriber.hpp (full final transcript, explicit
// warmup, no streaming preview, no silent engine substitution),
// native/transcribe/transcribe.h (the ABI, versioned next to the DLLs), and the
// spike numbers in the plan p_72adce780b6c (GigaAM-v3-e2e-rnnt Q8_0: 10.98 s
// clip -> ~1.4 s compute, 290 MB peak working set on the target machine).
//
// Two properties are deliberately ours and not the library's:
//   * the model window. GigaAM is trained on ~25 s utterances; the library
//     accepts longer audio with a warning and degraded accuracy. The product
//     does not accept degradation silently: a longer dictation is cut at a
//     pause and the parts are joined (src/domain/speech_segments.hpp).
//   * the warmup contract. warmup() loads weights, deep_warmup() runs one short
//     silence inference so the first real dictation is not the slow one; both
//     are inherited from the .NET engine convention and are required for a new
//     engine (see the engine-host lifecycle rules in src/asr/engine_host.hpp).

#include "asr/transcribe_engine.hpp"
#include "domain/error.hpp"
#include "domain/settings.hpp"
#include "domain/vad.hpp"
#include "platform/api/engine_registry.hpp"
#include "platform/api/transcriber.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace voicetyper::asr {

/// GigaAM engine. Inference is not interruptible inside the native decoder
/// beyond the library's own abort callback, so a cancel is observed between
/// decode steps; the observable rule stays "cancel wins, never a late result".
class GigaamTranscriber final : public platform::Transcriber {
public:
    /// `segmenter` is borrowed and may be null: without one, audio longer than
    /// the model window is reported as out_of_range instead of being cut at an
    /// arbitrary sample. A silent guess is not allowed here.
    GigaamTranscriber(
        std::filesystem::path model_path,
        std::unique_ptr<TranscribeEngine> engine,
        domain::SpeechSegmenter* segmenter);

    [[nodiscard]] domain::TranscriptionEngine engine() const noexcept override
    {
        return domain::TranscriptionEngine::gigaam;
    }
    [[nodiscard]] const std::filesystem::path& model_path() const noexcept override { return model_path_; }
    [[nodiscard]] platform::EngineCapabilities capabilities() const noexcept override;
    [[nodiscard]] bool is_ready() const noexcept override { return engine_ != nullptr && engine_->is_ready(); }

    domain::Status warmup() override;
    domain::Status deep_warmup(const domain::CancellationToken& cancellation) override;
    [[nodiscard]] domain::Result<std::string> transcribe(
        const domain::WavAudio& wav,
        const platform::TranscriptionRequest& request,
        const domain::CancellationToken& cancellation) override;

    /// The model's own declared input window in seconds (0 = unbounded).
    [[nodiscard]] double max_audio_seconds() const;

    /// Number of chunks the most recent transcribe() call needed; 1 means the
    /// audio fitted the window. For diagnostics and tests.
    [[nodiscard]] std::size_t last_chunk_count() const;

private:
    std::filesystem::path model_path_;
    std::unique_ptr<TranscribeEngine> engine_;
    domain::SpeechSegmenter* segmenter_ = nullptr;
    /// Mutable so the const diagnostics accessor can read the last chunk count
    /// under the same lock that guards inference.
    mutable std::mutex inference_mutex_;
    std::size_t last_chunk_count_ = 0;
};

/// Factory used by NativeEngineRegistry: opens the runtime library and wraps it.
/// The model is loaded by warmup(), not here, so a missing model is reported by
/// the readiness axis instead of failing engine creation.
[[nodiscard]] domain::Result<std::unique_ptr<platform::Transcriber>> make_gigaam_engine(
    const std::filesystem::path& dll_path,
    const std::filesystem::path& model_path,
    domain::SpeechSegmenter* segmenter);

/// Availability probe used by NativeEngineRegistry: inspects the library (no
/// model load) and reports a precise reason.
[[nodiscard]] platform::EngineAvailability probe_gigaam_for_registry(
    domain::TranscriptionEngine engine,
    const std::filesystem::path& model_path,
    const std::filesystem::path& dll_path);

} // namespace voicetyper::asr
