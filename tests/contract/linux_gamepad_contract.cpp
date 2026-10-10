// Linux-only contract test for the evdev gamepad backend.
//
// What this proves, and what it deliberately does not:
//
//   Proves, on Linux, with no controller and no /dev/input access at all:
//     * the binding grammar handling: an absent or empty binding is a success
//       ("no gamepad action"), a present but malformed binding is
//       invalid_argument, and a "DInput|Product|index" binding parses but never
//       matches - the documented parity gap with the Windows backend;
//     * the XInputPadButton -> evdev table: every one of the 16 bindable buttons
//       is recognised from its BTN_*/ABS_* code, the analog triggers use the
//       threshold of the .NET reference (30) and normalise a signed
//       -32768..32767 axis to 0..255, and the D-pad reads the hat axes;
//     * debounced press/release edges (2 samples by default), the cancel edge,
//       and that re-binding resets the edge state;
//     * capture_next(): the first pressed button, cancellation through the
//       token, cancel_capture(), "every pad went away" -> device_disconnected,
//       and a second concurrent capture refused rather than orphaning the first;
//     * devices(): empty without devices (not an error), a connected pad named
//       with its evdev node and product, and device loss that neither stops the
//       service nor spins the re-enumeration;
//     * stop() joins the poll thread and is idempotent, and a throwing sink
//       cannot escape into the poll thread.
//
//   Deliberately NOT exercised automatically: a real controller press. It
//   belongs to the physical gate (plan p_b5fbda6bfa1d, phase 5); the production
//   reader is smoke-tested only for "does not hang or crash, and reports the
//   node count it sees".

#include "domain/settings.hpp"
#include "platform/api/gamepad.hpp"
#include "platform/linux/linux_gamepad.hpp"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace voicetyper;
using namespace std::chrono_literals;
using domain::AppSettings;
using domain::GamepadSource;
using platform::GamepadAction;
using platform::GamepadEdge;
using platform::linuxos::EvdevAxisValue;
using platform::linuxos::EvdevGamepadSnapshot;
using platform::linuxos::GamepadDeviceReader;
using platform::linuxos::LinuxGamepadService;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

/// Bounded wait. Returns false on timeout, so no scenario can hang.
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds deadline, std::chrono::milliseconds step = 2ms)
{
    const auto until = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < until) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return predicate();
}

// ---------------------------------------------------------------------------
// Fake bus: the injected reader seam, so no hardware is needed
// ---------------------------------------------------------------------------

/// One poll's view of /dev/input: the list of gamepad nodes the reader sees.
using BusState = std::vector<EvdevGamepadSnapshot>;

/// A scripted stand-in for the evdev reader. One script step per poll: the
/// reader advances the cursor on every call and clamps on the last step, so a
/// one-step script is a permanent state and a longer script is a timeline.
class FakePads {
public:
    void set_script(std::vector<BusState> script)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        script_ = std::move(script);
        cursor_ = 0;
    }

    [[nodiscard]] int polls() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return polls_;
    }

    [[nodiscard]] GamepadDeviceReader reader()
    {
        return [this]() {
            const std::lock_guard<std::mutex> lock(mutex_);
            ++polls_;
            if (script_.empty()) {
                return BusState{};
            }
            const std::size_t at = std::min(cursor_, script_.size() - 1);
            if (cursor_ < script_.size()) {
                ++cursor_;
            }
            return script_[at];
        };
    }

private:
    mutable std::mutex mutex_;
    std::vector<BusState> script_;
    std::size_t cursor_ = 0;
    int polls_ = 0;
};

/// Waits until the poll thread has consumed the current script at least twice.
/// Without it a poll that is already in flight could still hold the previous
/// script and answer a capture with a button from the wrong step.
void settle(FakePads& pads, int polls_before)
{
    wait_for([&pads, polls_before]() { return pads.polls() >= polls_before + 2; }, 2000ms);
}

EvdevGamepadSnapshot key_pad(std::initializer_list<std::uint16_t> keys){
    EvdevGamepadSnapshot snapshot;
    snapshot.connected = true;
    snapshot.node_index = 0;
    snapshot.product_name = "Fake Pad";
    snapshot.pressed_keys.assign(keys.begin(), keys.end());
    return snapshot;
}

EvdevGamepadSnapshot axis_pad(std::initializer_list<EvdevAxisValue> axes)
{
    EvdevGamepadSnapshot snapshot;
    snapshot.connected = true;
    snapshot.node_index = 0;
    snapshot.product_name = "Fake Pad";
    snapshot.axes.assign(axes.begin(), axes.end());
    return snapshot;
}

