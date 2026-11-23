#include "app/capture_path_resolver.hpp"
#include "app/capture_switcher.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace app = voicetyper::app;
namespace domain = voicetyper::domain;
namespace platform = voicetyper::platform;
namespace audio = voicetyper::audio;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    std::printf(condition ? "  ok   %s\n" : "  FAIL %s\n", what.c_str());
    if (!condition) {
        ++g_failures;
    }
}

/// Stand-in for platform::WindowsAudioCapture: what the resolver and the switcher
/// need, with a scripted answer per candidate key.
struct FakeCapture {
    bool start_ok = true;
    bool delivers = true;
    std::string backend = "fake";
    std::string device;
    bool recording = false;
    bool drained = false;
    int starts = 0;

    domain::Status start(const domain::CancellationToken&)
    {
        ++starts;
        if (!start_ok) {
            return domain::Status::failure(domain::ErrorCode::unavailable, "start refused");
        }
        recording = true;
        return domain::Status::success();
    }

    domain::Result<audio::SampleBuffer> stop()
    {
        if (!recording) {
            return domain::Result<audio::SampleBuffer>::failure(
                domain::ErrorCode::invalid_state, "no session");
        }
        recording = false;
        audio::SampleBuffer buffer;
        if (delivers) {
            static_cast<void>(buffer.append(std::vector<float>{ 0.25F, -0.25F }));
        }
        return buffer;
    }

    domain::Status cancel()
    {
        recording = false;
        return domain::Status::success();
    }

    domain::Result<audio::SampleBuffer> drain()
    {
        audio::SampleBuffer buffer;
        // One non-empty drain, then the watermark makes it empty, like the real port.
        if (delivers && !drained) {
            drained = true;
            static_cast<void>(buffer.append(std::vector<float>{ 0.1F }));
        }
        return buffer;
    }

    double live_peak() const { return delivers ? 0.25 : 0.0; }
    std::string backend_description() const { return backend; }
    std::string device_id() const { return device; }
};

/// What the fake capture does for a candidate key: "<device>|native".
using Script = std::map<std::string, std::pair<bool, bool>>; // start_ok, delivers

std::string key_of(const app::CapturePathCandidate& candidate)
{
    return candidate.device_id + "|" + (candidate.allow_native_backend ? "native" : "interop");
}

std::vector<platform::MicrophoneDevice> two_devices()
{
    platform::MicrophoneDevice intel;
    intel.id = "intel";
    intel.name = "Intel array";
    intel.is_default = false;
    platform::MicrophoneDevice xreal;
    xreal.id = "xreal";
    xreal.name = "XREAL 1S";
    xreal.is_default = true;
    return { intel, xreal };
}

std::vector<std::string> ids_of(const std::vector<app::CapturePathCandidate>& candidates)
{
    std::vector<std::string> ids;
    for (const auto& candidate : candidates) {
        ids.push_back(candidate.device_id + (candidate.allow_native_backend ? "|native" : "|interop"));
    }
    return ids;
}

std::string join(const std::vector<std::string>& parts)
{
    std::string result;
    for (const auto& part : parts) {
        result += (result.empty() ? "" : ", ") + part;
    }
    return result;
}

} // namespace

