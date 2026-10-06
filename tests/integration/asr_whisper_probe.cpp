// Whisper parameter probe: which decoder configuration actually transcribes
// real speech?
//
// Why it exists: the product's frozen decoder parameters (mirroring
// WhisperEngine.cs: greedy, best_of, temperature 0, temperature_inc 0,
// entropy -1, logprob -1, no_speech 0.6, language "ru", no_context) transcribed
// a clear 18.77 s Russian TTS clip as `*РУА*` on the target machine, while the
// installed .NET build recognises dictation with the same model file. Guessing
// which of the eleven parameters is responsible is not engineering, so this
// tool runs the SAME audio through several named parameter sets and prints the
// transcript of each one.
//
// Opt-in by data, like the smoke test: it skips with a printed reason unless
// VOICETYPER_WHISPER_MODEL and VOICETYPER_ASR_FIXTURE point at real files. It is
// registered as a build target, not as a CTest case, because its output is
// diagnostic text rather than a pass/fail contract.

#include "domain/audio_wav.hpp"

#include <whisper.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string environment(const char* name)
{
    const auto* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

struct Profile {
    const char* name;
    whisper_sampling_strategy strategy;
    float temperature = 0.0f;
    float temperature_inc = 0.0f;
    float entropy_thold = -1.0f;
    float logprob_thold = -1.0f;
    float no_speech_thold = 0.6f;
    int best_of = 1;
    const char* language = "ru";
    bool no_context = true;
};

std::string run_profile(whisper_context* context, const std::vector<float>& samples, const Profile& profile,
    long long& elapsed_ms)
{
    whisper_full_params params = whisper_full_default_params(profile.strategy);
    params.n_threads = 4;
    params.strategy = profile.strategy;
    params.temperature = profile.temperature;
    params.temperature_inc = profile.temperature_inc;
    params.entropy_thold = profile.entropy_thold;
    params.logprob_thold = profile.logprob_thold;
    params.no_speech_thold = profile.no_speech_thold;
    params.greedy.best_of = profile.best_of;
    params.beam_search.beam_size = profile.strategy == WHISPER_SAMPLING_BEAM_SEARCH ? 5 : 1;
    params.no_context = profile.no_context;
    params.language = profile.language;
    params.detect_language = false;
    params.initial_prompt = nullptr;
    params.carry_initial_prompt = false;
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.print_special = false;

    const auto started = std::chrono::steady_clock::now();
    const int result = whisper_full(context, params, samples.data(), static_cast<int>(samples.size()));
    elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (result != 0) {
        return "<whisper_full failed with code " + std::to_string(result) + ">";
    }

    std::string text;
    const int segments = whisper_full_n_segments(context);
    for (int segment = 0; segment < segments; ++segment) {
        const char* piece = whisper_full_get_segment_text(context, segment);
        if (piece != nullptr) {
            text += piece;
        }
    }
    return text;
}

} // namespace

int main()
{
    const auto model = environment("VOICETYPER_WHISPER_MODEL");
    const auto fixture = environment("VOICETYPER_ASR_FIXTURE");
    if (model.empty() || !std::filesystem::is_regular_file(model)) {
        std::cout << "asr-whisper-probe: SKIP (set VOICETYPER_WHISPER_MODEL to a ggml model file)\n";
        return 0;
    }
    if (fixture.empty() || !std::filesystem::is_regular_file(fixture)) {
        std::cout << "asr-whisper-probe: SKIP (set VOICETYPER_ASR_FIXTURE to a 16 kHz mono PCM16 WAV)\n";
        return 0;
    }

    const auto samples = voicetyper::domain::read_wav_file(fixture);
    if (samples.is_error()) {
        std::cerr << "asr-whisper-probe: FAIL fixture is not a 16 kHz mono PCM16 WAV: "
                  << samples.error().message() << '\n';
        return 1;
    }
    std::cout << "fixture: " << fixture << " (" << samples.value().size() / 16000.0 << " s)\n";
    std::cout << "model:   " << model << '\n';

    // The samples whisper actually receives, so "the file is fine" and "the
    // buffer is fine" are two separate facts. A wrong scale factor, a wrong
    // chunk size or an int16/float mix-up all show up here in one line.
    {
        const auto& pcm = samples.value();
        double sum_squares = 0.0;
        float peak = 0.0f;
        for (const float sample : pcm) {
            sum_squares += static_cast<double>(sample) * static_cast<double>(sample);
            const float magnitude = sample < 0.0f ? -sample : sample;
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        const double rms = pcm.empty() ? 0.0 : std::sqrt(sum_squares / static_cast<double>(pcm.size()));
        std::cout << "samples: " << pcm.size() << " rms=" << rms << " peak=" << peak << " first=";
        for (std::size_t index = 0; index < 5 && index < pcm.size(); ++index) {
            std::cout << pcm[index] << ' ';
        }
        std::cout << '\n';
    }

    whisper_context_params context_params = whisper_context_default_params();
    context_params.use_gpu = false;
    context_params.flash_attn = false;
    whisper_context* context = whisper_init_from_file_with_params(model.c_str(), context_params);
    if (context == nullptr) {
        std::cerr << "asr-whisper-probe: FAIL the model could not be loaded\n";
        return 1;
    }

    const std::vector<Profile> profiles {
        // Exactly what the product sends today.
        {"product (greedy best3, temp0, inc0, entropy-1, logprob-1, no_speech .6, ru, no_context)",
            WHISPER_SAMPLING_GREEDY, 0.0f, 0.0f, -1.0f, -1.0f, 0.6f, 3, "ru", true},
        // Whisper's own defaults for greedy, language pinned.
        {"whisper defaults (greedy best5, temp0, inc0.2, entropy2.4, logprob-1.0, ru)",
            WHISPER_SAMPLING_GREEDY, 0.0f, 0.2f, 2.4f, -1.0f, 0.6f, 5, "ru", true},
        // Language detection instead of a pinned language.
        {"language auto (greedy best3, temp0, inc0, entropy-1, logprob-1)",
            WHISPER_SAMPLING_GREEDY, 0.0f, 0.0f, -1.0f, -1.0f, 0.6f, 3, nullptr, true},
        // Temperature fallback enabled, everything else as the product sends it.
        {"product + temperature fallback (inc 0.2, best5)",
            WHISPER_SAMPLING_GREEDY, 0.0f, 0.2f, -1.0f, -1.0f, 0.6f, 5, "ru", true},
        // Beam search, which ignores greedy.best_of.
        {"beam search 5", WHISPER_SAMPLING_BEAM_SEARCH, 0.0f, 0.0f, -1.0f, -1.0f, 0.6f, 1, "ru", true},
        // Context carried across calls (the .NET WithNoContext() off).
        {"product with context (no_context false)", WHISPER_SAMPLING_GREEDY, 0.0f, 0.0f, -1.0f, -1.0f, 0.6f,
            3, "ru", false},
    };

    int failures = 0;
    for (const auto& profile : profiles) {
        long long elapsed_ms = 0;
        const std::string text = run_profile(context, samples.value(), profile, elapsed_ms);
        std::cout << "\n[" << (elapsed_ms / 1000.0) << " s] " << profile.name << "\n  -> " << text << '\n';
        if (text.find("failed with code") != std::string::npos) {
            ++failures;
        }
    }

    whisper_free(context);
    std::cout << "\nasr-whisper-probe: " << (failures == 0 ? "OK" : "FAIL") << " ("
              << profiles.size() << " profiles)\n";
    return failures == 0 ? 0 : 1;
}
