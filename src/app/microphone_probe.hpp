#pragma once

// The microphone-probe policy, shared by both composition roots and testable without a
// device.
//
// Why it exists: on the live Windows build (12.10.2026) the FIRST probe of a fresh
// application session started a capture that reported success and then delivered nothing
// at all - the log line read "buffers=0 bytes=0" - while the second press heard the user
// (buffers=468). A standalone harness built from the same backend did not reproduce it
// (six sessions out of six delivered audio from the first second, with and without a
// 13 s idle gap), so this rule deliberately does not depend on the cause: do not trust a
// successful start(), wait for the first buffer, restart the session once if it stays
// mute, and report "the device delivered no audio" as its own verdict instead of
// accusing the user's microphone of being quiet.
//
// The capture surface is duck-typed on purpose: platform::WindowsAudioCapture and
// platform::linuxos::LinuxAudioCapture both provide it, and the contract test supplies a
// fake that stays mute until its second start.

#include "domain/cancellation.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>

namespace voicetyper::app {

/// What one probe concluded.
enum class MicrophoneProbeVerdict : std::uint8_t {
    /// Audio arrived and the peak was above the floor.
    heard = 0,
    /// Audio arrived, but the peak stayed below the floor: a quiet, muted or distant mic.
    silent = 1,
    /// No session delivered a single sample: the device produced no audio at all.
    no_audio = 2,
};

/// Everything the probe learned, for the journal and for the window.
struct MicrophoneProbeReport {
    MicrophoneProbeVerdict verdict = MicrophoneProbeVerdict::no_audio;
    double peak = 0.0;
    /// Milliseconds from the first start() to the first delivered sample, -1 if none.
    long long first_buffer_ms = -1;
    /// Capture sessions started: 2 when the first one stayed mute and was restarted.
    int sessions = 0;
    /// Non-empty when a start() failed; the verdict is then no_audio.
    std::string start_error;
    /// The backend's own description, including its buffer/byte counters.
    std::string backend;
};

/// Peak floor: 0.5 % of full scale. Below it the microphone is silent, muted or not
/// connected - the floor the .NET build never checked.
inline constexpr double kMicrophonePeakFloor = 0.005;

/// The live-peak cadence the settings window's meter follows.
inline constexpr std::chrono::milliseconds kMicrophoneProbeTick{50};

/// How long a session may stay mute before it is restarted once.
inline constexpr std::chrono::milliseconds kMicrophoneFirstBufferTimeout{2500};

/// The whole probe window: long enough to watch the meter follow the voice and to press
/// "Остановить", short enough that the block never hangs without a verdict.
inline constexpr std::chrono::milliseconds kMicrophoneProbeWindow{8000};

/// Runs one probe against `capture`, which must provide start/stop/drain/live_peak/
/// backend_description. `publish_level` receives the live peak on every tick.
template <typename Capture, typename PublishLevel>
[[nodiscard]] MicrophoneProbeReport run_microphone_probe(Capture& capture,
    const std::atomic<bool>& cancel,
    PublishLevel&& publish_level,
    const std::chrono::milliseconds first_buffer_timeout = kMicrophoneFirstBufferTimeout,
    const std::chrono::milliseconds total_window = kMicrophoneProbeWindow)
{
    using clock = std::chrono::steady_clock;
    MicrophoneProbeReport report;
    const auto started_at = clock::now();
    const auto cancelled = [&cancel] { return cancel.load(); };

    // Did the current session deliver anything yet? drain() is read-only - it moves a
    // watermark and never clears - so asking does not cost stop() any of the audio.
    const auto session_has_audio = [&capture] {
        const auto drained = capture.drain();
        return drained.is_ok() && !drained.value().samples().empty();
    };

    const auto start_session = [&]() {
        ++report.sessions;
        const auto status = capture.start(domain::CancellationToken{});
        if (status.is_error()) {
            report.start_error = status.error().to_string();
            return false;
        }
        report.start_error.clear();
        return true;
    };

    const auto note_first_buffer = [&] {
        if (report.first_buffer_ms < 0) {
            report.first_buffer_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                clock::now() - started_at).count();
        }
    };

    const auto wait_for_first_buffer = [&](const std::chrono::milliseconds timeout) {
        const auto deadline = clock::now() + timeout;
        while (!cancelled() && clock::now() < deadline) {
            if (session_has_audio()) {
                note_first_buffer();
                return true;
            }
            publish_level(capture.live_peak());
            std::this_thread::sleep_for(kMicrophoneProbeTick);
        }
        if (session_has_audio()) {
            note_first_buffer();
            return true;
        }
        return false;
    };

    if (!start_session()) {
        report.backend = capture.backend_description();
        return report;
    }

    bool delivering = wait_for_first_buffer(first_buffer_timeout);
    if (!delivering && !cancelled()) {
        // The session started successfully and stayed mute - exactly what the live build
        // showed on the first press. Restarting it once is what the user did by hand.
        static_cast<void>(capture.stop());
        if (start_session()) {
            delivering = wait_for_first_buffer(first_buffer_timeout);
        }
    }

    const auto window_end = started_at + total_window;
    while (!cancelled() && clock::now() < window_end) {
        publish_level(capture.live_peak());
        std::this_thread::sleep_for(kMicrophoneProbeTick);
    }
    publish_level(0.0);

    const auto stopped = capture.stop();
    if (stopped.is_ok()) {
        for (const float sample : stopped.value().samples()) {
            report.peak = std::max(report.peak, std::abs(static_cast<double>(sample)));
        }
    }
    report.backend = capture.backend_description();

    if (!delivering && report.first_buffer_ms < 0) {
        report.verdict = MicrophoneProbeVerdict::no_audio;
    } else {
        report.verdict = report.peak > kMicrophonePeakFloor ? MicrophoneProbeVerdict::heard
                                                            : MicrophoneProbeVerdict::silent;
    }
    return report;
}

} // namespace voicetyper::app
