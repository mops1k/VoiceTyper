#include "asr/engine_parameters.hpp"

#include "domain/audio_wav.hpp"

#include <algorithm>
#include <cmath>

namespace voicetyper::asr {
namespace {

/// Decodes one UTF-8 code point. Returns the code point and advances `index`;
/// invalid bytes are returned as-is so a malformed transcript still behaves
/// predictably instead of hanging the loop.
std::uint32_t next_code_point(std::string_view text, std::size_t& index)
{
    const auto first = static_cast<unsigned char>(text[index]);
    if (first < 0x80) {
        ++index;
        return first;
    }
    std::size_t extra = 0;
    std::uint32_t value = 0;
    if ((first & 0xE0U) == 0xC0U) {
        extra = 1;
        value = first & 0x1FU;
    } else if ((first & 0xF0U) == 0xE0U) {
        extra = 2;
        value = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
        extra = 3;
        value = first & 0x07U;
    } else {
        ++index;
        return first;
    }
    if (index + extra >= text.size()) {
        ++index;
        return first;
    }
    for (std::size_t i = 1; i <= extra; ++i) {
        const auto byte = static_cast<unsigned char>(text[index + i]);
        if ((byte & 0xC0U) != 0x80U) {
            ++index;
            return first;
        }
        value = (value << 6U) | (byte & 0x3FU);
    }
    index += extra + 1;
    return value;
}

/// char.IsWhiteSpace parity for the code points that can appear in a transcript.
bool is_unicode_space(std::uint32_t code_point) noexcept
{
    switch (code_point) {
    case 0x09: // tab
    case 0x0A: // line feed
    case 0x0B: // vertical tab
    case 0x0C: // form feed
    case 0x0D: // carriage return
    case 0x20: // space
    case 0x85: // next line
    case 0xA0: // no-break space
    case 0x1680: // ogham space mark
    case 0x2028: // line separator
    case 0x2029: // paragraph separator
    case 0x202F: // narrow no-break space
    case 0x205F: // medium mathematical space
    case 0x3000: // ideographic space
        return true;
    default:
        return code_point >= 0x2000 && code_point <= 0x200A;
    }
}

} // namespace

std::string_view whisper_language_code(RecognitionLanguage language) noexcept
{
    switch (language) {
    case RecognitionLanguage::automatic: return "auto";
    case RecognitionLanguage::ru: return "ru";
    case RecognitionLanguage::en: return "en";
    }
    return "auto";
}

bool whitespace_only(std::string_view text) noexcept
{
    std::size_t index = 0;
    while (index < text.size()) {
        if (!is_unicode_space(next_code_point(text, index))) {
            return false;
        }
    }
    return true;
}

Result<NormalizedRequest> normalize(const TranscriptionRequest& request)
{
    if (request.temperature < kMinTemperature) {
        return Result<NormalizedRequest>::failure(
            ErrorCode::out_of_range, "temperature is below the engine minimum");
    }
    if (request.best_of < kMinBestOf) {
        return Result<NormalizedRequest>::failure(
            ErrorCode::out_of_range, "best_of is below the engine minimum");
    }
    if (request.best_of > kMaxBestOf) {
        return Result<NormalizedRequest>::failure(
            ErrorCode::out_of_range, "best_of is above the engine maximum");
    }

    NormalizedRequest normalized;
    normalized.language_code = whisper_language_code(request.language);
    // A whitespace-only prompt is the same as no prompt, matching
    // string.IsNullOrWhiteSpace in the .NET reference.
    if (!whitespace_only(request.prompt)) {
        normalized.initial_prompt = request.prompt;
        normalized.carry_initial_prompt = true;
    }
    // One documented narrowing: settings store a double, whisper.cpp wants a
    // float, and values above the engine maximum are clamped rather than
    // rejected so a hand-edited settings.json cannot break dictation.
    normalized.temperature = static_cast<float>(std::min(request.temperature, kMaxTemperature));
    normalized.no_context = !request.condition_on_previous_text;
    normalized.best_of = request.best_of;
    return normalized;
}

ParakeetRequest normalize_parakeet(const TranscriptionRequest& request) noexcept
{
    // The shipped C# build ignores language, prompt, temperature and best_of for
    // Parakeet: the capabilities below state that, and this function keeps the
    // decoder and sample rate at the only values the pinned model supports.
    static_cast<void>(request);
    return ParakeetRequest{};
}

platform::EngineCapabilities whisper_capabilities() noexcept
{
    platform::EngineCapabilities capabilities;
    capabilities.supports_language_override = true;
    capabilities.supports_prompt = true;
    capabilities.supports_temperature = true;
    capabilities.supports_previous_context = false;
    capabilities.supports_best_of = true;
    capabilities.supports_streaming_partials = false;
    return capabilities;
}

platform::EngineCapabilities parakeet_capabilities() noexcept
{
    return platform::EngineCapabilities{};
}

Result<WavAudio> build_warmup_wav()
{
    const std::vector<float> silence(kWarmupSamples, 0.0f);
    std::string bytes;
    const auto written = domain::write_wav_pcm16(silence, bytes);
    if (written.is_error()) {
        return Result<WavAudio>::failure(written.code(), written.message());
    }
    std::vector<std::byte> file(bytes.size());
    std::transform(bytes.begin(), bytes.end(), file.begin(), [](char value) {
        return static_cast<std::byte>(static_cast<unsigned char>(value));
    });
    return WavAudio(std::move(file));
}

} // namespace voicetyper::asr