EvdevGamepadSnapshot idle_pad()
{
    return key_pad({});
}

BusState bus(EvdevGamepadSnapshot pad)
{
    return BusState{std::move(pad)};
}

/// Counts the delivered edges. The sink is called on the poll thread.
struct EdgeCounter {
    mutable std::mutex mutex;
    std::vector<GamepadAction> actions;

    platform::GamepadEventSink sink()
    {
        return [this](const GamepadEdge& edge) {
            const std::lock_guard<std::mutex> lock(mutex);
            actions.push_back(edge.action);
        };
    }

    [[nodiscard]] std::vector<GamepadAction> snapshot() const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return actions;
    }

    [[nodiscard]] int count(GamepadAction action) const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        int total = 0;
        for (const auto candidate : actions) {
            if (candidate == action) {
                ++total;
            }
        }
        return total;
    }
};

/// Runs a capture that is expected to find no button: the token is cancelled
/// from another thread after a short delay, so the test can never hang on a
/// default (never-cancellable) token.
domain::ErrorCode capture_with_timeout(LinuxGamepadService& service, std::chrono::milliseconds delay)
{
    domain::CancellationSource source;
    std::thread canceller([&source, delay] {
        std::this_thread::sleep_for(delay);
        source.request_cancellation();
    });
    const auto result = service.capture_next(source.token());
    canceller.join();
    return result.is_ok() ? domain::ErrorCode::ok : result.code();
}

/// A service tuned for the test: fast poll, the production debounce of 2.
struct Fixture {
    FakePads pads;
    EdgeCounter edges;
    LinuxGamepadService service{pads.reader()};

    Fixture()
    {
        service.set_poll_interval(5ms);
        service.set_debounce_samples(2);
        service.set_rescan_cooldown(200ms);
        service.set_shutdown_timeout(2000ms);
    }
};

// ---------------------------------------------------------------------------
// 1. Binding grammar
// ---------------------------------------------------------------------------

void check_binding_grammar()
{
    Fixture fixture;
    check(fixture.service.set_event_sink(fixture.edges.sink()).is_ok(), "the gamepad sink is installed");

    // An absent binding is "no gamepad action": a success, because a user may
    // bind only hotkeys.
    AppSettings absent;
    check(fixture.service.apply_settings(absent).is_ok(),
        "an absent gamepad binding is a success, not an error");
    check(fixture.service.apply_settings(absent).is_ok(),
        "re-applying an absent binding is idempotent");
    check(fixture.service.polling(), "an absent binding still starts the poll thread");

    // An empty string is the same unassigned state, not a malformed binding.
    AppSettings empty;
    empty.record_gamepad_button = std::string();
    empty.cancel_gamepad_button = std::string();
    check(fixture.service.apply_settings(empty).is_ok(), "an empty gamepad binding is a success");

    // A present but malformed binding is a reported failure.
    AppSettings unknown_button;
    unknown_button.record_gamepad_button = std::string("XInput|NotAButton");
    const auto bad_button = fixture.service.apply_settings(unknown_button);
    check(bad_button.is_error() && bad_button.code() == domain::ErrorCode::invalid_argument,
        "a present but unknown XInput button is invalid_argument");

    AppSettings bad_source;
    bad_source.record_gamepad_button = std::string("Gamepad|A");
    const auto bad_source_result = fixture.service.apply_settings(bad_source);
    check(bad_source_result.is_error() && bad_source_result.code() == domain::ErrorCode::invalid_argument,
        "an unknown binding source is invalid_argument");

    AppSettings bad_dinput;
    bad_dinput.cancel_gamepad_button = std::string("DInput|Logitech");
    const auto bad_dinput_result = fixture.service.apply_settings(bad_dinput);
    check(bad_dinput_result.is_error() && bad_dinput_result.code() == domain::ErrorCode::invalid_argument,
        "a DInput binding without its index is invalid_argument");

    // DInput parses but is never matched here: the backend enumerates evdev
    // nodes only, exactly like the Windows backend does not enumerate
    // DirectInput. The gap is deliberate and documented, not a silent "no
    // controller".
    AppSettings dinput;
    dinput.record_gamepad_button = std::string("DInput|Fake Pad|0");
    check(fixture.service.apply_settings(dinput).is_ok(), "a well-formed DInput binding is accepted");

    fixture.pads.set_script({bus(key_pad({BTN_SOUTH}))});
    std::this_thread::sleep_for(120ms);
    check(fixture.edges.count(GamepadAction::record_pressed) == 0,
        "a DInput binding never matches an evdev pad (documented parity gap)");

    check(fixture.service.stop().is_ok(), "the service stops after the grammar checks");
}

