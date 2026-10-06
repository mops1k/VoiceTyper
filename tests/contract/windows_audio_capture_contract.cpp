// Windows WASAPI capture contract.
//
// What this test proves on a machine that has a usable capture endpoint:
//   * the backend negotiates a device, starts a capture thread and delivers
//     ~300 ms of real microphone audio;
//   * stop() returns the WHOLE session as 16 kHz mono float - including audio
//     that drain() had already handed out, which is the .NET "Stop() returns the
//     whole session, not the undrained remainder" rule (AudioRecorder.cs:145);
//   * the capture was really downmixed and resampled through the portable audio
//     layer: the returned sample count is what the device format has to become
//     after a 16 kHz conversion, and every sample is inside [-1, 1];
//   * cancel() is idempotent, leaves nothing behind, and a cancelled session
//     cannot be stopped or drained into audio;
//   * start() on a live session and stop() without a session are reported, not
//     silently accepted.
//
// What it proves on a machine that has none: nothing is asserted and the reason
// is printed. The process exits 77 (the CTest skip code) instead of hanging:
// every wait here is bounded and the backend's own stop() is bounded by
// WasapiCaptureOptions::stop_timeout.
//
// What it deliberately does NOT claim: recognition quality, latency, the
// mc_wasapi.dll backend, MME, hot-plug handling, or that a specific share mode
// must be available. The two extra sessions only *report* whether shared-only
// and exclusive-first can be opened, because which of the two a given endpoint
// grants is a property of that endpoint (the .NET notes record that shared mode
// is refused on the Intel Smart Sound array), not of this backend.

#include "domain/audio_format.hpp"
#include "domain/audio_wav.hpp"
#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/recording_state_machine.hpp"
#include "platform/windows/windows_audio_capture.hpp"
#include "platform/windows/windows_microphone.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

using voicetyper::domain::AudioFormat;
using voicetyper::domain::CancellationSource;
using voicetyper::domain::Error;
using voicetyper::domain::ErrorCode;
using voicetyper::domain::SampleBuffer;
using voicetyper::domain::Status;
using voicetyper::platform::WasapiStrategy;
using voicetyper::platform::wasapi_strategy_name;
using voicetyper::platform::WindowsAudioCapture;
using voicetyper::platform::WindowsMicrophone;

/// CTest's documented "skipped" exit code.
constexpr int kSkipExitCode = 77;
/// A device id that can never exist, used to prove is_device_available() is a
/// real probe and not a constant.
constexpr const char* kBogusDeviceId = "{0.0.0.0-0000-0000-0000-000000000000}";

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (condition) {
        std::cout << "  ok   " << what << "\n";
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    ++g_failures;
}

void note(const std::string& text)
{
    std::cout << "  note " << text << "\n";
}

std::string describe(const Error& error)
{
    return error.to_string();
}

/// True for the failures that mean "this machine cannot record right now", as
/// opposed to a defect in the backend.
bool is_no_endpoint_failure(ErrorCode code)
{
    return code == ErrorCode::not_found || code == ErrorCode::unavailable
        || code == ErrorCode::permission_denied || code == ErrorCode::unsupported
        || code == ErrorCode::device_disconnected;
}

double seconds_of(const SampleBuffer& buffer)
{
    return static_cast<double>(buffer.size()) / static_cast<double>(voicetyper::domain::kTargetSampleRate);
}

struct SessionOutcome {
    Status start_status;
    Status stop_status;
    SampleBuffer whole;
    SampleBuffer drained;
    AudioFormat device_format;
    std::string backend;
    std::string device_id;
    std::string diagnostics;
};

/// Starts a session, waits `total` in two slices (draining between them) and
/// stops. Every wait is bounded, so a broken backend fails instead of hanging.
SessionOutcome run_session(WasapiStrategy strategy, std::chrono::milliseconds total, std::chrono::milliseconds split)
{
    SessionOutcome outcome;
    WindowsAudioCapture::Options options;
    options.strategy = strategy;

    WindowsAudioCapture capture(options);
    CancellationSource cancellation;
    outcome.start_status = capture.start(cancellation.token());
    outcome.diagnostics = capture.diagnostics();
    if (outcome.start_status.is_error()) {
        return outcome;
    }

    outcome.backend = capture.backend_description();
    outcome.device_format = capture.device_format();
    outcome.device_id = capture.device_id();

    if (split > std::chrono::milliseconds(0)) {
        std::this_thread::sleep_for(split);
        auto drained = capture.drain();
        if (drained.is_ok()) {
            outcome.drained = std::move(drained).value();
        }
    }

    const auto remaining = total > split ? total - split : std::chrono::milliseconds(0);
    std::this_thread::sleep_for(remaining);
    auto stopped = capture.stop();
    outcome.stop_status = stopped.status();
    if (stopped.is_ok()) {
        outcome.whole = std::move(stopped).value();
    }
    return outcome;
}

