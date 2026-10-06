#pragma once

// The two real engines, adapted to the frozen platform::Transcriber contract.
//
// Both adapters do the same three things and nothing else:
//   * decode the WAV the converter produced into float samples,
//   * map a TranscriptionRequest through asr::normalize (the single place where
//     settings become engine parameters),
//   * serialize every native call on one mutex, because neither whisper.cpp nor
//     parakeet.cpp is re-entrant.
// The factory functions at the bottom are what the composition root hands to
// NativeEngineRegistry, so availability and creation stay in one place.

#include "asr/whisper_native.hpp"
#include "domain/error.hpp"
#include "platform/api/transcriber.hpp"
#include "platform/windows/parakeet_runtime.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace voicetyper::asr {

/// Whisper engine. Cancellation is real: the token reaches whisper.cpp's abort
/// callback, so a cancel stops between decoder segments instead of after the
/// whole clip.
class WhisperTranscriber final : public platform::Transcriber {
public:
    WhisperTranscriber(std::filesystem::path model_path, std::unique_ptr<WhisperNativeContext> context);

    [[nodiscard]] domain::TranscriptionEngine engine() const noexcept override
    {
        return domain::TranscriptionEngine::whisper;
    }
    [[nodiscard]] const std::filesystem::path& model_path() const noexcept override { return model_path_; }
    [[nodiscard]] platform::EngineCapabilities capabilities() const noexcept override;
    [[nodiscard]] bool is_ready() const noexcept override { return context_->is_ready(); }

    domain::Status warmup() override;
    domain::Status deep_warmup(const domain::CancellationToken& cancellation) override;
    [[nodiscard]] domain::Result<std::string> transcribe(
        const domain::WavAudio& wav,
        const platform::TranscriptionRequest& request,
        const domain::CancellationToken& cancellation) override;

private:
    std::filesystem::path model_path_;
    std::unique_ptr<WhisperNativeContext> context_;
    std::mutex inference_mutex_;
};

/// Parakeet engine. D1: inference is not interruptible. The token is checked
/// before and after the native call, and a late result is discarded, so the
/// observable behaviour is still "cancel wins" — it just takes as long as the
/// decode.
class ParakeetTranscriber final : public platform::Transcriber {
public:
    ParakeetTranscriber(std::filesystem::path model_path, std::unique_ptr<platform::ParakeetRuntime> runtime);

    [[nodiscard]] domain::TranscriptionEngine engine() const noexcept override
    {
        return domain::TranscriptionEngine::parakeet;
    }
    [[nodiscard]] const std::filesystem::path& model_path() const noexcept override { return model_path_; }
    [[nodiscard]] platform::EngineCapabilities capabilities() const noexcept override;
    [[nodiscard]] bool is_ready() const noexcept override { return runtime_->is_ready(); }
    [[nodiscard]] int abi_version() const noexcept { return runtime_->abi_version(); }

    domain::Status warmup() override;
    domain::Status deep_warmup(const domain::CancellationToken& cancellation) override;
    [[nodiscard]] domain::Result<std::string> transcribe(
        const domain::WavAudio& wav,
        const platform::TranscriptionRequest& request,
        const domain::CancellationToken& cancellation) override;

private:
    std::filesystem::path model_path_;
    std::unique_ptr<platform::ParakeetRuntime> runtime_;
    std::mutex inference_mutex_;
};

/// Factory used by NativeEngineRegistry: loads the ggml model and wraps it.
[[nodiscard]] domain::Result<std::unique_ptr<platform::Transcriber>> make_whisper_engine(
    const std::filesystem::path& model_path, int requested_threads = 0);

/// Factory used by NativeEngineRegistry: opens parakeet.dll (ABI 6 asserted by
/// the runtime) and wraps it. The model is loaded by warmup(), not here.
[[nodiscard]] domain::Result<std::unique_ptr<platform::Transcriber>> make_parakeet_engine(
    const std::filesystem::path& dll_path, const std::filesystem::path& model_path);

/// Availability probe used by NativeEngineRegistry. Loads the DLL, resolves the
/// six frozen symbols, checks ABI 6 and the model file — without loading a model.
[[nodiscard]] platform::EngineAvailability probe_parakeet_for_registry(
    domain::TranscriptionEngine engine, const std::filesystem::path& model_path);

} // namespace voicetyper::asr
