#pragma once

// COM-based Windows WASAPI microphone capture.
//
// Evidence and contract:
//   * docs/migration/cpp/compatibility-contracts.md §5 ("Audio and ASR behavior")
//     and §2 (Windows lifecycle); the frozen `platform::AudioCapture` contract in
//     src/platform/api/audio_capture.hpp.
//   * .NET reference VoiceTyper.Core/Audio/RawWasapiCapture.cs: the instrumented
//     enumeration of (share mode x format x stream-flag x buffer size)
//     combinations, the EXCLUSIVE-first PCM16 48 kHz stereo attempt that the
//     Intel Smart Sound built-in array requires, and the role loop over
//     eConsole/eCommunications.
//   * .NET reference VoiceTyper.Core/Audio/AudioRecorder.cs: backend order
//     NATIVE -> RAW-WASAPI -> WASAPI -> MME, and the stop ordering
//     device -> unsubscribe -> snapshot with the buffer never cleared before the
//     snapshot.
//
// Division of responsibility: this file is *device I/O only*. It negotiates a
// device format and hands whole packets of interleaved PCM to a sink. It never
// converts, downmixes or resamples - that is domain::resample_to_16k /
// downmix_to_mono (see src/domain/audio_wav.hpp) and the WindowsAudioCapture
// adapter, exactly as src/platform/api/audio_capture.hpp requires.
//
// What this file deliberately does NOT do:
//   * it does not bind mc_wasapi.dll. The .NET NATIVE backend is a 114 KB
//     process-global C ABI with mc_start/mc_stop and no lifetime guarantee for
//     its callback (m_00379ac352a2); there is no ABI contract for it yet, so it
//     is not part of this slice.
//   * it does not implement the MME (waveIn) fallback.
//   * it does not handle hot-plug: a device removed while the stream is open
//     surfaces as a COM failure on the capture thread and is reported as
//     device_lost by stop(), not re-resolved onto another endpoint.
//
// Platform boundary: this header is standard C++20 and includes no <windows.h>,
// so a contract test can compile it on any host and the whole Win32/COM surface
// stays in wasapi_capture.cpp. Only the .cpp is compiled on Windows.
//
// Threading: start()/stop()/destroy() are called from the application thread and
// are mutually exclusive with each other. Packets are delivered on a dedicated
// capture thread, which is the only thread that touches IAudioClient /
// IAudioCaptureClient. The sink must not block, must not call back into the
// capture, and no exception may leave it (a throwing sink is caught at the
// boundary and treated as a capture failure).

#include "domain/audio_format.hpp"
#include "domain/cancellation.hpp"
#include "domain/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace voicetyper::platform {

using domain::AudioFormat;
using domain::CancellationToken;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// Share mode actually used by a started session.
enum class WasapiShareMode : std::uint8_t {
    none = 0,
    shared = 1,
    exclusive = 2,
};

[[nodiscard]] constexpr std::string_view wasapi_share_mode_name(WasapiShareMode mode) noexcept
{
    switch (mode) {
    case WasapiShareMode::none: return "none";
    case WasapiShareMode::shared: return "shared";
    case WasapiShareMode::exclusive: return "exclusive";
    }
    return "unknown";
}

/// The order in which start() negotiates a device configuration.
enum class WasapiStrategy : std::uint8_t {
    /// SHARED with the device mix format first (most robust: it goes through the
    /// Windows audio engine and works for every endpoint that exposes a shared
    /// mix format), then EXCLUSIVE PCM16 48 kHz stereo, which is the combination
    /// the Intel Smart Sound array needs, then a PCM16 shared retry. This is the
    /// default and the one the contract test exercises.
    shared_then_exclusive = 0,
    /// EXCLUSIVE first, shared second. Mirrors the .NET RawWasapiCapture order
    /// literally; kept because it is the order the existing hardware was
    /// validated with.
    exclusive_then_shared = 1,
    /// SHARED only. Used when exclusive mode is known to be refused (a machine
    /// policy or a driver that must not open in exclusive).
    shared_only = 2,
};

[[nodiscard]] constexpr std::string_view wasapi_strategy_name(WasapiStrategy strategy) noexcept
{
    switch (strategy) {
    case WasapiStrategy::shared_then_exclusive: return "shared_then_exclusive";
    case WasapiStrategy::exclusive_then_shared: return "exclusive_then_shared";
    case WasapiStrategy::shared_only: return "shared_only";
    }
    return "unknown";
}

/// One whole packet of interleaved PCM handed to the sink.
///
/// `data` is owned by the capture and valid only for the duration of the sink
/// call; a sink that needs the samples must copy them. `bytes` is always a whole
/// number of frames for `format`.
struct WasapiPacket {
    const std::byte* data = nullptr;
    std::uint32_t bytes = 0;
    std::uint32_t frames = 0;
    /// The format `data` is in. Carried per packet on purpose: a sink can be
    /// called before start() has returned to its caller, so the sink must never
    /// have to read a format field that the session owner has not published yet.
    AudioFormat format;
    /// True when the endpoint reported AUDCLNT_BUFFERFLAGS_SILENT, i.e. the
    /// packet is silence produced by the engine rather than device data. The
    /// bytes handed over are then a zero buffer owned by the capture, never the
    /// engine's packet memory, which is write-only in that case.
    bool silent = false;
};

/// Receives packets on the capture thread.
using WasapiPacketSink = std::function<void(const WasapiPacket&)>;

struct WasapiCaptureOptions {
    /// Empty = the system default capture endpoint. Otherwise an endpoint id
    /// previously produced by WindowsMicrophone (settings.microphoneDeviceId).
    std::string device_id;

