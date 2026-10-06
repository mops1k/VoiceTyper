#pragma once

// Speech-to-text contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §5 and
// docs/migration/cpp/feature-parity.md rows "Whisper", "Parakeet" and
// "ASR model lifecycle"; .NET reference
// VoiceTyper.Core/Services/Transcription/ITranscriptionEngine.cs, WhisperEngine.cs,
// ParakeetEngine.cs and EngineManager.cs.
//
// Frozen observable contract:
//   * Input is a complete 16 kHz mono PCM16 WAV (WavAudio).
//   * Output is the *full final* transcription. There is no streaming preview in
//     the product, and none may be added silently.
//   * At most one engine is loaded at a time; inference is serialized inside the
//     implementation because the underlying contexts are not thread-safe.
//   * warmup() loads the model without running inference; deep_warmup() runs one
//     short inference so the first real dictation is not the slow one. Both must
//     be callable in the background while the UI and hotkeys stay responsive.
//   * No silent engine fallback: see engine_registry.hpp. A Transcriber never
//     substitutes another engine and never returns a transcript from a different
//     model than the one it was created with.
//
// Parameters that a given engine honours are declared in EngineCapabilities
// instead of being silently dropped. The current .NET behavior is: Whisper
// applies language, prompt, temperature, context selection and bestOf, while
// Parakeet ignores language override, prompt, temperature, previous context and
// bestOf. Declaring that difference makes it visible without deciding a new
// policy for it.
//
// Cancellation: every call takes a token and returns ErrorCode::cancelled when
// the token fires. How a given engine stops mid-inference is an engine
// implementation detail (Phase C) and is intentionally not specified here.
//
// Thread affinity: transcribe() is a blocking call and must be invoked from a
// worker thread, never from the UI thread. The instance itself is owned by the
// caller and used from one thread at a time (the engine manager serializes).

#include "domain/audio_format.hpp"
#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/settings.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace voicetyper::platform {

using domain::AudioFormat;
using domain::CancellationToken;
using domain::ErrorCode;
using domain::RecognitionLanguage;
using domain::Result;
using domain::Status;
using domain::TranscriptionEngine;
using domain::WavAudio;

/// One transcription request.
struct TranscriptionRequest {
    /// Whisper applies this; Parakeet is model-auto and reports
    /// `supports_language_override == false`.
    RecognitionLanguage language = RecognitionLanguage::ru;
    /// Technical-terms initial prompt. Ignored by engines that do not support it.
    std::string prompt;
    /// 0.0 is the deterministic default. Ignored by engines that do not support it.
    double temperature = 0.0;
    /// Requests that the previous segment be used as context. The current .NET
    /// build only uses this to select `WithNoContext()` when false and never
    /// stores a previous transcript, so an implementation must not claim
    /// working context without its own contract test.
    bool condition_on_previous_text = false;
    /// Number of greedy candidates. The recording state machine requests 3 for
    /// the final result; 1 is the fast path used elsewhere.
    int best_of = 1;
};

/// Declares which request fields an engine actually applies.
///
/// This exists so that "the engine ignored my parameter" is a reported fact
/// rather than a silent behavior. A caller that requires a field can refuse an
/// engine whose capabilities say the field is unsupported.
struct EngineCapabilities {
    /// The engine honours TranscriptionRequest::language.
    bool supports_language_override = false;
    /// The engine honours TranscriptionRequest::prompt.
    bool supports_prompt = false;
    /// The engine honours TranscriptionRequest::temperature.
    bool supports_temperature = false;
    /// The engine can consume a previous segment as context.
    bool supports_previous_context = false;
    /// The engine honours TranscriptionRequest::best_of (multiple candidates).
    bool supports_best_of = false;
    /// The engine can report per-request progress/partial state. Must stay
    /// false: the product shows no streaming preview.
    bool supports_streaming_partials = false;
};

/// A loaded speech-to-text engine bound to exactly one model file.
class Transcriber {
public:
    virtual ~Transcriber() = default;

    Transcriber(const Transcriber&) = delete;
    Transcriber& operator=(const Transcriber&) = delete;
    Transcriber(Transcriber&&) = delete;
    Transcriber& operator=(Transcriber&&) = delete;

    /// The engine this instance implements.
    [[nodiscard]] virtual TranscriptionEngine engine() const noexcept = 0;

    /// The exact model file this instance was created with. The returned path is
    /// owned by the instance and stays valid until it is destroyed.
    [[nodiscard]] virtual const std::filesystem::path& model_path() const noexcept = 0;

    /// Which request fields this engine applies.
    [[nodiscard]] virtual EngineCapabilities capabilities() const noexcept = 0;

    /// True once the model is loaded and inference can start.
    [[nodiscard]] virtual bool is_ready() const noexcept = 0;

    /// Loads the model without running inference. Idempotent; a second call on a
    /// ready instance does nothing. Failure: model_not_ready, corrupt_data.
    virtual Status warmup() = 0;

    /// Runs one short inference so the compute paths are hot. Must be safe to
    /// call from a background thread while the UI is responsive. Failure codes as
    /// for transcribe(), plus cancelled.
    virtual Status deep_warmup(const CancellationToken& cancellation) = 0;

    /// Transcribes a complete WAV file and returns the full final text.
    ///
    /// Failure codes: invalid_argument (the WAV is not 16 kHz mono PCM16),
    /// model_not_ready (warmup() has not completed), cancelled, io_failure.
    /// An empty transcript is a *successful* result with an empty string; the
    /// caller decides whether that suppresses the paste.
    [[nodiscard]] virtual Result<std::string> transcribe(
        const WavAudio& wav, const TranscriptionRequest& request, const CancellationToken& cancellation) = 0;

protected:
    Transcriber() = default;
};

} // namespace voicetyper::platform