// ---------------------------------------------------------------------------
// 2. XInputPadButton -> evdev table, including the analog triggers
// ---------------------------------------------------------------------------

void check_evdev_mapping()
{
    Fixture fixture;
    fixture.pads.set_script({bus(idle_pad())});

    AppSettings settings;
    settings.record_gamepad_button = std::string("XInput|A");
    check(fixture.service.apply_settings(settings).is_ok(), "the service starts for the mapping table");

    struct Mapping {
        EvdevGamepadSnapshot pad;
        const char* name;
    };

    const std::vector<Mapping> table{
        {key_pad({BTN_SOUTH}), "A"},
        {key_pad({BTN_EAST}), "B"},
        {key_pad({BTN_WEST}), "X"},
        {key_pad({BTN_NORTH}), "Y"},
        {key_pad({BTN_TL}), "LB"},
        {key_pad({BTN_TR}), "RB"},
        {axis_pad({{ABS_Z, 255}}), "LT"},
        {axis_pad({{ABS_RZ, 255}}), "RT"},
        {axis_pad({{ABS_HAT0Y, -1}}), "DPadUp"},
        {axis_pad({{ABS_HAT0Y, 1}}), "DPadDown"},
        {axis_pad({{ABS_HAT0X, -1}}), "DPadLeft"},
        {axis_pad({{ABS_HAT0X, 1}}), "DPadRight"},
        {key_pad({BTN_START}), "Start"},
        {key_pad({BTN_SELECT}), "Back"},
        {key_pad({BTN_THUMBL}), "LeftStick"},
        {key_pad({BTN_THUMBR}), "RightStick"},
    };

    for (const auto& entry : table) {
        const int before = fixture.pads.polls();
        fixture.pads.set_script({bus(entry.pad)});
        settle(fixture.pads, before);
        const auto captured = fixture.service.capture_next({});
        const std::string expected = std::string("XInput|") + entry.name;
        check(captured.is_ok() && captured.value().to_string() == expected,
            "evdev code maps to " + expected + " (got "
                + (captured.is_ok() ? captured.value().to_string() : std::string("<error>")) + ")");
    }

    // The analog triggers use the .NET threshold of 30 out of 255, and a device
    // that reports the signed -32768..32767 range is normalised to 0..255.
    int before = fixture.pads.polls();
    fixture.pads.set_script({bus(axis_pad({{ABS_Z, 29}}))});
    settle(fixture.pads, before);
    check(capture_with_timeout(fixture.service, 150ms) == domain::ErrorCode::cancelled,
        "a trigger at 29/255 is below the press threshold (the capture is cancelled, not answered)");

    before = fixture.pads.polls();
    fixture.pads.set_script({bus(axis_pad({{ABS_Z, 30}}))});
    settle(fixture.pads, before);
    const auto at_threshold = fixture.service.capture_next({});
    check(at_threshold.is_ok() && at_threshold.value().to_string() == "XInput|LT",
        "a trigger at exactly 30/255 is a press");

    before = fixture.pads.polls();
    fixture.pads.set_script({bus(axis_pad({{ABS_RZ, 32767}}))});
    settle(fixture.pads, before);
    const auto signed_max = fixture.service.capture_next({});
    check(signed_max.is_ok() && signed_max.value().to_string() == "XInput|RT",
        "a signed-range trigger at +32767 normalises to a press");

    before = fixture.pads.polls();
    fixture.pads.set_script({bus(axis_pad({{ABS_Z, -32768}}))});
    settle(fixture.pads, before);
    check(capture_with_timeout(fixture.service, 150ms) == domain::ErrorCode::cancelled,
        "a signed-range trigger at -32768 normalises to a resting trigger");

    check(fixture.service.stop().is_ok(), "the service stops after the mapping table");
}

// ---------------------------------------------------------------------------
// 3. Debounced edges
// ---------------------------------------------------------------------------

