#pragma once

// domain::RecordingPort adapter over the Windows WASAPI capture backend.
//
// Evidence and contract:
//   * src/domain/recording_state_machine.hpp (`RecordingPort`): start(CancellationToken),
//     stop() -> Result<SampleBuffer> holding the WHOLE session, idempotent
//     cancel(), drain() for VAD mode, no exception across the boundary.
//   * src/platform/api/audio_capture.hpp: the .NET stop ordering
//     device -> unsubscribe -> snapshot, and "the buffer is never cleared before
//     the snapshot" (VoiceTyper.Core/Audio/AudioRecorder.cs:145-173). The
//     .NET Stop() returns the whole session, not the undrained remainder,
//     because drain() only moves a watermark; the same holds here.
//   * docs/migration/cpp/compatibility-contracts.md §5: the buffer handed up is
//     16 kHz mono float, so the conversion is done by the portable audio layer
//     (domain::downmix_to_mono / domain::resample_to_16k) and not by this file.
//
// Why the conversion lives here and not in the capture backend: the backend is
// device I/O (src/platform/windows/wasapi_capture.hpp documents the split), and
// keeping conversion out of it is what lets the DSP be contract-tested without a
// microphone. This adapter is the seam that joins them.
//
// Backend order: this adapter is the *NATIVE* link of the .NET chain
// NATIVE -> RAW-WASAPI -> WASAPI -> MME. mc_wasapi.dll is the native backend the
// .NET build actually used (VoiceTyper.Core/Audio/NativeWasapiCapture.cs), and it
// is tried first; the interop WASAPI path below is the fallback. There is no MME
// fallback, so a failure of both surfaces as an ErrorCode instead of a silent
// zero-length capture.
//
// Threading: start/stop/cancel/drain are called from the application thread.
// Packets arrive on the WASAPI capture thread and are appended to the session
// buffer under a short mutex; no call ever holds that mutex while the device
// is being stopped, which is what keeps stop() deadlock-free.

#include "domain/audio_format.hpp"
#include "domain/audio_wav.hpp"
#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/recording_state_machine.hpp"
#include "platform/mc_wasapi.hpp"
#include "platform/windows/wasapi_capture.hpp"

#include <atomic>
#include <condition_variable>
#include <thread>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace voicetyper::platform {

class WindowsAudioCapture final : public domain::RecordingPort {
public:
    struct Options {
        /// Empty (or the legacy "0" alias) means the system default endpoint.
        std::string device_id;

        /// Share mode / format negotiation order, see WasapiStrategy.
        WasapiStrategy strategy = WasapiStrategy::shared_then_exclusive;

        /// Requested device format for the explicit-format candidates. The
        /// device may negotiate something else; device_format() is authoritative.
        std::uint32_t sample_rate = 48000;
        std::uint16_t channel_count = 2;
    };

    /// The default construction captures the system default endpoint with the
    /// shared-then-exclusive strategy.
    WindowsAudioCapture();

    explicit WindowsAudioCapture(Options options);

    ~WindowsAudioCapture() override;

    WindowsAudioCapture(const WindowsAudioCapture&) = delete;
    WindowsAudioCapture& operator=(const WindowsAudioCapture&) = delete;
    WindowsAudioCapture(WindowsAudioCapture&&) = delete;
    WindowsAudioCapture& operator=(WindowsAudioCapture&&) = delete;

    /// Opens the endpoint and begins the session. Failure codes: not_found,
    /// permission_denied, unavailable, device_disconnected, unsupported,
    /// invalid_state (a session is already running), resource_exhausted,
    /// cancelled. Every failure carries the per-candidate HRESULT diagnostics
    /// as its detail, because "could not open the microphone" without the
    /// reason is the .NET diagnostic gap this backend closes.
    Status start(const domain::CancellationToken& cancellation) override;

