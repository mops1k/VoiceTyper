#pragma once

// Portable audio buffer, PCM16 WAV and DSP primitives. This slice contains no
// capture backend, VAD, ASR or platform code; it only turns deterministic byte
// fixtures into the normalized float stream consumed by the later stages.

#include "domain/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::domain {

inline constexpr std::size_t kTargetSampleRate = 16000;
inline constexpr std::size_t kDefaultAudioCapacity = 16000 * 60;
/// D7: one recording is bounded at 512 MiB of float samples.
inline constexpr std::size_t kMaxRecordingBytes = 512U * 1024U * 1024U;

class SampleBuffer {
public:
    explicit SampleBuffer(std::size_t capacity = kDefaultAudioCapacity);

    [[nodiscard]] Status append(const float* samples, std::size_t count);
    [[nodiscard]] Status append(const std::vector<float>& samples);
    [[nodiscard]] Status append_d7_bounded(const float* samples, std::size_t count);

    [[nodiscard]] const std::vector<float>& samples() const noexcept { return samples_; }
    [[nodiscard]] std::size_t size() const noexcept { return samples_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return samples_.empty(); }
    void clear() noexcept { samples_.clear(); }

private:
    std::size_t capacity_;
    std::vector<float> samples_;
};

/// Strict WavPcmReader contract: PCM16, mono, 16 kHz, RIFF chunk cursor
/// 8 + size + (size & 1), int16/32768 conversion. Rejection order is
/// audioFormat, bitsPerSample, channels, sampleRate as in the C# reference.
[[nodiscard]] Result<std::vector<float>> read_wav_pcm16(std::string_view bytes);
/// Byte-oriented overload for WavAudio::file_view(); the span is contiguous, so
/// this is the same parse without copying the file into a string.
[[nodiscard]] Result<std::vector<float>> read_wav_pcm16(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::vector<float>> read_wav_file(const std::string& path);

/// Minimal canonical PCM16 mono 16 kHz writer; no implicit resampling.
[[nodiscard]] Status write_wav_pcm16(
    const std::vector<float>& samples,
    std::string& bytes,
    std::uint32_t sample_rate = static_cast<std::uint32_t>(kTargetSampleRate));

/// Continuous fractional-position linear resampler. It accepts a whole source
/// stream and never discards a fractional tail; callers that stream capture
/// should keep the position in the same rational state across calls.
[[nodiscard]] std::vector<float> resample_to_16k(
    const std::vector<float>& samples,
    std::uint32_t source_rate);

/// Stereo-to-mono average downmix (the NAudio reference uses an average).
[[nodiscard]] std::vector<float> downmix_to_mono(const std::vector<float>& interleaved_stereo);

/// Port of VoiceTyper.Core/Audio/NoiseSuppressor.cs; processes float samples
/// in place with the frozen 256-frame high-pass/adaptive-damp constants.
void suppress_noise(std::vector<float>& samples, std::size_t sample_rate = kTargetSampleRate);

/// Port of VoiceTyper.Core/Audio/SilenceTrimmer.cs with its frozen constants.
[[nodiscard]] std::vector<float> trim_silence(
    const std::vector<float>& samples,
    std::size_t sample_rate = kTargetSampleRate,
    double margin_seconds = 0.25,
    double max_silence_seconds = 0.6,
    double gap_silence_seconds = 0.3);

} // namespace voicetyper::domain
