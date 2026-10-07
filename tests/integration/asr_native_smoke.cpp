// Real-model ASR smoke test. It is opt-in by data, not by build: when the model
// file is absent it prints a skip reason and passes, so CI without models stays
// green while a developer with a model gets real evidence.
//
// What it proves: the whole chain works on this machine — EngineRegistry ->
// model load -> deep warm-up -> one full transcription — and it reports the
// timings the performance-parity task needs. What it does NOT prove: recognition
// quality. Every checked-in WAV fixture is sine or silence, so a non-empty
// transcript is not a quality claim; a real speech clip is a physical-smoke input.

#include "asr/engine_parameters.hpp"
#include "asr/gigaam_transcriber.hpp"
#include "asr/native_engine_registry.hpp"
#include "asr/native_transcribers.hpp"
#include "domain/audio_wav.hpp"
#include "domain/terms_dictionary.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

std::string environment(const char* name)
{
    const auto* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

void report(const std::string& key, const std::string& value)
{
    std::cout << "  \"" << key << "\": \"" << value << "\",\n";
}

} // namespace

int main()
{
    // Engine selection is opt-in by data, like the model: the default stays
    // Whisper, and `VOICETYPER_ENGINE=gigaam` runs the same chain through the
    // transcribe.cpp runtime instead.
    const std::string engine_name = environment("VOICETYPER_ENGINE").empty()
        ? std::string("whisper")
        : environment("VOICETYPER_ENGINE");
    const auto model = environment("VOICETYPER_GIGAAM_MODEL").empty()
        ? environment("VOICETYPER_WHISPER_MODEL")
        : environment("VOICETYPER_GIGAAM_MODEL");
    const auto fixture = environment("VOICETYPER_ASR_FIXTURE");
    if (model.empty() || !std::filesystem::is_regular_file(model)) {
        std::cout << "asr-native-smoke: SKIP (set VOICETYPER_WHISPER_MODEL or VOICETYPER_GIGAAM_MODEL to a model file)\n";
        return 0;
    }
    if (fixture.empty() || !std::filesystem::is_regular_file(fixture)) {
        std::cout << "asr-native-smoke: SKIP (set VOICETYPER_ASR_FIXTURE to a 16 kHz mono PCM16 WAV)\n";
        return 0;
    }

    const auto raw = voicetyper::domain::read_wav_file(fixture);
    if (raw.is_error()) {
        std::cerr << "asr-native-smoke: FAIL fixture is not a 16 kHz mono PCM16 WAV: "
                  << raw.error().message() << '\n';
        return 1;
    }

    const voicetyper::domain::TranscriptionEngine engine_kind
        = engine_name == "gigaam" ? voicetyper::domain::TranscriptionEngine::gigaam
        : engine_name == "parakeet" ? voicetyper::domain::TranscriptionEngine::parakeet
                                    : voicetyper::domain::TranscriptionEngine::whisper;

    voicetyper::asr::NativeEngineRegistryOptions options;
    options.whisper_available = true;
    options.whisper_factory = [](const std::filesystem::path& path) {
        return voicetyper::asr::make_whisper_engine(path);
    };
    options.parakeet_factory = [](const std::filesystem::path&, const std::filesystem::path&) {
        return voicetyper::asr::make_parakeet_engine({}, {});
    };
    options.parakeet_probe = [](voicetyper::domain::TranscriptionEngine engine, const std::filesystem::path&) {
        voicetyper::platform::EngineAvailability state;
        state.engine = engine;
        state.available = false;
        state.reason = voicetyper::platform::EngineAvailabilityReason::platform_unsupported;
        return state;
    };
    // The real GigaAM wiring, so this smoke test exercises the shipped path:
    // registry -> TranscribeEngine -> library probe -> warmup -> transcribe.
    // A segmenter is bound so a dictation longer than the model window is cut at
    // a pause (the energy heuristic here; Silero replaces it behind the same seam).
    voicetyper::domain::EnergySpeechSegmenter segmenter;
    options.gigaam_library = voicetyper::asr::transcribe_library_beside_executable();
    options.gigaam_factory = [&segmenter](const std::filesystem::path& dll, const std::filesystem::path& path) {
        return voicetyper::asr::make_gigaam_engine(dll, path, &segmenter);
    };
    options.gigaam_probe = [](voicetyper::domain::TranscriptionEngine engine, const std::filesystem::path& path) {
        return voicetyper::asr::probe_gigaam_for_registry(
            engine, path, voicetyper::asr::transcribe_library_beside_executable());
    };

    voicetyper::asr::NativeEngineRegistry registry(std::move(options));
    const auto registered = registry.set_model_path(engine_kind, model);
    if (registered.is_error()) {
        std::cerr << "asr-native-smoke: FAIL could not register the model path\n";
        return 1;
    }
    const auto availability = registry.availability(engine_kind);
    if (!availability.available) {
        std::cerr << "asr-native-smoke: FAIL " << engine_name << " reported unavailable ("
                  << voicetyper::platform::engine_availability_reason_name(availability.reason)
                  << ") for a present model\n";
        if (engine_kind == voicetyper::domain::TranscriptionEngine::gigaam) {
            // The registry reports the coarse reason; the probe carries the exact
            // cause (path, Win32 error, version, struct sizes, missing symbols).
            const auto probe = voicetyper::asr::probe_transcribe_runtime(
                voicetyper::asr::transcribe_library_beside_executable());
            std::cerr << "asr-native-smoke: probe path=" << probe.library_path.string()
                      << " loaded=" << (probe.library_loaded ? "yes" : "no")
                      << " version='" << probe.version << "' detail=" << probe.detail << '\n';
        }
        return 1;
    }

    const auto load_started = std::chrono::steady_clock::now();
    auto engine = registry.create(engine_kind, {});
    if (engine.is_error()) {
        std::cerr << "asr-native-smoke: FAIL create: " << engine.error().message() << '\n';
        return 1;
    }
    // Whisper loads its weights inside create(); GigaAM/Parakeet load them in
    // warmup(), which is the contract's "explicit warmup, never a lazy load
    // inside transcribe" rule. The measured load time covers whichever runs.
    if (!engine.value()->is_ready()) {
        const auto loaded = engine.value()->warmup();
        if (loaded.is_error()) {
            std::cerr << "asr-native-smoke: FAIL warmup: " << loaded.error().message() << '\n';
            return 1;
        }
    }
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - load_started).count();

    if (!engine.value()->is_ready()) {
        std::cerr << "asr-native-smoke: FAIL the engine is not ready after create + warmup\n";
        return 1;
    }

    const voicetyper::domain::CancellationToken none;
    const auto warm_started = std::chrono::steady_clock::now();
    const auto warm = engine.value()->deep_warmup(none);
    const auto warm_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - warm_started).count();
    if (warm.is_error()) {
        std::cerr << "asr-native-smoke: FAIL deep warm-up: " << warm.error().message() << '\n';
        return 1;
    }

    std::string wav_bytes;
    const auto written = voicetyper::domain::write_wav_pcm16(raw.value(), wav_bytes);
    if (written.is_error()) {
        std::cerr << "asr-native-smoke: FAIL could not encode the fixture\n";
        return 1;
    }
    std::vector<std::byte> wav_data(wav_bytes.size());
    for (std::size_t i = 0; i < wav_bytes.size(); ++i) {
        wav_data[i] = static_cast<std::byte>(static_cast<unsigned char>(wav_bytes[i]));
    }

    voicetyper::platform::TranscriptionRequest request;
    request.language = voicetyper::domain::RecognitionLanguage::ru;
    // The engine prompt, so the effect of the terms dictionary on an engine that
    // has one (Whisper) can be measured against the same clip: empty, the legacy
    // comma list, or the sentence domain::terms_initial_prompt builds.
    request.prompt = voicetyper::domain::terms_initial_prompt(
        voicetyper::domain::parse_terms_dictionary(environment("VOICETYPER_ASR_DICTIONARY")));
    if (const std::string raw = environment("VOICETYPER_ASR_RAW_PROMPT"); !raw.empty()) {
        request.prompt = raw;
    }
    request.best_of = 3;

    const auto transcribe_started = std::chrono::steady_clock::now();
    const auto transcript = engine.value()->transcribe(
        voicetyper::domain::WavAudio(std::move(wav_data)), request, none);
    const auto transcribe_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - transcribe_started).count();
    if (transcript.is_error()) {
        std::cerr << "asr-native-smoke: FAIL transcribe: " << transcript.error().message() << '\n';
        return 1;
    }

    std::cout << "{\n";
    report("model", model);
    report("fixture", fixture);
    report("fixture_seconds", std::to_string(static_cast<double>(raw.value().size()) / 16000.0));
    report("load_ms", std::to_string(load_ms));
    report("deep_warmup_ms", std::to_string(warm_ms));
    report("transcribe_ms", std::to_string(transcribe_ms));
    // The dictionary is applied here exactly as TermsDictionaryPort applies it in
    // the product (the same pure function), so a clip plus a pair proves the
    // mechanism on a real engine - including the two engines that have no native
    // dictionary at all.
    // A file is the reliable way to pass Cyrillic on Windows: an environment
    // variable travels through the process code page, a file does not.
    std::string dictionary_text = environment("VOICETYPER_ASR_DICTIONARY");
    const std::string dictionary_file = environment("VOICETYPER_ASR_DICTIONARY_FILE");
    if (dictionary_text.empty() && !dictionary_file.empty()) {
        std::ifstream stream(dictionary_file, std::ios::binary);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        dictionary_text = buffer.str();
    }
    const auto dictionary = voicetyper::domain::parse_terms_dictionary(dictionary_text);
    const std::string final_text = voicetyper::domain::apply_terms(transcript.value(), dictionary);
    report("dictionary", dictionary_text);
    report("transcript", transcript.value());
    report("final_text", final_text);
    std::cout << "  \"note\": \"pipeline evidence only; every checked-in fixture is sine or silence, so this is not a quality claim\"\n";
    std::cout << "}\n";
    std::cout << "asr-native-smoke: OK\n";
    return 0;
}
