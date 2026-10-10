#pragma once

// Linux implementation of the domain::RecordingPort seam over the sound server.
//
// Evidence and contract:
//   * src/domain/recording_state_machine.hpp (RecordingPort): start(token),
//     stop() -> the WHOLE session as 16 kHz mono float, idempotent cancel(),
//     drain() for VAD mode, no exception across the boundary.
//   * src/platform/windows/windows_audio_capture.hpp - the same adapter shape on
//     Windows, so both platforms deliver the identical buffer format and the
//     identical "empty capture is not an error, it is an empty buffer" rule.
//   * docs/migration/cpp/release-gates.md risk R9 - "bounded queue, device-change
//     tests, exact diagnostics".
//
// Capture path: a PulseAudio/PipeWire *record stream* opened with pa_simple,
// which is the synchronous API of the sound server and therefore does not need a
// main loop of its own. The stream is opened at the device's usual format
// (48 kHz stereo PCM16, the same the Windows backend asks for) and converted to
// 16 kHz mono float by the portable DSP (domain::downmix_to_mono /
// domain::resample_to_16k), so the conversion is identical on both platforms and
// stays contract-tested without a microphone.
//
// Bounded queue (D7): the session buffer is capped at kMaxRecordingBytes of
// float samples. A session that would exceed it stops appending and reports
// resource_exhausted from stop() instead of returning a silently truncated
// recording.
//
// Threading: start/stop/cancel/drain are called from the application thread;
// audio arrives on one dedicated capture thread owned by this object. No lock is
// ever held while the device is being stopped, which is what keeps stop()
// deadlock-free.

#include "domain/audio_format.hpp"
#include "domain/audio_wav.hpp"
#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/recording_state_machine.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace voicetyper::platform::linuxos {

using domain::AudioFormat;
using domain::CancellationToken;
using domain::ErrorCode;
using domain::Result;
using domain::SampleBuffer;
using domain::Status;

class LinuxAudioCapture final : public domain::RecordingPort {
public:
    struct Options {
        /// Empty means the system default source. Otherwise a source name as
        /// reported by LinuxMicrophoneService (or by `pactl list sources`).
        std::string device_id;

        /// Format requested from the sound server. It may negotiate a different
        /// one; device_format() is authoritative.
        std::uint32_t sample_rate = 48000;
        std::uint16_t channel_count = 2;
    };

    LinuxAudioCapture();
    explicit LinuxAudioCapture(Options options);
    ~LinuxAudioCapture() override;

    LinuxAudioCapture(const LinuxAudioCapture&) = delete;
    LinuxAudioCapture& operator=(const LinuxAudioCapture&) = delete;
    LinuxAudioCapture(LinuxAudioCapture&&) = delete;
    LinuxAudioCapture& operator=(LinuxAudioCapture&&) = delete;

    /// Opens the source and begins the session. Failure codes: not_found (no such
    /// source), unavailable (no sound server), permission_denied, invalid_state
    /// (a session is already running), resource_exhausted. Every failure carries
    /// the sound server's own message as its detail.
    Status start(const CancellationToken& cancellation) override;

    /// Stops the stream, joins the capture thread, and only then converts the
    /// whole session to 16 kHz mono float. Failure codes: invalid_state (no live
    /// session), resource_exhausted (the D7 bound was hit), io_failure.
    [[nodiscard]] Result<SampleBuffer> stop() override;

    /// Stops the stream and discards the session. Idempotent: a second call (or a
    /// call with no session) is a success.
    Status cancel() override;

    /// Returns the audio captured since the previous drain, converted per chunk.
    /// Read-only: it moves a watermark and never clears, so a later stop() still
    /// returns the whole session.
    [[nodiscard]] Result<SampleBuffer> drain() override;

    /// The peak of the last ~0.2 s, 0..1: the settings window reads it while the
    /// microphone test runs.
    [[nodiscard]] double live_peak() const;

    [[nodiscard]] bool is_recording() const noexcept;
    /// The device format of the running (or last stopped) session. Rate 0 before
    /// the first start().
    [[nodiscard]] AudioFormat device_format() const noexcept;
    /// Source name actually opened, empty when no session ever succeeded.
    [[nodiscard]] std::string device_id() const;
    /// Human-readable backend, e.g. "pipewire-pulse 48000 Hz 2 ch".
    [[nodiscard]] std::string backend_description() const;
    /// The last failure's detail, for the log.
    [[nodiscard]] std::string diagnostics() const;
    /// True when the session hit the D7 bound; stop() then fails with
    /// resource_exhausted instead of returning a truncated buffer.
    [[nodiscard]] bool overflowed() const noexcept;

private:
    void capture_main();
    [[nodiscard]] std::vector<float> to_16k_mono_locked(std::size_t from, std::size_t to) const;

    const Options options_;

    mutable std::mutex mutex_;
    /// Signalled when the capture thread has either opened the stream or failed
    /// to open it, so start() never returns before the device is really open.
    std::condition_variable ready_signal_;
    bool start_finished_ = false;
    bool failed_ = false;
    ErrorCode start_error_code_ = ErrorCode::unavailable;
    bool recording_ = false;
    bool stopping_ = false;
    std::thread capture_thread_;
    /// Device-rate interleaved float samples of the whole session, in the device
    /// channel layout. Only the watermark moves on drain.
    std::vector<float> session_;
    std::size_t drained_ = 0;
    AudioFormat device_format_;
    std::string device_id_;
    std::string backend_;
    std::string diagnostics_;
    bool overflowed_ = false;
};

} // namespace voicetyper::platform::linuxos