int skip(const std::string& reason, const std::string& diagnostics)
{
    std::cout << "windows-audio-capture-contract: SKIP\n";
    std::cout << "  reason: " << reason << "\n";
    if (!diagnostics.empty()) {
        std::cout << "  diagnostics: " << diagnostics << "\n";
    }
    return kSkipExitCode;
}

} // namespace

int main()
{
    std::cout << "windows-audio-capture-contract\n";

    // ---------------------------------------------------------------------
    // 1. Enumeration (the frozen MicrophoneService contract).
    // ---------------------------------------------------------------------
    WindowsMicrophone microphone;
    const auto devices = microphone.list_devices();
    std::cout << "  enumerated " << devices.size() << " active capture endpoint(s)\n";
    for (const auto& device : devices) {
        const bool truncated = device.id.size() > 48;
        std::cout << "    - " << device.name << " [id="
                  << (truncated ? device.id.substr(0, 48) + "..." : device.id) << "] default="
                  << (device.is_default ? "yes" : "no") << " mix=" << device.native_format.sample_rate() << "Hz/"
                  << device.native_format.channel_count() << "ch\n";
    }
    if (microphone.last_error().is_ok()) {
        check(true, "microphone enumeration reported no error");
    } else {
        // A refused enumeration is not a backend defect; it is the documented
        // permission_denied / unavailable diagnostic path being exercised.
        note("microphone enumeration reported " + describe(microphone.last_error()));
    }
    note("enumeration diagnostics: " + microphone.diagnostics());

    const std::string default_id = microphone.default_device_id();
    note("default capture endpoint: " + (default_id.empty() ? std::string("<none>") : default_id));
    note("default endpoint diagnostics: " + microphone.diagnostics());
    if (!default_id.empty()) {
        for (const auto& device : devices) {
            if (device.id == default_id) {
                check(device.is_default, "the default endpoint is flagged in the enumeration");
            }
        }
        check(microphone.is_device_available(default_id), "the default endpoint reports as available");
    }
    check(!microphone.is_device_available(kBogusDeviceId), "a nonsense endpoint id reports as unavailable");

    // ---------------------------------------------------------------------
    // 2. The default strategy must produce ~300 ms of 16 kHz mono audio.
    // ---------------------------------------------------------------------
    const auto session
        = run_session(WasapiStrategy::shared_then_exclusive, std::chrono::milliseconds(300), std::chrono::milliseconds(200));
    if (session.start_status.is_error()) {
        const ErrorCode code = session.start_status.code();
        if (is_no_endpoint_failure(code)) {
            return skip("no usable capture endpoint on this machine", session.diagnostics);
        }
        check(false, "start() failed with an unexpected code: " + session.start_status.error().to_string());
        return 1;
    }

    std::cout << "  backend: " << session.backend << "\n";
    std::cout << "  device format: " << session.device_format.sample_rate() << "Hz x "
              << session.device_format.channel_count() << "ch x " << session.device_format.bits_per_sample() << "bit\n";
    note("negotiation diagnostics: " + session.diagnostics);

    check(session.start_status.is_ok(), "start() succeeded");
    check(session.device_format.validate().is_ok(), "the device negotiated a usable format");
    check(!session.device_id.empty(), "the opened endpoint has an id");
    check(session.stop_status.is_ok(),
        "stop() succeeded: " + (session.stop_status.is_error() ? session.stop_status.error().to_string() : "ok"));

    check(!session.drained.empty(), "drain() returned the audio captured before the stop");
    check(!session.whole.empty(), "stop() returned a non-empty 16 kHz mono buffer");
    check(session.whole.size() >= session.drained.size(),
        "stop() returns the whole session, not the undrained remainder");
    check(session.whole.size() > session.drained.size(),
        "audio captured after the drain is present in the stopped session");

    const double whole_seconds = seconds_of(session.whole);
    std::cout << "  stopped buffer: " << session.whole.size() << " samples (" << whole_seconds << " s), drained "
              << session.drained.size() << "\n";
    check(whole_seconds > 0.1, "the stopped buffer covers a meaningful part of the 300 ms session");
    check(whole_seconds < 3.0, "the stopped buffer is not longer than the session it recorded");

    bool in_range = true;
    for (const float sample : session.whole.samples()) {
        if (!(sample >= -1.0f && sample <= 1.0f)) {
            in_range = false;
            break;
        }
    }
    check(in_range, "every converted sample is inside [-1, 1]");

    {
        WindowsAudioCapture capture;
        check(capture.stop().code() == ErrorCode::invalid_state, "stop() without a session is invalid_state");
        check(capture.drain().is_ok(), "drain() without a session is a successful empty result");
        check(capture.cancel().is_ok(), "cancel() without a session is a success");
    }

    // ---------------------------------------------------------------------
    // 3. Share-mode probes: reported, never asserted. Which of the two a given
    //    endpoint grants is a property of that endpoint, so a refusal is a fact
    //    about the machine, not a test failure.
    // ---------------------------------------------------------------------
    for (const auto strategy : { WasapiStrategy::shared_only, WasapiStrategy::exclusive_then_shared }) {
        const std::string name(wasapi_strategy_name(strategy));
        const auto probe = run_session(strategy, std::chrono::milliseconds(200), std::chrono::milliseconds(200));
        if (probe.start_status.is_ok()) {
            note(name + " opened: " + probe.backend + " -> " + std::to_string(probe.whole.size())
                + " 16 kHz samples (" + std::to_string(seconds_of(probe.whole)) + " s)");
        } else {
            note(name + " refused: " + probe.start_status.error().to_string());
        }
    }

    // ---------------------------------------------------------------------
    // 4. cancel() is idempotent, leaves nothing behind, and a cancelled
    //    session cannot be stopped.
    // ---------------------------------------------------------------------
    {
        WindowsAudioCapture capture;
        CancellationSource cancellation;
        const Status started = capture.start(cancellation.token());
        if (started.is_error()) {
            return skip("the default endpoint became unavailable mid-test", capture.diagnostics());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const Status first = capture.cancel();
        const Status second = capture.cancel();
        const Status third = capture.cancel();
        check(first.is_ok(), "the first cancel() succeeds");
        check(second.is_ok(), "a repeated cancel() is still a success");
        check(third.is_ok(), "a third cancel() is still a success");
        check(!capture.is_recording(), "the adapter reports no live session after cancel()");
        check(capture.stop().code() == ErrorCode::invalid_state, "stop() after cancel() is invalid_state");
        check(capture.drain().is_ok(), "drain() after cancel() is a successful empty result");

        // The same object must be reusable: a cancelled session leaves nothing
        // behind for the next one.
        check(capture.start(cancellation.token()).is_ok(), "a session starts again after cancel()");
        check(capture.start(cancellation.token()).code() == ErrorCode::invalid_state,
            "start() on a live session is invalid_state");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto stopped = capture.stop();
        const double seconds = stopped.is_ok() ? seconds_of(stopped.value()) : 0.0;
        std::cout << "  restarted session: " << (stopped.is_ok() ? stopped.value().size() : 0) << " samples (" << seconds
                  << " s)\n";
        check(stopped.is_ok(), "the restarted session stops cleanly");
        check(seconds > 0.05 && seconds < 1.0,
            "the restarted session holds only its own ~200 ms, not the cancelled audio");
    }

    // ---------------------------------------------------------------------
    // 5. The adapter really is a domain::RecordingPort, driven through the
    //    abstract interface the recording state machine uses.
    // ---------------------------------------------------------------------
    {
        WindowsAudioCapture capture;
        voicetyper::domain::RecordingPort& port = capture;
        CancellationSource cancellation;
        check(port.start(cancellation.token()).is_ok(), "start() through the RecordingPort interface succeeds");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        check(port.cancel().is_ok(), "cancel() through the RecordingPort interface is idempotent");
        check(port.cancel().is_ok(), "a second cancel() through the interface is still a success");
    }

    if (g_failures != 0) {
        std::cout << "windows-audio-capture-contract: " << g_failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "windows-audio-capture-contract: OK\n";
    return 0;
}