    /// Stops the device, drops the sink, and only then snapshots the whole
    /// session as 16 kHz mono float. Idempotent in effect: with no live session
    /// it returns invalid_state and touches nothing.
    [[nodiscard]] Result<domain::SampleBuffer> stop() override;

    /// Stops the device and discards the session. Idempotent: a second call (or
    /// a call with no session) is a success and touches no device.
    Status cancel() override;

    /// Returns the audio captured since the previous drain, resampled per chunk.
    /// Read-only: it moves a watermark and never clears, so a later stop() still
    /// returns the WHOLE session (the .NET DrainNewBytes/Stop pair).
    [[nodiscard]] Result<domain::SampleBuffer> drain() override;

    [[nodiscard]] bool is_recording() const noexcept;
    /// The device format of the running (or last stopped) session. Rate 0 before
    /// the first start().
    [[nodiscard]] domain::AudioFormat device_format() const noexcept;
    /// Endpoint id actually opened, empty when no session ever succeeded.
    [[nodiscard]] std::string device_id() const;
    /// Human-readable backend, e.g. "WASAPI exclusive/poll 100ms".
    [[nodiscard]] std::string backend_description() const;
    /// The last start()'s per-candidate diagnostics, or the last stop error.
    /// Returned by value because it is written by the application thread while
    /// a capture thread may be running.
    [[nodiscard]] std::string diagnostics() const;
    /// Why the last session's capture thread ended.
    [[nodiscard]] WasapiEndReason end_reason() const noexcept;
    /// True when a session hit the D7 512 MiB bound; stop() then fails with
    /// resource_exhausted instead of returning a silently truncated buffer.
    [[nodiscard]] bool overflowed() const noexcept;

private:
    void append_packet(const WasapiPacket& packet);

    /// The negotiated client is kept between dictations - that is what makes the second
    /// start fast instead of paying the ~1 s negotiation again - but not forever. A
    /// client left initialised holds the endpoint inside the driver, and a process that
    /// is then killed mid-capture leaves the device allocated for EVERY application:
    /// that is the state this machine was in (MME answered MMSYSERR_ALLOCATED, WASAPI
    /// E_INVALIDARG for every format, and the installed .NET build's own microphone
    /// test reported no data). The reaper releases the session once the adapter has
    /// been idle, so the device is not held while the user is not dictating and
    /// consecutive dictations still start immediately.
    void start_idle_reaper();
    void note_session_stopped();
    static constexpr auto kIdleReleaseDelay = std::chrono::seconds(20);
    std::mutex idle_mutex_;
    std::condition_variable idle_cv_;
    std::thread idle_reaper_;
    bool idle_reaper_stop_ = false;
    std::chrono::steady_clock::time_point last_activity_{};

    /// The native mc_wasapi.dll session, set only while that path is the one in use;
    /// mc_api_ is loaded once and kept, because the DLL stays loaded for the process.
    std::unique_ptr<McWasapiSession> mc_session_;
    McWasapiApi mc_api_;
    /// How much the native library actually delivered in the current session. The
    /// first thing to check when "the capture runs but nothing is recognised" is
    /// whether its callback fired at all, and these numbers answer that from the log.
    std::atomic<std::size_t> native_buffers_{0};
    std::atomic<std::size_t> native_bytes_{0};
    [[nodiscard]] std::vector<float> to_16k_mono_locked(std::size_t from, std::size_t to) const;

    const Options options_;

    mutable std::mutex mutex_;
    std::unique_ptr<WasapiCapture> capture_;
    bool recording_ = false;
    /// Device-rate interleaved float samples of the whole session, in the device
    /// channel layout. Only the watermark moves on drain.
    std::vector<float> session_;
    std::size_t drained_ = 0;
    domain::AudioFormat device_format_;
    std::string device_id_;
    std::string backend_;
    std::string diagnostics_;
    WasapiEndReason end_reason_ = WasapiEndReason::completed;
    bool overflowed_ = false;
};

} // namespace voicetyper::platform
