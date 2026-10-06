#pragma once

// Audio format contract shared by the capture backends, the conversion/DSP
// layer and the transcriber input contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §5 ("Audio and ASR
// behavior"): the Whisper-facing format is PCM16 / mono / 16 kHz, while capture
// devices deliver 44.1/48 kHz stereo PCM16.
//
// This header describes *what* a format is, never *how* samples are produced or
// converted. Resampler/DSP policy belongs to Phase B (differential tests) and is
// deliberately absent here.

#include "domain/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace voicetyper::domain {

/// Sample encoding of a raw (headerless) PCM buffer.
enum class SampleFormat : std::uint8_t {
    /// Signed 16-bit little-endian integer PCM. What capture and Whisper use.
    pcm_s16 = 0,
    /// IEEE-754 binary32, host byte order. Intermediates only; never a file format.
    ieee_float32 = 1,
};

[[nodiscard]] constexpr std::string_view sample_format_name(SampleFormat format) noexcept
{
    switch (format) {
    case SampleFormat::pcm_s16: return "pcm_s16";
    case SampleFormat::ieee_float32: return "ieee_float32";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::uint16_t sample_format_bits(SampleFormat format) noexcept
{
    switch (format) {
    case SampleFormat::pcm_s16: return 16;
    case SampleFormat::ieee_float32: return 32;
    }
    return 0;
}

[[nodiscard]] constexpr std::uint16_t sample_format_bytes(SampleFormat format) noexcept
{
    return static_cast<std::uint16_t>(sample_format_bits(format) / 8);
}

/// Sample rate required by Whisper and by every WAV file the app produces.
inline constexpr std::uint32_t kWhisperSampleRate = 16000;
/// Channel count required by Whisper: mono after downmix.
inline constexpr std::uint16_t kWhisperChannelCount = 1;
/// Bits per sample of the WAV output contract.
inline constexpr std::uint16_t kWhisperBitsPerSample = 16;

/// Byte length of the canonical 44-byte RIFF/WAVE header written by the app.
inline constexpr std::uint64_t kWavHeaderBytes = 44;

/// Immutable description of a linear PCM stream.
///
/// Ownership/threading: value type with no interior mutability, so it is safe to
/// pass by value across threads and to store in a callback payload.
class AudioFormat {
public:
    /// An invalid/empty format: rate 0, 0 channels, PCM16. Always validate()
    /// before use; a default format is never silently accepted.
    constexpr AudioFormat() noexcept = default;

    constexpr AudioFormat(std::uint32_t sample_rate, std::uint16_t channel_count, SampleFormat sample_format) noexcept
        : sample_rate_(sample_rate)
        , channel_count_(channel_count)
        , sample_format_(sample_format)
    {
    }

    /// The Whisper transcription input contract: 16 kHz, mono, PCM16.
    /// Also the exact format of the WAV files the app writes (see WavAudio).
    [[nodiscard]] static constexpr AudioFormat whisper_input() noexcept
    {
        return AudioFormat(kWhisperSampleRate, kWhisperChannelCount, SampleFormat::pcm_s16);
    }

    [[nodiscard]] constexpr std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] constexpr std::uint16_t channel_count() const noexcept { return channel_count_; }
    [[nodiscard]] constexpr SampleFormat sample_format() const noexcept { return sample_format_; }

    [[nodiscard]] constexpr std::uint16_t bits_per_sample() const noexcept
    {
        return sample_format_bits(sample_format_);
    }

    /// One sample across all channels.
    [[nodiscard]] constexpr std::uint32_t bytes_per_frame() const noexcept
    {
        return static_cast<std::uint32_t>(bytes_per_sample()) * channel_count_;
    }

    /// One sample of one channel.
    [[nodiscard]] constexpr std::uint32_t bytes_per_sample() const noexcept
    {
        return sample_format_bytes(sample_format_);
    }

    [[nodiscard]] constexpr std::uint32_t bytes_per_second() const noexcept
    {
        return bytes_per_frame() * sample_rate_;
    }

    /// Number of frames a byte count of `byte_count` represents. A partial
    /// trailing frame is not counted; callers must reject short buffers via
    /// validate_buffer rather than silently dropping the tail.
    [[nodiscard]] constexpr std::uint64_t frame_count(std::uint64_t byte_count) const noexcept
    {
        const auto frame = static_cast<std::uint64_t>(bytes_per_frame());
        return frame == 0 ? 0 : byte_count / frame;
    }

    /// Wall-clock duration of a frame count. 0 when the rate is unknown.
    [[nodiscard]] constexpr double duration_seconds(std::uint64_t frames) const noexcept
    {
        if (sample_rate_ == 0) {
            return 0.0;
        }
        return static_cast<double>(frames) / static_cast<double>(sample_rate_);
    }

    /// Rejects a zero sample rate, zero channels, an unknown sample format and
    /// a buffer whose length is not a whole number of frames.
    [[nodiscard]] Status validate() const
    {
        if (sample_rate_ == 0) {
            return Status::failure(ErrorCode::invalid_argument, "sample rate must be greater than zero");
        }
        if (channel_count_ == 0) {
            return Status::failure(ErrorCode::invalid_argument, "channel count must be greater than zero");
        }
        if (sample_format_bits(sample_format_) == 0) {
            return Status::failure(ErrorCode::invalid_argument, "unknown sample format");
        }
        return Status::success();
    }

    [[nodiscard]] Status validate_buffer(std::uint64_t byte_count) const
    {
        const Status format_status = validate();
        if (format_status.is_error()) {
            return format_status;
        }
        if (byte_count % bytes_per_frame() != 0) {
            return Status::failure(ErrorCode::corrupt_data, "buffer length is not a whole number of frames");
        }
        return Status::success();
    }

    /// True when this format is exactly the Whisper input contract.
    [[nodiscard]] constexpr bool is_whisper_input() const noexcept
    {
        return sample_rate_ == kWhisperSampleRate && channel_count_ == kWhisperChannelCount
            && sample_format_ == SampleFormat::pcm_s16;
    }

    friend constexpr bool operator==(const AudioFormat& lhs, const AudioFormat& rhs) noexcept
    {
        return lhs.sample_rate_ == rhs.sample_rate_ && lhs.channel_count_ == rhs.channel_count_
            && lhs.sample_format_ == rhs.sample_format_;
    }

private:
    std::uint32_t sample_rate_ = 0;
    std::uint16_t channel_count_ = 0;
    SampleFormat sample_format_ = SampleFormat::pcm_s16;
};

/// A raw PCM block plus the format it is in. `bytes` is always a whole number of
/// frames once `format.validate_buffer()` passes.
struct AudioBuffer {
    AudioFormat format;
    std::uint64_t bytes = 0;

    [[nodiscard]] constexpr std::uint64_t frame_count() const noexcept { return format.frame_count(bytes); }
    [[nodiscard]] constexpr double duration_seconds() const noexcept { return format.duration_seconds(frame_count()); }
};

/// A complete, self-describing WAV file: RIFF/WAVE, PCM16, mono, 16 kHz.
///
/// `data` owns the whole file, including the 44-byte canonical header, so it can
/// be handed to Whisper/parakeet unchanged. `bytes` is the authoritative size of
/// that file; the two are kept consistent by construction and by assign_file().
///
/// Why the buffer lives here: `AudioConverter::convert()` documents that the
/// returned WavAudio owns its byte buffer, and `Transcriber::transcribe()` needs
/// the samples. A metadata-only struct (format + length) could satisfy neither,
/// which is why a length-only field is not enough on its own.
struct WavAudio {
    AudioFormat format = AudioFormat::whisper_input();
    /// Whole-file bytes. Empty for a header-only, size-only description.
    std::vector<std::byte> data;
    /// Size of the whole file, header included.
    std::uint64_t bytes = 0;

    WavAudio() = default;
    explicit WavAudio(std::vector<std::byte> file, AudioFormat file_format = AudioFormat::whisper_input())
        : format(file_format)
        , data(std::move(file))
        , bytes(static_cast<std::uint64_t>(data.size()))
    {
    }

    /// Replaces the file contents and keeps `bytes` consistent.
    void assign_file(std::vector<std::byte> file)
    {
        data = std::move(file);
        bytes = static_cast<std::uint64_t>(data.size());
    }

    /// The complete file, header included. Empty when only metadata is present.
    [[nodiscard]] std::span<const std::byte> file_view() const noexcept
    {
        return std::span<const std::byte>(data.data(), data.size());
    }

    /// The WAV body without the canonical header.
    [[nodiscard]] std::span<const std::byte> payload_view() const noexcept
    {
        if (data.size() <= kWavHeaderBytes) {
            return {};
        }
        return std::span<const std::byte>(data.data() + kWavHeaderBytes, data.size() - kWavHeaderBytes);
    }

    [[nodiscard]] constexpr std::uint64_t frame_count() const noexcept { return format.frame_count(bytes); }
    [[nodiscard]] constexpr double duration_seconds() const noexcept { return format.duration_seconds(frame_count()); }

    /// The WAV body is `bytes` minus the canonical 44-byte header.
    [[nodiscard]] constexpr std::uint64_t payload_bytes() const noexcept
    {
        return bytes > kWavHeaderBytes ? bytes - kWavHeaderBytes : 0;
    }

    /// True when the struct carries a real, non-empty file buffer.
    [[nodiscard]] bool has_samples() const noexcept { return !data.empty() && payload_bytes() > 0; }
};

} // namespace voicetyper::domain
