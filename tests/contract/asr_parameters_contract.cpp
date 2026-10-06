#include "asr/engine_parameters.hpp"

#include "asr/whisper_native.hpp"
#include "domain/audio_wav.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

using namespace voicetyper;
using namespace voicetyper::asr;

void check_language_codes()
{
    check(whisper_language_code(domain::RecognitionLanguage::ru) == "ru", "ru maps to ru");
    check(whisper_language_code(domain::RecognitionLanguage::en) == "en", "en maps to en");
    check(whisper_language_code(domain::RecognitionLanguage::automatic) == "auto",
        "automatic maps to the whisper sentinel \"auto\"");
    check(!whisper_language_code(domain::RecognitionLanguage::automatic).empty(),
        "the automatic language is not an empty string");
}

void check_whitespace()
{
    check(whitespace_only("") && whitespace_only("  \t\r\n\v\f"), "ASCII whitespace counts as blank");
    check(whitespace_only("\xC2\xA0"), "NBSP counts as blank");
    check(whitespace_only("\xE3\x80\x80"), "the ideographic space counts as blank");
    check(whitespace_only("\xE2\x80\xA8"), "U+2028 counts as blank");
    check(whitespace_only("\xE2\x80\xAF"), "the narrow no-break space counts as blank");
    check(!whitespace_only(" \xC2\xA0x"), "a non-space code point makes the text non-blank");
    check(!whitespace_only("привет"), "cyrillic text is not blank");
}

void check_normalize()
{
    platform::TranscriptionRequest request;
    request.language = domain::RecognitionLanguage::automatic;
    request.prompt = "   \t ";
    request.temperature = 0.0;
    request.condition_on_previous_text = false;
    request.best_of = 3;

    const auto normalized = normalize(request);
    check(normalized.is_ok(), "a default request normalizes");
    check(normalized.value().language_code == "auto", "language survives normalization");
    check(normalized.value().initial_prompt.empty() && !normalized.value().carry_initial_prompt,
        "a whitespace-only prompt is dropped");
    check(normalized.value().no_context, "no_context is the default");
    check(normalized.value().best_of == 3, "best_of passes through");

    request.prompt = "термины";
    request.condition_on_previous_text = true;
    const auto with_prompt = normalize(request);
    check(with_prompt.value().initial_prompt == "термины" && with_prompt.value().carry_initial_prompt,
        "a real prompt is carried");
    check(!with_prompt.value().no_context, "condition_on_previous_text clears no_context");

    request.best_of = 0;
    check(normalize(request).is_error(), "best_of below 1 is rejected");
    request.best_of = 9;
    check(normalize(request).is_error(), "best_of above 8 is rejected");
    request.best_of = 1;
    check(normalize(request).value().best_of == 1, "best_of 1 is accepted");

    request.temperature = -0.1;
    check(normalize(request).is_error(), "a negative temperature is rejected");
    request.temperature = 0.0;
    check(normalize(request).value().temperature == 0.0f, "temperature 0 narrows to 0");
    request.temperature = 0.7;
    check(std::abs(normalize(request).value().temperature - 0.7f) < 1e-6f, "temperature 0.7 narrows to float");
    request.temperature = 1.0;
    check(std::abs(normalize(request).value().temperature - 1.0f) < 1e-6f, "temperature 1.0 narrows to float");
    request.temperature = 2.0;
    check(std::abs(normalize(request).value().temperature - 1.0f) < 1e-6f,
        "an out-of-range temperature is clamped instead of breaking dictation");
}

void check_frozen_constants()
{
    check(kWhisperNoSpeechThreshold == 0.6f, "no_speech_threshold stays 0.6");
    check(kWhisperTemperatureIncrement == 0.0f, "the temperature fallback stays disabled");
    check(kWhisperEntropyThreshold == -1.0f, "the entropy fallback stays disabled");
    check(kWhisperLogProbThreshold == -1.0f, "the logprob fallback stays disabled");
}

void check_capabilities()
{
    const auto whisper = whisper_capabilities();
    check(whisper.supports_language_override && whisper.supports_prompt && whisper.supports_temperature,
        "whisper applies language, prompt and temperature");
    check(!whisper.supports_previous_context, "whisper does not claim previous-segment context");
    check(whisper.supports_best_of, "whisper applies best_of");
    check(!whisper.supports_streaming_partials, "streaming partials stay off");

    const auto parakeet = parakeet_capabilities();
    check(!parakeet.supports_language_override && !parakeet.supports_prompt && !parakeet.supports_temperature
            && !parakeet.supports_previous_context && !parakeet.supports_best_of
            && !parakeet.supports_streaming_partials,
        "parakeet declares every field unsupported, matching the C# engine");

    platform::TranscriptionRequest request;
    request.language = domain::RecognitionLanguage::ru;
    request.temperature = 0.9;
    request.prompt = "x";
    request.best_of = 5;
    const auto parakeet_request = normalize_parakeet(request);
    check(parakeet_request.decoder == 0, "parakeet uses the default decoder");
    check(parakeet_request.sample_rate == 16000, "parakeet runs at 16 kHz");
    check(parakeet_request.target_language.empty(), "parakeet keeps the model default language");
}

void check_warmup_wav()
{
    const auto wav = build_warmup_wav();
    check(wav.is_ok(), "the warm-up WAV is built");
    check(wav.value().bytes == kWarmupWavBytes, "the warm-up WAV is exactly 9644 bytes");
    check(wav.value().has_samples(), "the warm-up WAV carries its samples, not only metadata");
    const auto reread = domain::read_wav_pcm16(wav.value().file_view());
    check(reread.is_ok() && reread.value().size() == kWarmupSamples,
        "the warm-up WAV decodes back to 4800 silent samples");
    for (const auto sample : reread.value()) {
        check(sample == 0.0f, "the warm-up payload is silent");
        break;
    }
}

} // namespace

int main()
{
    check_language_codes();
    check_whitespace();
    check_normalize();
    check_frozen_constants();
    check_capabilities();
    check_warmup_wav();
    if (failures != 0) {
        std::cerr << "asr-parameters-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "asr-parameters-contract: OK\n";
    return 0;
}