void check_edges_and_debounce()
{
    Fixture fixture;
    check(fixture.service.set_event_sink(fixture.edges.sink()).is_ok(), "the edge sink is installed");

    AppSettings settings;
    settings.record_gamepad_button = std::string("XInput|A");
    settings.cancel_gamepad_button = std::string("XInput|B");

    // A single 5 ms sample must not produce an edge with a debounce of 2.
    fixture.pads.set_script({bus(key_pad({BTN_SOUTH})), bus(idle_pad())});
    check(fixture.service.apply_settings(settings).is_ok(), "apply_settings starts polling");
    check(fixture.service.polling(), "the poll thread is running");
    std::this_thread::sleep_for(150ms);
    check(fixture.edges.count(GamepadAction::record_pressed) == 0,
        "a one-sample press produces no edge with a debounce of 2");
    check(fixture.edges.count(GamepadAction::record_released) == 0,
        "a one-sample press produces no release edge either");

    // Two consecutive samples are a press; two consecutive idle samples are the
    // release. The script clamps on its last step.
    fixture.pads.set_script({bus(key_pad({BTN_SOUTH})), bus(key_pad({BTN_SOUTH})), bus(idle_pad()), bus(idle_pad())});
    const bool pressed = wait_for(
        [&fixture]() { return fixture.edges.count(GamepadAction::record_pressed) == 1; }, 2000ms);
    check(pressed, "a held press produces exactly one debounced press edge");
    const bool released = wait_for(
        [&fixture]() { return fixture.edges.count(GamepadAction::record_released) == 1; }, 2000ms);
    check(released, "the release produces exactly one debounced release edge");
    {
        const auto sequence = fixture.edges.snapshot();
        check(sequence.size() == 2 && sequence[0] == GamepadAction::record_pressed
                && sequence[1] == GamepadAction::record_released,
            "the edge order is press then release");
    }

    // The cancel binding is a separate edge; it reports only on press.
    fixture.pads.set_script({bus(key_pad({BTN_EAST})), bus(key_pad({BTN_EAST})), bus(idle_pad()), bus(idle_pad())});
    const bool cancelled = wait_for(
        [&fixture]() { return fixture.edges.count(GamepadAction::cancel_pressed) == 1; }, 2000ms);
    check(cancelled, "the cancel binding produces a cancel_pressed edge");
    std::this_thread::sleep_for(60ms);
    check(fixture.edges.count(GamepadAction::cancel_pressed) == 1,
        "the cancel edge fires once per hold, not on release");

    // Re-binding resets the edge state, so a button held across the change
    // cannot emit a release for a gesture that is no longer bound.
    AppSettings rebound;
    rebound.record_gamepad_button = std::string("XInput|X");
    fixture.pads.set_script({bus(key_pad({BTN_SOUTH}))});
    check(fixture.service.apply_settings(rebound).is_ok(), "re-binding succeeds");
    std::this_thread::sleep_for(120ms);
    check(fixture.edges.count(GamepadAction::record_released) == 1,
        "re-binding resets the debounced edge state");

    // A throwing sink must not escape into the poll thread nor stop the service.
    check(fixture.service.set_event_sink([](const GamepadEdge&) { throw std::runtime_error("sink"); }).is_ok(),
        "a throwing sink can be installed");
    fixture.pads.set_script({bus(key_pad({BTN_WEST})), bus(key_pad({BTN_WEST}))});
    std::this_thread::sleep_for(150ms);
    check(fixture.service.polling(), "a throwing sink does not kill the poll thread");

    check(fixture.service.stop().is_ok(), "the service stops after the edge checks");
}

// ---------------------------------------------------------------------------
// 4. Capture and its cancellation
// ---------------------------------------------------------------------------

