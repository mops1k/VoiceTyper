#pragma once

// Audio conversion / DSP contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §5 "Audio and ASR
// behavior" and docs/migration/cpp/feature-parity.md rows "Audio conversion",
// "Noise reduction" and "Silence trimming"; .NET reference
// VoiceTyper.Core/Audio/WavBuilder.cs, NoiseSuppressor.cs, SilenceTrimmer.cs and
// WavPcmReader.cs.
//
// Frozen observable contract (Phase B implements and differential-tests it):
//   * Output is a complete WAV file: RIFF/WAVE, PCM16, mono, 16 kHz, with the
//     canonical 44-byte header. Whisper and parakeet-tdt both require exactly
//     this, so it is a compatibility contract and not an implementation detail.
//   * Input may be any device format; conversion downmixes to mono and resamples
//     to 16 kHz.
//   * Optional denoise, then optional leading/trailing silence trim, in that
//     order, exactly as WavBuilder.ConvertTo16KHzMonoWav does it.
//
// Deliberately NOT decided here (Phase B with differential tests, per the plan):
//   * which resampler is used (the current WDL resampler is a behavioral
//     reference, not a code dependency);
//   * how a fractional resampling tail is produced;
//   * the noise-reduction and trim tuning constants.
//
// The contract therefore only constrains the *interface*: it must never lose the
// tail of the input, and it must be a pure function of its inputs so the
// differential harness can compare it byte-for-byte against the .NET fixtures.
//
// Thread affinity: pure and reentrant. Callable from any thread, including a
// worker thread while the UI thread stays responsive. No shared mutable state.

#include "domain/audio_format.hpp"
#include "domain/error.hpp"

#include <cstdint>
#include <vector>

namespace voicetyper::platform {

using domain::AudioBuffer;
using domain::AudioFormat;
using domain::ErrorCode;
using domain::Result;
using domain::SampleFormat;
using domain::Status;
using domain::WavAudio;

/// Options for a single conversion pass.
struct AudioConvertOptions {
    /// Apply the noise suppressor before trimming (settings.noiseReductionEnabled).
    bool noise_reduction = false;
    /// Trim leading/trailing silence after the optional denoise pass.
    bool trim_silence = true;
};

/// The format every conversion produces. Also the format of every WAV file the
/// app writes to disk and of every buffer handed to a Transcriber.
[[nodiscard]] constexpr AudioFormat target_transcription_format() noexcept
{
    return AudioFormat::whisper_input();
}

/// Converts captured audio into the transcription format.
///
/// Ownership: the returned WavAudio owns its byte buffer; the converter keeps no
/// reference to the input after convert() returns. The input buffer may be
/// reused by the caller immediately afterwards.
class AudioConverter {
public:
    virtual ~AudioConverter() = default;

    AudioConverter(const AudioConverter&) = delete;
    AudioConverter& operator=(const AudioConverter&) = delete;
    AudioConverter(AudioConverter&&) = delete;
    AudioConverter& operator=(AudioConverter&&) = delete;

    /// Converts `input` (in `input_format`) into a 16 kHz mono PCM16 WAV.
    ///
    /// Failure codes:
    ///   invalid_argument - the input format is invalid, or the byte count is not
    ///                     a whole number of frames (a truncated capture);
    ///   unsupported      - the input sample format cannot be converted;
    ///   corrupt_data     - the byte count cannot describe any audio at all.
    ///
    /// A successful result with a zero-length payload is a valid outcome for
    /// silent or fully trimmed input; callers decide what to do with it.
    [[nodiscard]] virtual Result<WavAudio> convert(
        const AudioBuffer& input, const AudioConvertOptions& options) const = 0;

    /// Headerless 16 kHz mono float samples, used by the VAD segmenter.
    /// No denoise and no trim: the VAD must see the unaltered stream.
    [[nodiscard]] virtual Result<std::vector<float>> convert_for_vad(const AudioBuffer& input) const = 0;

protected:
    AudioConverter() = default;
};

} // namespace voicetyper::platform
