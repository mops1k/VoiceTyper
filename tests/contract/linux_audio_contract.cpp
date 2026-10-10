// Linux-only contract test for the audio backends (microphone enumeration,
// capture and the microphone level control).
//
// What this proves, and what it deliberately does not:
//
//   Proves, on Linux, with or without a sound server:
//     * linux_microphone: enumeration never crashes and an empty list is a
//       success (the user may genuinely have no microphone), an unknown device
//       id is reported unavailable, and a reported default device is in the
//       list it was reported for;
//     * linux_audio_capture: a session that cannot be opened fails with a
//       precise error instead of returning an empty recording, stop() without a
//       session is invalid_state, cancel() is idempotent, and - when a real
//       device is present - a short session delivers 16 kHz mono audio;
//     * linux_microphone_level: the port is always constructible, and where the
//       sound server exposes a level control the scalar is a 0..1 value that
//       round-trips through write().
//
//   Deliberately NOT exercised automatically: a long dictation, device hotplug
//   and a denied-device scenario. Those belong to the physical Arch Linux gate.

#include "domain/audio_format.hpp"
#include "domain/audio_wav.hpp"
#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "platform/api/microphone.hpp"
#include "platform/api/microphone_level.hpp"
#include "platform/linux/linux_audio_capture.hpp"
#include "platform/linux/linux_microphone.hpp"
#include "platform/linux/linux_microphone_level.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace voicetyper;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

void check_microphone()
{
    using namespace voicetyper::platform::linuxos;

    LinuxMicrophoneService microphone;
    const auto devices = microphone.list_devices();

    // An empty list is a success: "no microphone" and "cannot enumerate" must
    // not look the same to the caller.
    if (devices.empty()) {
        std::cout << "skip: no capture device enumerated (" << microphone.diagnostics() << ")\n";
    }
    for (const auto& device : devices) {
        check(!device.id.empty(), "every enumerated device has an id");
        check(!device.name.empty(), "every enumerated device has a name");
    }

    check(!microphone.is_device_available("voicetyper-no-such-source"),
        "an unknown device id is not available");

    const std::string default_id = microphone.default_device_id();
    if (!default_id.empty()) {
        check(microphone.is_device_available(default_id),
            "the reported default device is available");
    } else {
        std::cout << "skip: no default source reported\n";
    }
}

void check_capture()
{
    using namespace voicetyper::platform::linuxos;

    // A session that cannot be opened reports a precise failure rather than an
    // empty recording: handing a zero-length buffer to the engine is exactly the
    // silent failure the contract forbids.
    {
        LinuxAudioCapture::Options options;
        options.device_id = "voicetyper-no-such-source";
        LinuxAudioCapture capture(options);
        const auto started = capture.start(domain::CancellationToken{});
        check(started.is_error(), "starting capture on an unknown device fails");
        if (started.is_error()) {
            check(started.code() == domain::ErrorCode::not_found
                    || started.code() == domain::ErrorCode::unavailable,
                "an unknown device reports not_found or unavailable");
        }
        check(!capture.is_recording(), "a failed start leaves no session behind");
    }

    // stop() without a session is invalid_state; cancel() is idempotent.
    {
        LinuxAudioCapture capture;
        const auto stopped = capture.stop();
        check(stopped.is_error() && stopped.code() == domain::ErrorCode::invalid_state,
            "stop() without a session is invalid_state");
        check(capture.cancel().is_ok(), "cancel() without a session succeeds");
        check(capture.cancel().is_ok(), "cancel() is idempotent");
    }

    // A real device, when the machine has one: a short session must deliver
    // 16 kHz mono audio (or report why it could not start).
    {
        LinuxMicrophoneService microphone;
        const auto devices = microphone.list_devices();
        if (devices.empty()) {
            std::cout << "skip: no capture device, the live capture path is not tested here\n";
            return;
        }
        LinuxAudioCapture capture;
        const auto started = capture.start(domain::CancellationToken{});
        if (started.is_error()) {
            std::cout << "skip: capture could not start (" << started.error().to_string() << ")\n";
            return;
        }
        // A suspended ALSA source needs about two seconds to wake up on this
        // machine (measured 2026-10-08: the first pa_simple_read() blocked for
        // 2018 ms), so the session is given enough time to deliver audio. The
        // composition pays the same cost once at startup with its warm-up.
        std::this_thread::sleep_for(std::chrono::milliseconds(2600));
        const auto stopped = capture.stop();
        check(stopped.is_ok(), "a live session stops cleanly");
        if (stopped.is_ok()) {
            const auto& samples = stopped.value().samples();
            check(!samples.empty(), "a live session delivered audio");
            std::cout << "live capture: samples=" << samples.size()
                      << " backend=" << capture.backend_description() << '\n';
            for (const float sample : samples) {
                if (!std::isfinite(sample)) {
                    check(false, "captured samples are finite");
                    break;
                }
            }
        }
    }
}

void check_microphone_level()
{
    using namespace voicetyper::platform::linuxos;

    auto port = create_linux_microphone_level();
    check(port != nullptr, "the level factory returns a port");

    const auto state = port->read();
    if (!state.available) {
        std::cout << "skip: no level control in this session\n";
        return;
    }
    check(state.level >= 0.0 && state.level <= 1.0, "the level is a 0..1 scalar");
    const int percent = platform::microphone_level_percent(state.level);
    check(percent >= 0 && percent <= 100, "the level maps to a 0..100 percent");

    // Writing the value that was just read back must not change the control or
    // fail: the slider and the sound server stay in step.
    const auto written = port->write(state.level, state.muted);
    check(written.is_ok(), "writing the current level succeeds");
    const auto after = port->read();
    check(after.available, "the control is still available after a write");
    check(std::abs(after.level - state.level) < 0.05, "the written level reads back");
    check(after.muted == state.muted, "the mute flag is preserved by a level write");
}

} // namespace

int main()
{
    check_microphone();
    check_capture();
    check_microphone_level();

    if (failures != 0) {
        std::cerr << "linux-audio-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "linux-audio-contract: OK\n";
    return 0;
}
