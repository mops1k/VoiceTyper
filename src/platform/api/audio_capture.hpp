#pragma once

// Microphone capture contract (device I/O only).
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §5 and
// docs/migration/cpp/feature-parity.md rows "Windows audio backends",
// "Audio conversion" and "Recording start gate"; .NET reference
// VoiceTyper.Core/Abstractions/IAudioRecorder.cs and
// VoiceTyper.Core/Audio/AudioRecorder.cs (backend chain
// mc_wasapi.dll -> Raw WASAPI -> NAudio WASAPI -> MME).
//
// Division of responsibility (intent-parity decision, recorded here because it
// changes the shape of the interface compared with IAudioRecorder):
//   * AudioCapture owns *device* I/O: open, negotiate a device format, deliver
//     frames, stop, cancel. It never converts or resamples.
//   * AudioConverter (audio_converter.hpp) owns the portable conversion/DSP that
//     produces the 16 kHz mono PCM16 WAV.
//   The composed recorder in Phase B reproduces the .NET WAV output; keeping
//   capture and conversion apart is what makes the conversion testable without a
//   microphone. This is a seam decision, not a behavior change.
//
// Device format: the native WASAPI path asks for 48 kHz stereo, ordinary
// backends usually deliver 44.1/48 kHz stereo PCM16. The negotiated format is
// reported by device_format() after a successful start() and is authoritative.
//
// The backend fallback order and the Intel Smart Sound physical validation are
// Phase D work; this header only requires that a failed backend chain surfaces
// as an error code instead of a silent zero-length capture.
//
// Thread affinity:
//   * start/stop/cancel/is_recording/device_format/drain are called from the
//     application (UI) thread.
//   * Frames are delivered on a sink that runs on a backend-owned audio thread.
//     The sink must not block, must not call back into the AudioCapture, and must
//     not let an exception escape (it cannot: no exception crosses this boundary).
//     Ownership of the frame buffer belongs to the sink call and ends when the
//     sink returns, unless the sink copies it.
//   * stop() must be safe to call from the UI thread while the audio thread is
//     running; it flushes and then joins. The implementation is responsible for
//     making the ordering idempotent.

#include "domain/audio_format.hpp"
#include "domain/cancellation.hpp"
#include "domain/error.hpp"

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace voicetyper::platform {

using domain::AudioBuffer;
using domain::AudioFormat;
using domain::CancellationToken;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// Why a capture session ended. Reported by stop() so the caller can tell an
/// empty capture from a device loss without inspecting logs.
enum class CaptureStopReason : std::uint8_t {
    /// stop() was called and frames were captured.
    completed = 0,
    /// stop() was called but the device produced no frames.
    empty = 1,
    /// cancel() was called; the partial buffer is discarded.
    cancelled = 2,
    /// The device disappeared or was disabled.
    device_lost = 3,
    /// The capture backend reported a failure.
    failed = 4,
};

[[nodiscard]] constexpr std::string_view capture_stop_reason_name(CaptureStopReason reason) noexcept
{
    switch (reason) {
    case CaptureStopReason::completed: return "completed";
    case CaptureStopReason::empty: return "empty";
    case CaptureStopReason::cancelled: return "cancelled";
    case CaptureStopReason::device_lost: return "device_lost";
    case CaptureStopReason::failed: return "failed";
    }
    return "unknown";
}

/// Request used to open a capture device.
///
/// The `device_id` semantics are intentionally left open: the .NET build passes
/// the selected endpoint id to the Raw/NAudio backends but not to Native/MME,
/// and the plan records that the C++ build must preserve the user's selected
/// device. Which backends honour which field is Phase D behavior with its own
/// tests, so this contract only fixes that a non-empty `device_id` means
/// "this specific device" and an empty one means "the system default".
struct CaptureStartOptions {
    /// Empty = system default device. Otherwise an opaque backend id.
    std::string_view device_id;

    /// Requested device format. Backends may negotiate a different format; the
    /// result of start() must be read from device_format().
    AudioFormat requested_format{48000, 2, domain::SampleFormat::pcm_s16};

    /// Optional cancellation for the whole capture session.
    CancellationToken cancellation;
};

/// Frames delivered by the capture backend.
///
/// `bytes` is owned by the AudioCapture and valid only for the duration of the
/// sink invocation. A sink that needs the data must copy it.
struct CapturedFrame {
    AudioFormat format;
    /// Raw interleaved PCM, a whole number of frames.
    std::uint64_t bytes = 0;
};

/// Receives captured frames on the backend audio thread.
using CaptureFrameSink = std::function<void(const CapturedFrame&)>;

/// A microphone capture session.
///
/// Ownership: the caller owns the AudioCapture instance and must destroy it
/// before the process-wide audio subsystem is torn down. destroy() is
/// idempotent; calling it while recording stops and discards the session.
class AudioCapture {
public:
    virtual ~AudioCapture() = default;

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;
    AudioCapture(AudioCapture&&) = delete;
    AudioCapture& operator=(AudioCapture&&) = delete;

    /// True between a successful start() and stop()/cancel().
    [[nodiscard]] virtual bool is_recording() const noexcept = 0;

    /// The format the device actually delivers. Valid only while recording or
    /// after a session that has been stopped; an invalid AudioFormat (rate 0)
    /// before the first start() is expected and not an error.
    [[nodiscard]] virtual AudioFormat device_format() const noexcept = 0;

    /// Installs the frame sink. Must be called before start(). The sink is owned
    /// by the capture backend for the lifetime of the session.
    virtual Status set_frame_sink(CaptureFrameSink sink) = 0;

    /// Opens the device and begins delivering frames.
    /// Failure codes: not_found (no such device), permission_denied (the OS
    /// blocked capture, e.g. privacy settings), unavailable (no backend could
    /// open any device), device_disconnected, invalid_state (already recording).
    virtual Status start(const CaptureStartOptions& options) = 0;

    /// Stops capture and returns everything captured since the last drain.
    /// `empty` reason means the device produced no frames; the caller must treat
    /// that as "nothing recorded" and must not hand a zero-length WAV to the
    /// engine.
    [[nodiscard]] virtual Result<AudioBuffer> stop() = 0;

    /// Stops capture and discards the buffered audio. Idempotent.
    virtual Status cancel() = 0;

    /// Returns the bytes captured since the previous drain without stopping.
    /// This is the VAD-mode primitive: the recording state machine polls it so
    /// the VAD sees the newest audio. A successful empty drain is not an error.
    [[nodiscard]] virtual Result<AudioBuffer> drain() = 0;

    /// Releases the device and any backend resources. Idempotent and safe to
    /// call from the UI thread; blocks until the audio thread has stopped.
    virtual void destroy() = 0;

protected:
    AudioCapture() = default;
};

} // namespace voicetyper::platform
