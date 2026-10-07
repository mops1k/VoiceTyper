// Contract for the background noise suppressor.
//
// The filter is a line-by-line port of the .NET module VoiceTyper.Core/Audio/NoiseSuppressor.cs,
// which ran inside WavBuilder when the recording buffer was assembled: a high-pass stage that
// removes the DC offset and the rumble, and an adaptive suppressor that damps the frames below
// the estimated noise floor. It lives next to the other audio helpers (domain/audio_wav.hpp),
// and this contract pins its behaviour - the DC removal, the quiet-noise damping, the untouched
// loud speech and the frame handling - because the point of the port is that a Russian phrase
// stops losing its endings in a noisy room, exactly as in the .NET build.

#include "domain/audio_wav.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what)
{
    ++checks;
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
        return;
    }
    ++failures;
    std::printf("  FAIL %s\n", what.c_str());
}

/// Root mean square of a block, the measure the filter evaluates per frame.
double rms(const std::vector<float>& samples, std::size_t from, std::size_t count)
{
    double sum = 0.0;
    std::size_t seen = 0;
    for (std::size_t i = from; i < from + count && i < samples.size(); ++i) {
        sum += static_cast<double>(samples[i]) * samples[i];
        ++seen;
    }
    return seen == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(seen));
}

std::vector<float> sine(std::size_t count, double amplitude, double offset = 0.0)
{
    std::vector<float> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<float>(offset + amplitude * std::sin(2.0 * 3.14159265358979 * 440.0
            * static_cast<double>(i) / 16000.0));
    }
    return out;
}

constexpr std::size_t kFrame = 256;

void the_high_pass_removes_the_constant_offset()
{
    std::vector<float> samples(4096, 0.25f);
    voicetyper::domain::suppress_noise(samples, 16000);
    // The first samples carry the filter's transient, so the settled tail is what shows that
    // the constant offset is gone.
    double worst = 0.0;
    for (std::size_t i = samples.size() - 512; i < samples.size(); ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(samples[i])));
    }
    check(worst < 0.001, "постоянная составляющая убрана (ВЧ-фильтр ~80 Гц)");
}

void silence_stays_silent_and_finite()
{
    std::vector<float> samples(4096, 0.0f);
    voicetyper::domain::suppress_noise(samples, 16000);
    bool finite = true;
    double worst = 0.0;
    for (const float value : samples) {
        finite = finite && std::isfinite(value);
        worst = std::max(worst, std::fabs(static_cast<double>(value)));
    }
    check(finite, "в тишине нет NaN и бесконечностей");
    check(worst == 0.0, "тишина остаётся тишиной");
}

void quiet_noise_is_damped_more_than_speech()
{
    // The floor is learned from quiet noise (amplitude 0.002), then a loud phrase
    // (amplitude 0.2) follows it. The noise must come out damped, the speech must not.
    std::vector<float> samples;
    const auto noise = sine(kFrame * 40, 0.002);
    const auto speech = sine(kFrame * 40, 0.2);
    samples.insert(samples.end(), noise.begin(), noise.end());
    samples.insert(samples.end(), speech.begin(), speech.end());

    const double noise_before = rms(samples, kFrame * 20, kFrame * 20);
    const double speech_before = rms(samples, noise.size() + kFrame * 20, kFrame * 20);

    voicetyper::domain::suppress_noise(samples, 16000);

    const double noise_after = rms(samples, kFrame * 20, kFrame * 20);
    const double speech_after = rms(samples, noise.size() + kFrame * 20, kFrame * 20);
    check(noise_after < noise_before * 0.9, "тихий шум приглушён");
    check(speech_after > speech_before * 0.9, "громкая речь проходит почти без потерь");
}

void the_result_is_stable_whatever_the_length()
{
    // The filter works in 256-sample frames: a length that is not a multiple of the frame must
    // not break it, and the sample rate argument must not change the result (the constants are
    // defined for the 16 kHz pipeline).
    std::vector<float> samples = sine(1000, 0.05, 0.1);
    voicetyper::domain::suppress_noise(samples, 16000);
    bool finite = true;
    for (const float value : samples) {
        finite = finite && std::isfinite(value);
    }
    check(finite, "неполный последний кадр не ломает фильтр");

    std::vector<float> other = sine(1000, 0.05, 0.1);
    voicetyper::domain::suppress_noise(other, 48000);
    check(samples == other, "частота дискретизации не меняет результат (константы для 16 кГц)");
}

} // namespace

int main()
{
    std::printf("noise-suppression-contract:\n");
    the_high_pass_removes_the_constant_offset();
    silence_stays_silent_and_finite();
    quiet_noise_is_damped_more_than_speech();
    the_result_is_stable_whatever_the_length();

    if (failures == 0) {
        std::printf("noise-suppression-contract: OK (%d checks)\n", checks);
        return 0;
    }
    std::printf("noise-suppression-contract: FAILED (%d of %d checks)\n", failures, checks);
    return 1;
}
