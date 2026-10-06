#pragma once

// Thin C++ wrapper over the pinned whisper.cpp C API.
//
// The upstream header (`whisper.h`, from the commit pinned in
// docs/migration/cpp/native-dependencies.json) is included by
// whisper_native.cpp only. This header stays standard-C++20 with no third-party
// include, so portable code can name the engine, its parameters and its
// failure modes without compiling against whisper.cpp. The real context handle
// is hidden behind a pimpl for the same reason.
//
// Contract, and where it comes from:
//   * src/platform/api/transcriber.hpp - full final transcription, no streaming
//     preview, at most one engine loaded at a time, warmup without inference,
//     cooperative cancellation, and never a transcript from another model than
//     the one the engine was created with.
//   * VoiceTyper.Core/Services/Transcription/WhisperEngine.cs - the frozen .NET
//     parameters: physical cores clamped to 1..16, greedy search, no-speech
//     threshold 0.6, temperature increment 0 and entropy/logprob thresholds -1
//     (i.e. no temperature or entropy fallback), bestOf from the caller, and
//     WithNoContext() when the caller does not ask for previous context.
//   * docs/migration/cpp/compatibility-contracts.md section 5.
//
// This is a dependency boundary, not an engine policy. It has no notion of
// "the other engine": if whisper.cpp or the model file is unusable, the call
// fails with a reported ErrorCode. Substituting another engine, or retrying
// with a different model, is the engine registry's job and is explicitly not
// done here (src/platform/api/engine_registry.hpp).

#include "domain/cancellation.hpp"
#include "domain/error.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace voicetyper::asr {

using domain::CancellationToken;
using domain::Error;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// C ABI revision of the pinned whisper.cpp build, injected by
/// cmake/NativeAsr.cmake from docs/migration/cpp/native-dependencies.json.
/// The contract test re-checks it against the manifest, so a pin change cannot
/// pass unnoticed.
[[nodiscard]] std::string_view whisper_pinned_commit() noexcept;
[[nodiscard]] std::string_view whisper_upstream_version() noexcept;
/// Static, offline, local CPU inference. whisper.cpp sends nothing anywhere;
/// this exists so the no-telemetry claim is a checked constant, not prose.
[[nodiscard]] std::string_view whisper_license() noexcept;
[[nodiscard]] std::string_view whisper_repository() noexcept;
[[nodiscard]] std::string_view whisper_archive_sha256() noexcept;

/// Frozen decode parameters of the .NET engine. Exposed so a contract test can
/// assert parity without loading a model; whisper_native.cpp applies exactly
/// these values.
inline constexpr float kWhisperNoSpeechThreshold = 0.6f;
inline constexpr float kWhisperTemperatureIncrement = 0.0f;
inline constexpr float kWhisperEntropyThreshold = -1.0f;
inline constexpr float kWhisperLogProbThreshold = -1.0f;
/// VoiceTyper.Core/Interop/... and SileroSpeechSegmenter clamp the physical core
/// count to 1..16 for inference; the same bound is applied here.
inline constexpr int kWhisperMinThreads = 1;
inline constexpr int kWhisperMaxThreads = 16;

/// Applies the .NET clamp to a requested thread count. Public so the contract
/// test can check the bound without a model.
[[nodiscard]] int clamp_whisper_threads(int requested) noexcept;

/// Context-level options. CPU only: the product is an offline recognizer
/// (parity row PRIV-01), so no GPU or backend selection is offered here.
struct WhisperContextOptions {
    /// Already-clamped worker threads. Values outside 1..16 are clamped by
    /// clamp_whisper_threads(); 0 selects the physical-core-derived default.
    int threads = 0;
};

/// Decode options. Every field maps to one whisper_full_params field, so an
/// unsupported parameter is a compile error rather than a silently dropped
/// setting.
struct WhisperDecodeOptions {
    /// nullptr, "" or "auto" selects whisper.cpp language detection, matching
    /// .NET RecognitionLanguage.Auto. "ru"/"en" are passed through verbatim.
    const char* language = nullptr;
    /// Initial prompt (the settings terms dictionary). Empty means no prompt.
    std::string prompt;
    /// .NET calls WithPrompt(prompt).WithCarryInitialPrompt(true) when the
    /// prompt is non-blank; the same rule is applied here.
    bool carry_initial_prompt = true;
    /// Initial temperature. 0.0 is the deterministic default.
    float temperature = 0.0f;
    /// Greedy candidates. The recording state machine requests 3 for the final
    /// result; 1 is the fast path. Values below 1 are clamped to 1.
    int best_of = 1;
    /// Mirrors .NET: WithNoContext() is used when the caller does not want
    /// previous context. No previous transcript is ever stored here, because
    /// the .NET engine does not store one either.
    bool no_context = true;
};

/// One loaded whisper.cpp model.
///
/// Lifetime/ownership: the context is created by load(), destroyed here, and
/// the model file is read once. Inference is not thread-safe inside whisper.cpp,
/// so one instance must be used from one thread at a time (the engine
/// manager serializes). Failure closes the object; a failed load() returns no
/// object at all.
class WhisperNativeContext {
public:
    ~WhisperNativeContext();

    WhisperNativeContext(const WhisperNativeContext&) = delete;
    WhisperNativeContext& operator=(const WhisperNativeContext&) = delete;
    WhisperNativeContext(WhisperNativeContext&&) = delete;
    WhisperNativeContext& operator=(WhisperNativeContext&&) = delete;

    /// Loads a ggml model without running inference (the .NET Warmup()).
    ///
    /// Failure codes:
    ///   not_found      - the model file does not exist,
    ///   corrupt_data   - the file exists but is not a usable ggml model,
    ///   resource_exhausted - whisper.cpp could not allocate the context,
    ///   cancelled      - the token was already requested.
    [[nodiscard]] static Result<std::unique_ptr<WhisperNativeContext>> load(
        const std::filesystem::path& model_path,
        const WhisperContextOptions& options,
        const CancellationToken& cancellation);

    /// True when a model is loaded and inference may start.
    [[nodiscard]] bool is_ready() const noexcept;

    /// Runs the whole buffer (PCM float32, the 16 kHz mono samples the rest of
    /// the pipeline produces) and returns the full final text, trimmed.
    ///
    /// Cancellation is cooperative: the token is polled from whisper.cpp's
    /// abort callback during the run, so a cancel stops the work instead of
    /// waiting for the whole buffer. The abort is observed as
    /// ErrorCode::cancelled.
    ///
    /// Failure codes: model_not_ready (no model loaded), invalid_argument
    /// (zero samples or a null buffer), cancelled, internal.
    [[nodiscard]] Result<std::string> transcribe(
        const float* samples,
        std::size_t sample_count,
        const WhisperDecodeOptions& options,
        const CancellationToken& cancellation);

    /// Releases the native context. Idempotent, and also run by the destructor.
    void free_context() noexcept;

private:
    WhisperNativeContext();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voicetyper::asr
