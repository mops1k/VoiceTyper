#include "domain/audio_wav.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

std::string fixture(const std::string& name)
{
    return (std::filesystem::path(VOICETYPER_SOURCE_DIR) / "tests/fixtures/migration/wav" / name).string();
}

void check_buffer()
{
    using namespace voicetyper::domain;
    SampleBuffer buffer(4);
    const std::vector<float> samples{0.1f, 0.2f, 0.3f};
    check(buffer.append(samples).is_ok(), "buffer accepts samples");
    check(buffer.append(samples).is_error(), "buffer enforces its capacity");
    check(buffer.size() == 3, "buffer keeps accepted samples");

    SampleBuffer bounded(8);
    const std::vector<float> too_many(9, 0.0f);
    check(bounded.append_d7_bounded(too_many.data(), too_many.size()).is_error(), "D7 bound rejects oversized append");
}

void check_wav_reader()
{
    using namespace voicetyper::domain;
    const auto speech = read_wav_file(fixture("wav-silence-speech-silence.wav"));
    check(speech.is_ok(), "16 kHz mono speech fixture loads");
    check(speech.value().size() == 32000, "speech fixture frame count");

    const auto odd = read_wav_file(fixture("wav-noncanonical-oddpad-chunk.wav"));
    check(odd.is_ok() && odd.value().size() == 4, "odd-padded chunk fixture loads all samples");
    const auto noncanonical = read_wav_file(fixture("wav-noncanonical-chunks.wav"));
    check(noncanonical.is_ok() && noncanonical.value().size() == 1 &&
        std::abs(noncanonical.value()[0] + 0.5f) < 1e-6f, "non-canonical chunk fixture returns -0.5");

    const auto rate = read_wav_file(fixture("wav-sine-44k-mono.wav"));
    check(rate.is_error() && rate.code() == ErrorCode::unsupported, "44.1 kHz is rejected by the strict reader");
    const auto stereo = read_wav_file(fixture("wav-reject-stereo.wav"));
    check(stereo.is_error() && stereo.code() == ErrorCode::unsupported, "stereo is rejected");
    const auto non_pcm = read_wav_file(fixture("wav-reject-nonpcm-format28.wav"));
    check(non_pcm.is_error() && non_pcm.code() == ErrorCode::unsupported, "non-PCM format is rejected");
}

void check_writer_and_resampler()
{
    using namespace voicetyper::domain;
    std::vector<float> source;
    for (int i = 0; i < 4410; ++i) {
        source.push_back(std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 44100.0f));
    }
    const auto mono = downmix_to_mono({0.25f, 0.25f, -0.5f, -0.5f});
    check(mono.size() == 2 && mono[0] == 0.25f && mono[1] == -0.5f, "stereo downmix averages channels");
    const auto resampled = resample_to_16k(source, 44100);
    check(resampled.size() == 1600, "44.1 kHz stream resamples to 16 kHz without a lost tail");
    check(std::abs(resampled.front() - source.front()) < 0.01f, "resampler keeps the stream start");

    const auto written = resample_to_16k(source, 44100);
    std::string bytes;
    check(write_wav_pcm16(written, bytes).is_ok(), "writer emits PCM16 mono WAV");
    const auto read_back = read_wav_pcm16(bytes);
    check(read_back.is_ok() && read_back.value().size() == written.size(), "WAV writer/reader round-trips");
    check(std::abs(read_back.value()[10] - written[10]) < 1.0f / 32768.0f, "WAV round-trip preserves samples");

    std::vector<float> quiet(512, 0.01f);
    suppress_noise(quiet);
    bool finite = true;
    for (const auto sample : quiet) {
        finite = finite && std::isfinite(sample);
    }
    check(finite && std::abs(quiet[0]) < 0.02f, "noise suppression is finite and damps a constant low signal");
}

void check_trim()
{
    using namespace voicetyper::domain;
    const auto speech = read_wav_file(fixture("wav-silence-speech-silence.wav"));
    const auto quiet = read_wav_file(fixture("wav-silence-quietspeech-silence.wav"));
    const auto long_pause = read_wav_file(fixture("wav-long-internal-pause.wav"));
    check(speech.is_ok() && quiet.is_ok() && long_pause.is_ok(), "trim fixtures load");
    if (!speech.is_ok() || !quiet.is_ok() || !long_pause.is_ok()) {
        return;
    }
    const auto trimmed_speech = trim_silence(speech.value());
    const auto trimmed_quiet = trim_silence(quiet.value());
    const auto trimmed_pause = trim_silence(long_pause.value());
    check(trimmed_speech.size() == 24000, "speech trim keeps the 1.5 s speech region");
    check(trimmed_quiet.size() == 24000, "quiet speech is not cut as silence");
    check(trimmed_pause.size() == 44800, "long internal pause is compressed to 2.8 s");
}

} // namespace

int main()
{
    check_buffer();
    check_wav_reader();
    check_writer_and_resampler();
    check_trim();
    if (failures != 0) {
        std::cerr << "audio-dsp-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "audio-dsp-contract: OK\n";
    return 0;
}