void check_capture()
{
    Fixture fixture;
    fixture.pads.set_script({});

    // A capture before apply_settings is a reported invalid_state, not a hang.
    const auto not_started = fixture.service.capture_next({});
    check(not_started.is_error() && not_started.code() == domain::ErrorCode::invalid_state,
        "a capture before apply_settings reports invalid_state");

    AppSettings settings;
    settings.record_gamepad_button = std::string("XInput|A");
    check(fixture.service.apply_settings(settings).is_ok(), "the service starts for the capture checks");

    // No controller at all: the capture answers instead of waiting for a press
    // that can never come.
    const auto begin = std::chrono::steady_clock::now();
    const auto no_device = fixture.service.capture_next({});
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    check(no_device.is_error() && no_device.code() == domain::ErrorCode::device_disconnected,
        "a capture with no controller reports device_disconnected");
    check(elapsed < 1000ms, "that answer is immediate, not a wait");

    // The first pressed button of a poll is returned, in the Windows order
    // (triggers first, then the fixed button table).
    int before = fixture.pads.polls();
    fixture.pads.set_script({bus(key_pad({BTN_SOUTH, BTN_EAST}))});
    settle(fixture.pads, before);
    const auto first = fixture.service.capture_next({});
    check(first.is_ok() && first.value().to_string() == "XInput|A",
        "the capture returns the first button of the fixed table");

    before = fixture.pads.polls();
    fixture.pads.set_script({bus(axis_pad({{ABS_Z, 255}, {ABS_HAT0X, 1}}))});
    settle(fixture.pads, before);
    const auto trigger_first = fixture.service.capture_next({});
    check(trigger_first.is_ok() && trigger_first.value().to_string() == "XInput|LT",
        "an analog trigger is observed before the digital buttons, as on Windows");

    // A capture can be cancelled from another thread and returns promptly.
    before = fixture.pads.polls();
    fixture.pads.set_script({bus(idle_pad())});
    settle(fixture.pads, before);
    domain::CancellationSource source;
    std::thread canceller([&source] {
        std::this_thread::sleep_for(120ms);
        source.request_cancellation();
    });
    const auto cancel_begin = std::chrono::steady_clock::now();
    const auto cancelled = fixture.service.capture_next(source.token());
    const auto cancel_elapsed = std::chrono::steady_clock::now() - cancel_begin;
    canceller.join();
    check(cancelled.is_error() && cancelled.code() == domain::ErrorCode::cancelled,
        "a capture cancelled through the token returns cancelled");
    check(cancel_elapsed < 1000ms, "the cancelled capture ends promptly");

    // cancel_capture() ends a waiting capture from another thread and is
    // idempotent.
    std::atomic<domain::ErrorCode> first_code{domain::ErrorCode::ok};
    std::thread waiter([&fixture, &first_code] {
        const auto result = fixture.service.capture_next({});
        first_code.store(result.is_ok() ? domain::ErrorCode::ok : result.code());
    });
    std::this_thread::sleep_for(80ms);
    check(fixture.service.cancel_capture().is_ok(), "cancel_capture succeeds");
    waiter.join();
    check(first_code.load() == domain::ErrorCode::cancelled,
        "the cancelled capture returns cancelled, not a hang");
    check(fixture.service.cancel_capture().is_ok(), "cancel_capture is idempotent");

    // A second concurrent capture is refused rather than orphaning the first
    // waiter (the .NET StartCapture overwrote its completion source).
    before = fixture.pads.polls();
    fixture.pads.set_script({bus(idle_pad())});
    settle(fixture.pads, before);
    std::thread first_waiter([&fixture] { static_cast<void>(fixture.service.capture_next({})); });
    std::this_thread::sleep_for(80ms);
    const auto second = fixture.service.capture_next({});
    check(second.is_error() && second.code() == domain::ErrorCode::invalid_state,
        "a second concurrent capture is refused, so the first waiter is not orphaned");
    check(fixture.service.cancel_capture().is_ok(), "the first waiter is then cancelled");
    first_waiter.join();

    check(fixture.service.stop().is_ok(), "the service stops after the capture checks");
}

// ---------------------------------------------------------------------------
// 5. Device list and device loss
// ---------------------------------------------------------------------------

void check_devices_and_loss()
{
    Fixture fixture;
    fixture.pads.set_script({});

    AppSettings settings;
    settings.record_gamepad_button = std::string("XInput|A");
    check(fixture.service.apply_settings(settings).is_ok(), "apply_settings with no controller succeeds");

    std::this_thread::sleep_for(120ms);
    check(fixture.service.devices().empty(), "no controller means an empty device list, not an error");
    check(fixture.service.polling(), "the service polls with no controller attached");
    check(fixture.service.rescan_count() >= 1, "the empty bus was enumerated at least once");
    const int empty_rescans = fixture.service.rescan_count();
    std::this_thread::sleep_for(300ms);
    check(fixture.service.rescan_count() <= empty_rescans + 1,
        "an absent controller is not re-enumerated per poll (count "
            + std::to_string(fixture.service.rescan_count()) + ")");

    // A controller appears.
    fixture.pads.set_script({bus(idle_pad())});
    const bool appeared = wait_for([&fixture]() { return fixture.service.devices().size() == 1; }, 2000ms);
    check(appeared, "a connected controller appears in devices()");
    if (appeared) {
        const auto devices = fixture.service.devices();
        check(devices[0].source == GamepadSource::xinput, "the device names the xinput source");
        check(devices[0].connected, "the device is reported as connected");
        check(devices[0].index == 0, "the device reports its evdev node index");
        check(devices[0].product_name == "Fake Pad", "the device reports its product name");
    }

    // The controller goes away: no crash, no stop, no unbounded re-enumeration.
    fixture.pads.set_script({});
    const bool vanished = wait_for([&fixture]() { return fixture.service.devices().empty(); }, 2000ms);
    check(vanished, "a disconnected controller leaves devices()");
    check(fixture.service.polling(), "device loss does not stop the service");
    std::this_thread::sleep_for(300ms);
    check(fixture.service.rescan_count() <= empty_rescans + 8,
        "re-enumeration stays bounded across a hot-plug cycle (count "
            + std::to_string(fixture.service.rescan_count()) + ")");

    check(fixture.service.stop().is_ok(), "the service stops after the device checks");
}

