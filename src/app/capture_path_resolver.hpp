#pragma once

// The capture-path policy: which microphone (and which backend) a dictation should
// open, decided by asking the devices themselves instead of trusting the OS default.
//
// Why it exists (Alexander, 2026-11-23: "when I plugged the AR glasses in, capture
// stopped working"): connecting the XREAL 1S glasses made Windows present their
// "Capture Input terminal" as the DEFAULT recording endpoint, and that endpoint
// accepts no format at all (shared 0x80070057, exclusive 0x88890008, measured with
// the capture contract and a standalone probe). The app followed the default,
// mc_wasapi.dll returned "success" without a single buffer, and the dictation was
// silent while every layer reported success. The OS default is therefore a hint,
// not an answer: a candidate is chosen only after it actually delivers audio.
//
// Order (the first candidate that delivers wins):
//   1. the microphone the user picked in the settings, if any;
//   2. the current default endpoint through the native backend (mc_wasapi.dll),
//      which starts in milliseconds and therefore keeps the dictation latency at
//      the level it had before this change;
//   3. the same default endpoint through the interop backend;
//   4. every other active endpoint through the interop backend, so a dead default
//      never leaves the app mute.
//
// `allow_native_backend` on a candidate means "this candidate is the system default
// endpoint", because mc_wasapi.dll takes no device id at all: the platform adapter
// only uses it when the resolver said so, and the first-buffer check still rejects a
// default endpoint that reports success and delivers nothing (the XREAL case).
//
// The policy is duck-typed on purpose: the contract test supplies a fake capture,
// and the composition supplies platform::WindowsAudioCapture, so no microphone and
// no Windows is needed to prove the order and the fallback.

#include "domain/cancellation.hpp"
#include "domain/recording_state_machine.hpp"
#include "platform/api/microphone.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace voicetyper::app {

/// One thing to try: a concrete endpoint (or the system default when the id is
/// empty) and whether the native mc_wasapi.dll backend may be used for it.
struct CapturePathCandidate {
    std::string device_id;
    bool allow_native_backend = false;
    /// Human-readable name for the log: the device name or "the default endpoint".
    std::string label;
};

/// What the resolver learned, for the journal and for the status line.
struct CapturePathChoice {
    bool found = false;
    CapturePathCandidate candidate;
    /// The backend that actually delivered, as the capture describes itself.
    std::string backend;
    /// One "<label>: <reason>" line per rejected candidate, in the order tried.
    std::vector<std::string> attempts;
};

/// How long one candidate may stay mute before it is rejected. A live endpoint
/// delivers its first buffer in milliseconds; the budget is the price of catching
/// an endpoint that claims success and never delivers (see the header comment).
inline constexpr std::chrono::milliseconds kCapturePathFirstSoundTimeout{ 800 };

/// How often the resolver polls drain() while waiting for the first audio.
inline constexpr std::chrono::milliseconds kCapturePathPollInterval{ 25 };

/// The candidate order described in the header comment. Public because the
/// contract test asserts it without running a resolver.
[[nodiscard]] inline std::vector<CapturePathCandidate> capture_path_candidates(
    const std::vector<platform::MicrophoneDevice>& devices,
    const std::optional<std::string>& selected_device_id)
{
    std::vector<CapturePathCandidate> candidates;
    const auto selected = selected_device_id.has_value() && !selected_device_id->empty()
            && *selected_device_id != "0"
        ? *selected_device_id
        : std::string();

    std::string default_id;
    std::string default_name = "the default endpoint";
    for (const auto& device : devices) {
        if (device.is_default) {
            default_id = device.id;
            default_name = device.name;
            break;
        }
    }
    const auto name_of = [&devices](const std::string& id) {
        for (const auto& device : devices) {
            if (device.id == id) {
                return device.name;
            }
        }
        return id;
    };
    // The native backend takes no device id, so both the empty id and the id of the
    // current default endpoint mean "the default endpoint" to it.
    const auto add = [&candidates](const std::string& id, bool native, const std::string& label) {
        for (const auto& candidate : candidates) {
            if (candidate.device_id == id && candidate.allow_native_backend == native) {
                return;
            }
        }
        candidates.push_back(CapturePathCandidate{ id, native, label });
    };

    if (!selected.empty()) {
        const bool selected_is_default = selected == default_id;
        add(selected, selected_is_default, selected_is_default ? default_name : name_of(selected));
        if (selected_is_default) {
            // The same endpoint through the interop backend, for a native backend that
            // cannot open it.
            add(selected, false, default_name);
        }
    }
    if (!default_id.empty() && default_id != selected) {
        add(default_id, true, default_name);
        add(default_id, false, default_name);
    } else if (default_id.empty() && selected.empty()) {
        // No endpoint reports itself as the default: the id stays empty and the
        // adapter asks Windows for whichever endpoint the role resolves to.
        add(std::string(), true, default_name);
        add(std::string(), false, default_name);
    }

    for (const auto& device : devices) {
        if (device.id == selected || device.id == default_id) {
            continue;
        }
        add(device.id, false, device.name);
    }
    return candidates;
}

/// Runs one candidate: creates a capture, starts it, waits for the first non-empty
/// drain() and stops it. True when audio actually arrived.
template <typename Capture, typename Factory>
[[nodiscard]] bool capture_path_candidate_delivers(Factory&& make,
    const CapturePathCandidate& candidate,
    std::string& backend,
    std::string& reason,
    const std::chrono::milliseconds first_sound_timeout = kCapturePathFirstSoundTimeout)
{
    std::unique_ptr<Capture> capture = make(candidate);
    if (capture == nullptr) {
        reason = "the capture could not be created";
        return false;
    }
    const auto started = capture->start(domain::CancellationToken{});
    if (started.is_error()) {
        reason = started.error().to_string();
        return false;
    }
    backend = capture->backend_description();
    const auto deadline = std::chrono::steady_clock::now() + first_sound_timeout;
    bool delivered = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto drained = capture->drain();
        if (drained.is_ok() && !drained.value().samples().empty()) {
            delivered = true;
            break;
        }
        std::this_thread::sleep_for(kCapturePathPollInterval);
    }
    const auto stopped = capture->stop();
    if (!delivered) {
        reason = stopped.is_error() ? stopped.error().to_string()
                                    : "the endpoint did not deliver audio in "
                + std::to_string(first_sound_timeout.count()) + " ms";
    }
    return delivered;
}

/// Chooses the capture path. `make` builds one capture for a candidate (the
/// composition passes platform::WindowsAudioCapture, the test a fake).
template <typename Capture, typename Factory>
[[nodiscard]] CapturePathChoice resolve_capture_path(
    const std::vector<platform::MicrophoneDevice>& devices,
    const std::optional<std::string>& selected_device_id,
    Factory&& make,
    const std::chrono::milliseconds first_sound_timeout = kCapturePathFirstSoundTimeout)
{
    CapturePathChoice choice;
    for (const auto& candidate : capture_path_candidates(devices, selected_device_id)) {
        std::string backend;
        std::string reason;
        if (capture_path_candidate_delivers<Capture>(
                make, candidate, backend, reason, first_sound_timeout)) {
            choice.found = true;
            choice.candidate = candidate;
            choice.backend = backend;
            return choice;
        }
        choice.attempts.push_back(candidate.label + ": " + reason);
    }
    return choice;
}

} // namespace voicetyper::app