    /// Strategy used to negotiate share mode / format. See WasapiStrategy.
    WasapiStrategy strategy = WasapiStrategy::shared_then_exclusive;

    /// Which endpoint role the default device is taken from. eConsole is tried
    /// first and eCommunications second, mirroring RawWasapiCapture.TryRole.
    /// A non-empty device_id ignores both roles.
    bool prefer_console_role = true;

    /// Requested capture format for the EXCLUSIVE attempts. Shared mode with an
    /// explicit PCM16 format is only tried when the mix format is not PCM16.
    std::uint32_t sample_rate = 48000;
    std::uint16_t channel_count = 2;

    /// How long the capture thread sleeps (polling mode) or waits on the audio
    /// event (event mode) when GetNextPacketSize reports nothing. Bounded on
    /// purpose: stop() must never wait longer than this after signalling.
    std::chrono::milliseconds idle_wait{3};

    /// Upper bound on how long stop() waits for the capture thread to join. The
    /// thread polls the stop event at `idle_wait`, so exceeding this means a
    /// defect (or a wedged COM call) and is reported as io_failure rather than
    /// hanging the UI thread forever.
    std::chrono::milliseconds stop_timeout{2000};
};

/// Why a capture session ended, reported after stop().
enum class WasapiEndReason : std::uint8_t {
    /// stop() was requested and the client stopped cleanly.
    completed = 0,
    /// The endpoint or the COM object failed while capturing (device removed,
    /// disabled, invalidated).
    device_lost = 1,
    /// A sink callback threw; the stream is stopped and the session fails.
    sink_failed = 2,
    /// The session's cancellation token fired.
    cancelled = 3,
};

[[nodiscard]] constexpr std::string_view wasapi_end_reason_name(WasapiEndReason reason) noexcept
{
    switch (reason) {
    case WasapiEndReason::completed: return "completed";
    case WasapiEndReason::device_lost: return "device_lost";
    case WasapiEndReason::sink_failed: return "sink_failed";
    case WasapiEndReason::cancelled: return "cancelled";
    }
    return "unknown";
}

/// Outcome of one start() attempt chain: the configuration that was accepted and
/// the per-candidate diagnostics of everything that was refused first.
struct WasapiCaptureReport {
    WasapiShareMode share_mode = WasapiShareMode::none;
    bool event_driven = false;
    std::uint32_t buffer_milliseconds = 0;
    /// Which candidate was accepted, 1-based, in the order start() tried them.
    int accepted_candidate = 0;
    /// Endpoint id that was opened (the default endpoint resolved to a real id,
    /// so a later is_device_available() check is possible).
    std::string device_id;
    /// One line per refused candidate: "shared/mix: init=0x88890008". Always
    /// populated on failure; that is the diagnostic the .NET build logs as
    /// `RAW-WASAPI: ...` and it is what makes a machine-specific failure
    /// explainable.
    std::string diagnostics;
};

/// A single WASAPI capture session.
///
/// Ownership/lifetime: the caller owns the object. Exactly one session may be
/// active at a time; start() on an already-started session returns invalid_state.
/// destroy() is idempotent, stops the device, joins the capture thread, releases
/// every COM object and can be called from the application thread.
class WasapiCapture {
public:
    virtual ~WasapiCapture() = default;

    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;
    WasapiCapture(WasapiCapture&&) = delete;
    WasapiCapture& operator=(WasapiCapture&&) = delete;

    /// Creates an idle capture object. Never fails; start() is where a device is
    /// actually opened.
    [[nodiscard]] static Result<std::unique_ptr<WasapiCapture>> create();

    /// Opens the endpoint and starts delivering packets to `sink`.
    ///
    /// Failure codes: not_found (no such device id / no default endpoint),
    /// permission_denied (OS privacy settings), unavailable (every candidate
    /// configuration was refused - the report detail lists the HRESULTs),
    /// device_disconnected, invalid_state (already capturing),
    /// unsupported (the device format is neither PCM16 nor IEEE float32),
    /// cancelled (the token was already requested).
    ///
    /// The returned report is only meaningful on success; on failure the same
    /// report is also available from report() so a caller can log the exact
    /// diagnostics after a failed start().
    [[nodiscard]] virtual Status start(
        const WasapiCaptureOptions& options, WasapiPacketSink sink, const CancellationToken& cancellation) = 0;

    /// Stops the device, joins the capture thread and drops the sink, in that
    /// order (device -> unsubscribe). Idempotent and safe on an idle object.
    /// Returns io_failure only when the capture thread could not be joined
    /// inside WasapiCaptureOptions::stop_timeout.
    [[nodiscard]] virtual Status stop() = 0;

    /// Stops and releases every COM object. Idempotent; also run by the
    /// destructor, which must never throw.
    virtual void destroy() noexcept = 0;

    [[nodiscard]] virtual bool is_capturing() const noexcept = 0;

    /// The format the device actually delivers, valid only after a successful
    /// start(). Before that it is the invalid default format (rate 0).
    [[nodiscard]] virtual AudioFormat device_format() const noexcept = 0;

    /// Why the last session ended. `completed` while no session has finished.
    [[nodiscard]] virtual WasapiEndReason end_reason() const noexcept = 0;

    /// The negotiated configuration and per-candidate diagnostics of the last
    /// start() attempt. Never throws and never returns null.
    [[nodiscard]] virtual WasapiCaptureReport report() const = 0;

protected:
    WasapiCapture() = default;
};

} // namespace voicetyper::platform