// ---------------------------------------------------------------------------
// 6. stop() and the production reader
// ---------------------------------------------------------------------------

void check_stop()
{
    Fixture fixture;
    fixture.pads.set_script({bus(idle_pad())});

    AppSettings settings;
    settings.record_gamepad_button = std::string("XInput|A");
    check(fixture.service.apply_settings(settings).is_ok(), "the service starts before the stop checks");

    const auto stop_begin = std::chrono::steady_clock::now();
    const auto stopped = fixture.service.stop();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_begin;
    check(stopped.is_ok(), "stop() joins the poll thread");
    check(stop_elapsed < 2000ms, "the join finished inside the 2 s deadline");
    check(!fixture.service.polling(), "the poll thread is gone after stop()");
    check(fixture.service.stop().is_ok(), "stop() is idempotent");
    const int after_stop = fixture.pads.polls();
    std::this_thread::sleep_for(100ms);
    check(fixture.pads.polls() == after_stop, "a stopped service does not keep polling");
}

/// The production evdev reader with whatever this machine has (usually no
/// gamepad). It proves the real path never hangs or crashes; it makes no claim
/// about hardware.
void check_real_reader()
{
    const auto reader = platform::linuxos::evdev_gamepad_reader();
    const auto bus_state = reader();
    std::cout << "note: the production evdev reader reports " << bus_state.size() << " gamepad node(s)\n";
    for (const auto& pad : bus_state) {
        check(pad.node_index >= 0 && pad.connected, "a reported node is connected and names its index");
    }

    LinuxGamepadService service(reader);
    service.set_poll_interval(std::chrono::milliseconds(platform::kGamepadPollIntervalMs));
    AppSettings settings;
    settings.record_gamepad_button = std::string("XInput|A");
    check(service.apply_settings(settings).is_ok(), "the production reader starts polling");
    std::this_thread::sleep_for(120ms);
    const auto devices = service.devices();
    std::cout << "note: devices() reports " << devices.size() << " controller(s) after 120 ms\n";
    for (const auto& device : devices) {
        check(device.source == GamepadSource::xinput && device.connected,
            "a reported device names its source and is connected");
    }
    const auto stop_begin = std::chrono::steady_clock::now();
    check(service.stop().is_ok(), "the production reader stops");
    check(std::chrono::steady_clock::now() - stop_begin < 2000ms, "that stop is prompt");
}

/// What makes a node a gamepad. A virtual keyboard/mouse (ydotoold's device)
/// reports the face-button codes but has no EV_ABS at all, so it must not be
/// listed as a controller - measured live 2026-10-11, when it showed up as a
/// third "gamepad" beside the two real ones.
void check_node_filter()
{
    using namespace voicetyper::platform::linuxos;

    check(evdev_node_is_gamepad(true, true, true),
        "a node with key events, absolute axes and face buttons is a gamepad");
    check(!evdev_node_is_gamepad(true, false, true),
        "a node without EV_ABS (a virtual keyboard/mouse) is not a gamepad");
    check(!evdev_node_is_gamepad(false, true, true), "a node without EV_KEY is not a gamepad");
    check(!evdev_node_is_gamepad(true, true, false), "a node without a face button is not a gamepad");
}

} // namespace
int main()
{
    check_binding_grammar();
    check_evdev_mapping();
    check_edges_and_debounce();
    check_capture();
    check_devices_and_loss();
    check_stop();
    check_real_reader();
    check_node_filter();

    if (failures != 0) {
        std::cerr << "linux-gamepad-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "linux-gamepad-contract: OK\n";
    return 0;
}