int main()
{
    const auto devices = two_devices();

    // --- the order of the candidates ---------------------------------------
    {
        std::printf("capture-path-candidates\n");
        const auto unselected = ids_of(app::capture_path_candidates(devices, std::nullopt));
        check(join(unselected) == "xreal|interop, |native, intel|interop",
            "the default endpoint comes first, the native default second, the rest last");

        const auto selected_intel = ids_of(app::capture_path_candidates(devices, std::string("intel")));
        check(join(selected_intel) == "intel|interop, xreal|interop, |native",
            "the selected microphone is first and is not duplicated");

        const auto selected_default = ids_of(app::capture_path_candidates(devices, std::string("xreal")));
        check(join(selected_default) == "xreal|interop, |native, intel|interop",
            "a selected endpoint that is already the default does not repeat");
    }

    // --- the resolver picks the first candidate that delivers audio ---------
    {
        std::printf("capture-path-resolution\n");
        Script script;
        script["xreal|interop"] = { true, true };
        const auto choice = app::resolve_capture_path<FakeCapture>(devices, std::nullopt,
            [&script](const app::CapturePathCandidate& candidate) {
                auto capture = std::make_unique<FakeCapture>();
                const auto found = script.find(key_of(candidate));
                if (found != script.end()) {
                    capture->start_ok = found->second.first;
                    capture->delivers = found->second.second;
                }
                capture->backend = "fake " + candidate.device_id;
                return capture;
            },
            std::chrono::milliseconds(40));
        check(choice.found, "the working default endpoint is chosen");
        check(choice.candidate.device_id == "xreal", "the chosen candidate is the default endpoint");
        check(choice.attempts.empty(), "no candidate was rejected before the success");
        check(choice.backend == "fake xreal", "the backend of the chosen candidate is reported");
    }

    {
        // The AR glasses case: the default endpoint refuses to start, the native
        // backend on it starts and stays mute, and the other endpoint works.
        Script script;
        script["xreal|interop"] = { false, false };
        script["|native"] = { true, false };
        script["intel|interop"] = { true, true };
        const auto choice = app::resolve_capture_path<FakeCapture>(devices, std::nullopt,
            [&script](const app::CapturePathCandidate& candidate) {
                auto capture = std::make_unique<FakeCapture>();
                const auto found = script.find(key_of(candidate));
                if (found != script.end()) {
                    capture->start_ok = found->second.first;
                    capture->delivers = found->second.second;
                }
                return capture;
            },
            std::chrono::milliseconds(40));
        check(choice.found, "a dead default endpoint does not leave the app without audio");
        check(choice.candidate.device_id == "intel", "the other active endpoint is chosen");
        check(choice.attempts.size() == 2, "both rejected candidates are recorded");
        check(choice.attempts.size() == 2 && choice.attempts[0].find("start refused") != std::string::npos,
            "a start failure keeps its own reason");
        check(choice.attempts.size() == 2 && choice.attempts[1].find("did not deliver audio") != std::string::npos,
            "a mute endpoint is reported as no audio, not as a quiet microphone");
    }

    {
        std::printf("capture-path-nothing-delivers\n");
        const auto choice = app::resolve_capture_path<FakeCapture>(devices, std::nullopt,
            [](const app::CapturePathCandidate& candidate) {
                auto capture = std::make_unique<FakeCapture>();
                capture->delivers = false;
                if (candidate.allow_native_backend) {
                    capture->start_ok = true;
                }
                return capture;
            },
            std::chrono::milliseconds(30));
        check(!choice.found, "when nothing delivers the resolver reports no choice");
        check(choice.attempts.size() == 3, "every candidate is tried and recorded");
    }

    // --- the switching port -------------------------------------------------
    {
        std::printf("capture-switcher\n");
        int built = 0;
        std::string route = "first";
        app::SwitchingCapture<FakeCapture> port([&built, &route] {
            ++built;
            auto capture = std::make_unique<FakeCapture>();
            capture->backend = "fake " + route;
            return capture;
        });
        check(port.start(domain::CancellationToken{}).is_ok(), "the first session starts");
        check(port.is_recording(), "the port reports the running session");
        check(port.backend_description() == "fake first", "the backend belongs to the first route");
        const auto first_stop = port.stop();
        check(first_stop.is_ok() && first_stop.value().size() == 2, "the first session returns audio");
        check(!port.is_recording(), "the port is idle after stop");

        route = "second";
        port.reconfigure([&built, &route] {
            ++built;
            auto capture = std::make_unique<FakeCapture>();
            capture->backend = "fake " + route;
            return capture;
        });
        check(port.needs_new_capture(), "a reconfigured port builds the next capture on demand");
        check(port.start(domain::CancellationToken{}).is_ok(), "the next session starts on the new route");
        check(port.backend_description() == "fake second", "the backend follows the new route");
        check(built == 2, "exactly one capture is built per route");
        static_cast<void>(port.stop());

        // A swap during a recording must not cut the session: the switch waits.
        port.reconfigure([&built, &route] {
            ++built;
            auto capture = std::make_unique<FakeCapture>();
            capture->backend = "fake " + route;
            return capture;
        });
        check(port.start(domain::CancellationToken{}).is_ok(), "a session is running");
        route = "third";
        port.reconfigure([&built, &route] {
            ++built;
            auto capture = std::make_unique<FakeCapture>();
            capture->backend = "fake " + route;
            return capture;
        });
        check(port.backend_description() == "fake second", "the open session keeps its capture");
        check(built == 3, "no capture is built while a session is open");
        const auto second_stop = port.stop();
        check(second_stop.is_ok() && second_stop.value().size() == 2, "the open session still returns its audio");
        check(port.needs_new_capture(), "the new route is used by the next session");
    }

    if (g_failures == 0) {
        std::printf("capture-path-contract: OK\n");
        return 0;
    }
    std::printf("capture-path-contract: %d FAILURE(S)\n", g_failures);
    return 1;
}
