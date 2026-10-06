#pragma once

// Engine-independent ASR request mapping. Pure functions over the frozen
// platform request/capability types: no whisper.cpp, no parakeet, no Qt, no OS.
// This is where a settings value becomes an engine call, and therefore where the
// parity rules live:
//   * automatic language is the whisper.cpp sentinel "auto", never "";
//   * the temperature/entropy/logprob fallbacks stay disabled
//   * an engine that ignores a field says so through EngineCapabilities instead
//     of silently pretending to honour it.

#include "asr/whisper_native.hpp"
#include "domain/audio_format.hpp"
#include "domain/error.hpp"
#include "domain/settings.hpp"
#include "platform/api/transcriber.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::asr {

using domain::ErrorCode;
using domain::RecognitionLanguage;
using domain::Result;
using domain::WavAudio;
using platform::TranscriptionRequest;

/// The four frozen decode constants (no_speech_threshold, temperature_inc,
/// entropy_thold, logprob_thold) live in whisper_native.hpp, which is the
/// authority for the whisper.cpp call shape. Declaring them twice would let the
/// two copies drift, so this header only pins the bounds that are not part of the
/// C API call itself.
/// whisper.cpp accepts 0..1; settings.json stores a double without that bound.
inline constexpr double kMinTemperature = 0.0;
inline constexpr double kMaxTemperature = 1.0;
inline constexpr int kMinBestOf = 1;
inline constexpr int kMaxBestOf = 8;
/// 0.3 s of silence at 16 kHz, the payload both C# engines warm up with.
inline constexpr std::size_t kWarmupSamples = 4800;
/// 44-byte canonical header + 4800 * 2 bytes of PCM16 silence.
inline constexpr std::size_t kWarmupWavBytes = 9644;

/// "auto" is whisper.cpp's own sentinel for language detection. It is a
/// non-empty string on purpose: an empty language means "model default" there.
[[nodiscard]] std::string_view whisper_language_code(RecognitionLanguage language) noexcept;

/// Unicode-aware equivalent of C# string.IsNullOrWhiteSpace. ASCII-only checks
/// miss NBSP and the ideographic space, which do occur in recognized text.
[[nodiscard]] bool whitespace_only(std::string_view text) noexcept;

struct NormalizedRequest {
    std::string language_code;
    std::string initial_prompt;
    float temperature = 0.0f;
    bool no_context = true;
    bool carry_initial_prompt = false;
    int best_of = 1;
};

/// Validates and narrows a request. The only double->float narrowing in the
/// codebase happens here, once.
[[nodiscard]] Result<NormalizedRequest> normalize(const TranscriptionRequest& request);

struct ParakeetRequest {
    /// 0 = default head (TDT for the pinned v3 model).
    int decoder = 0;
    std::uint32_t sample_rate = 16000;
    /// Empty means "the model's own language"; the shipped C# build never calls
    /// the _lang entry point, so an empty value keeps that parity.
    std::string target_language;
};

[[nodiscard]] ParakeetRequest normalize_parakeet(const TranscriptionRequest& request) noexcept;

[[nodiscard]] platform::EngineCapabilities whisper_capabilities() noexcept;
[[nodiscard]] platform::EngineCapabilities parakeet_capabilities() noexcept;

/// The shared warm-up payload: 0.3 s of 16 kHz mono PCM16 silence as a complete
/// WAV file. One generator for both engines, so the warm-up input cannot drift.
[[nodiscard]] Result<WavAudio> build_warmup_wav();

} // namespace voicetyper::asr
