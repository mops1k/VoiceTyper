// Contract for the microphone-probe policy (src/app/microphone_probe.hpp).
//
// Written from the live-build report of 12.10.2026: the first probe of a fresh app
// session started successfully and delivered nothing (buffers=0), the second press heard
// the user. The capture is a fake here, so the rule - wait for the first sample, restart
// the session once if it stays mute, and never call "no audio" a quiet microphone - is
// pinned without a device.

#include "app/microphone_probe.hpp"
#include "audio/audio_wav.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using voicetyper::app::MicrophoneProbeVerdict;

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FAIL %s\n", what.c_str());
    } else {
        std::printf("  ok   %s\n", what.c_str());
    }
}

/// A capture stand-in that can be mute on its first session, mute forever, quiet or loud.
class FakeCapture {
public:
    /// The first start() reports success and delivers nothing: the live-build symptom.
    bool mute_first_session = false;
    /// No start() ever delivers a sample.
    bool never_delivers = false;
    /// The amplitude of the delivered samples.
    float amplitude = 0.5f;
    /// When set, start() fails with this message.
    std::string start_failure;

    int starts = 0;
    int stops = 0;

    [[nodiscard]] voicetyper::domain::Status start(const voicetyper::domain::CancellationToken&)
    {
        ++starts;
        if (!start_failure.empty()) {
            return voicetyper::domain::Status::failure(
                voicetyper::domain::ErrorCode::unavailable, start_failure);
        }
        delivering_ = !never_delivers && !(mute_first_session && starts == 1);
        return voicetyper::domain::Status::success();
    }

    [[nodiscard]] voicetyper::domain::Result<voicetyper::audio::SampleBuffer> stop()
    {
        ++stops;
        voicetyper::audio::SampleBuffer buffer;
        if (delivering_) {
            const std::vector<float> samples(4800, amplitude);
            static_cast<void>(buffer.append(samples));
        }
        delivering_ = false;
        return buffer;
    }

    [[nodiscard]] voicetyper::domain::Result<voicetyper::audio::SampleBuffer> drain()
    {
        voicetyper::audio::SampleBuffer buffer;
        if (delivering_ && !drained_) {
            const std::vector<float> one{amplitude};
            static_cast<void>(buffer.append(one));
            drained_ = true;
        }
        return buffer;
    }

    [[nodiscard]] double live_peak() const { return delivering_ ? static_cast<double>(amplitude) : 0.0; }

    [[nodiscard]] std::string backend_description() const
    {
        return "fake backend buffers=" + std::to_string(delivering_ ? 1 : 0) + " bytes=0";
    }

private:
    bool delivering_ = false;
    bool drained_ = false;
};

/// Short windows keep the contract fast; the production values live in the header.
constexpr auto kFastFirstBuffer = std::chrono::milliseconds(150);
constexpr auto kFastWindow = std::chrono::milliseconds(250);

voicetyper::app::MicrophoneProbeReport run(FakeCapture& capture, std::atomic<bool>& cancel)
{
    return voicetyper::app::run_microphone_probe(
        capture, cancel, [](double) {}, kFastFirstBuffer, kFastWindow);
}

void the_mute_first_session_is_restarted_and_then_heard()
{
    FakeCapture capture;
    capture.mute_first_session = true;
    std::atomic<bool> cancel{false};
    const auto report = run(capture, cancel);

    check(report.verdict == MicrophoneProbeVerdict::heard,
        "a session that stays mute is restarted and the second one is heard");
    check(report.sessions == 2, "exactly two sessions were started");
    check(capture.stops >= 2, "the mute session was stopped before the restart");
    check(report.first_buffer_ms >= 0, "the time to the first sample is recorded");
    check(report.peak > voicetyper::app::kMicrophonePeakFloor, "the peak comes from the session");
}

void a_device_that_never_delivers_is_no_audio()
{
    FakeCapture capture;
    capture.never_delivers = true;
    std::atomic<bool> cancel{false};
    const auto report = run(capture, cancel);

    check(report.verdict == MicrophoneProbeVerdict::no_audio,
        "a device that never delivers is reported as no_audio, not as a quiet microphone");
    check(report.first_buffer_ms == -1, "no first sample is claimed");
    check(report.sessions == 2, "the probe still tried a restart once");
}

void a_quiet_session_is_silent_not_absent()
{
    FakeCapture capture;
    capture.amplitude = 0.001f;
    std::atomic<bool> cancel{false};
    const auto report = run(capture, cancel);

    check(report.verdict == MicrophoneProbeVerdict::silent,
        "audio arrived but stayed under the floor: silent, not no_audio");
    check(report.sessions == 1, "a delivering session is not restarted");
}

void a_loud_session_is_heard()
{
    FakeCapture capture;
    std::atomic<bool> cancel{false};
    const auto report = run(capture, cancel);

    check(report.verdict == MicrophoneProbeVerdict::heard, "a loud session is heard");
    check(report.sessions == 1, "one session is enough when it delivers");
    check(report.backend.find("fake backend") == 0, "the backend text is passed through");
}

void a_start_failure_reports_its_reason()
{
    FakeCapture capture;
    capture.start_failure = "no capture device";
    std::atomic<bool> cancel{false};
    const auto report = run(capture, cancel);

    check(report.verdict == MicrophoneProbeVerdict::no_audio, "a failed start is no_audio");
    // Error::to_string() carries the code as well ("unavailable: no capture device").
    check(report.start_error.find("no capture device") != std::string::npos,
        "the start error is reported with its message");
    check(report.sessions == 1, "a failed start is not retried inside the probe");
}

void a_cancelled_probe_returns_a_verdict()
{
    FakeCapture capture;
    capture.never_delivers = true;
    std::atomic<bool> cancel{true};
    const auto report = run(capture, cancel);

    check(report.sessions == 1, "a cancelled probe does not start a second session");
    check(report.verdict == MicrophoneProbeVerdict::no_audio, "a cancelled probe still returns a verdict");
}

} // namespace

int main()
{
    std::printf("microphone-probe-contract:\n");
    the_mute_first_session_is_restarted_and_then_heard();
    a_device_that_never_delivers_is_no_audio();
    a_quiet_session_is_silent_not_absent();
    a_loud_session_is_heard();
    a_start_failure_reports_its_reason();
    a_cancelled_probe_returns_a_verdict();

    if (failures != 0) {
        std::printf("microphone-probe-contract: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("microphone-probe-contract: OK (%d checks)\n", checks);
    return 0;
}
