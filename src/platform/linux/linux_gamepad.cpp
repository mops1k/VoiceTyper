// Linux gamepad input: evdev polling on a dedicated worker thread.
//
// See linux_gamepad.hpp for the contract, the Windows reference semantics, the
// documented XInput-only scope and the button table. Everything here below the
// include block is Linux/evdev.

#include "platform/linux/linux_gamepad.hpp"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace voicetyper::platform::linuxos {
namespace {

constexpr int kMaxEventNodes = 32;
constexpr int kMaxDebounceSamples = 16;

/// The contract's device count: one bit per evdev node index in the signature.
static_assert(kMaxEventNodes <= 32, "the connection signature is a 32-bit mask");

/// Bound of the stop() join. Not shared with the hotkey backend on purpose:
/// each backend owns its own constant rather than depending on the other's
/// header.
constexpr int kGamepadDefaultShutdownTimeoutMs = 2000;

/// How often the real reader looks for newly attached nodes. Between rescans it
/// only drains the open file descriptors, so an idle pad costs no syscalls
/// beyond the four reads per poll.
constexpr auto kEvdevRescanInterval = std::chrono::milliseconds(1000);

// --- Button table -----------------------------------------------------------

/// The four axes the XInput mapping reads.
constexpr std::array<std::uint16_t, 4> kTrackedAxes{ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};

/// True when the pad holds a raw EV_KEY code.
bool has_key(const EvdevGamepadSnapshot& pad, std::uint16_t code)
{
    return std::find(pad.pressed_keys.begin(), pad.pressed_keys.end(), code) != pad.pressed_keys.end();
}

/// The raw value of one tracked axis, 0 when the node does not report it.
std::int32_t axis_value(const EvdevGamepadSnapshot& pad, std::uint16_t code)
{
    for (const auto& axis : pad.axes) {
        if (axis.code == code) {
            return axis.value;
        }
    }
    return 0;
}

/// Normalises an evdev ABS_Z/ABS_RZ trigger to the 0..255 scale the .NET
/// threshold (kGamepadTriggerPressThreshold) was measured on.
///
/// evdev drivers disagree about the range: xpad reports 0..255, while a pad
/// driven through a DirectInput-style driver reports -32768..32767. A value in
/// 0..255 is already normalised; a negative value, and a positive value above
/// 255, can only come from the signed range, which is shifted to 0..65535 and
/// scaled down. The only ambiguous point is a small positive value on a signed
/// device (0..255 raw would read as a light press): it is still far below the
/// threshold only for values under 30, which is exactly where a real press
/// starts on both ranges.
int normalise_trigger(std::int32_t value)
{
    if (value >= 0 && value <= 255) {
        return static_cast<int>(value);
    }
    const std::int64_t shifted = static_cast<std::int64_t>(value) + 32768;
    const std::int64_t clamped = std::clamp<std::int64_t>(shifted, 0, 65535);
    return static_cast<int>(clamped * 255 / 65535);
}

/// True when one XInputPadButton is pressed on this evdev snapshot. This is the
/// XInputPadButton -> evdev table of linux_gamepad.hpp, in one place.
bool button_pressed(const EvdevGamepadSnapshot& pad, domain::XInputPadButton button)
{
    switch (button) {
    case domain::XInputPadButton::a: return has_key(pad, BTN_SOUTH); // == BTN_GAMEPAD
    case domain::XInputPadButton::b: return has_key(pad, BTN_EAST);
    case domain::XInputPadButton::x: return has_key(pad, BTN_WEST);
    case domain::XInputPadButton::y: return has_key(pad, BTN_NORTH);
    case domain::XInputPadButton::lb: return has_key(pad, BTN_TL);
    case domain::XInputPadButton::rb: return has_key(pad, BTN_TR);
    case domain::XInputPadButton::lt:
        return normalise_trigger(axis_value(pad, ABS_Z)) >= kGamepadTriggerPressThreshold;
    case domain::XInputPadButton::rt:
        return normalise_trigger(axis_value(pad, ABS_RZ)) >= kGamepadTriggerPressThreshold;
    case domain::XInputPadButton::dpad_up: return axis_value(pad, ABS_HAT0Y) < 0;
    case domain::XInputPadButton::dpad_down: return axis_value(pad, ABS_HAT0Y) > 0;
    case domain::XInputPadButton::dpad_left: return axis_value(pad, ABS_HAT0X) < 0;
    case domain::XInputPadButton::dpad_right: return axis_value(pad, ABS_HAT0X) > 0;
    case domain::XInputPadButton::start: return has_key(pad, BTN_START);
    case domain::XInputPadButton::back: return has_key(pad, BTN_SELECT);
    case domain::XInputPadButton::left_stick: return has_key(pad, BTN_THUMBL);
    case domain::XInputPadButton::right_stick: return has_key(pad, BTN_THUMBR);
    }
    return false;
}

/// The observation order of the Windows backend: analog triggers first, then the
/// fixed button table (A, B, X, Y, LB, RB, DPadUp, DPadDown, DPadLeft, DPadRight,
/// Start, Back, LeftStick, RightStick). capture_next() returns the *first*
/// pressed button of a poll, so the order is observable and kept identical.
constexpr std::array<domain::XInputPadButton, 16> kObservationOrder{{
    domain::XInputPadButton::lt,
    domain::XInputPadButton::rt,
    domain::XInputPadButton::a,
    domain::XInputPadButton::b,
    domain::XInputPadButton::x,
    domain::XInputPadButton::y,
    domain::XInputPadButton::lb,
    domain::XInputPadButton::rb,
    domain::XInputPadButton::dpad_up,
    domain::XInputPadButton::dpad_down,
    domain::XInputPadButton::dpad_left,
    domain::XInputPadButton::dpad_right,
    domain::XInputPadButton::start,
    domain::XInputPadButton::back,
    domain::XInputPadButton::left_stick,
    domain::XInputPadButton::right_stick,
}};

/// The buttons pressed on one snapshot, in the observation order above.
std::vector<domain::GamepadInput> pressed_inputs(const EvdevGamepadSnapshot& pad)
{
    std::vector<domain::GamepadInput> inputs;
    if (!pad.connected) {
        return inputs;
    }
    for (const auto button : kObservationOrder) {
        if (button_pressed(pad, button)) {
            inputs.push_back(domain::GamepadInput{
                domain::GamepadSource::xinput,
                std::string(domain::x_input_button_name(button).value())});
        }
    }
    return inputs;
}

/// The case-insensitive comparison of GamepadBindingMatcher.cs: same source and
/// the same button name, ignoring case. A DirectInput binding never matches here
/// because this backend does not enumerate DirectInput devices.
bool matches(const domain::GamepadBinding& binding, const domain::GamepadInput& input)
{
    if (!binding.is_assigned() || binding.source != input.source) {
        return false;
    }
    return domain::detail::equals_ignore_ascii_case(binding.button_id, input.button_id);
}

std::vector<GamepadDevice> devices_from(const std::vector<EvdevGamepadSnapshot>& pads)
{
    std::vector<GamepadDevice> devices;
    for (const auto& pad : pads) {
        if (!pad.connected) {
            continue;
        }
        GamepadDevice device;
        device.source = domain::GamepadSource::xinput;
        device.index = pad.node_index;
        device.product_name = pad.product_name;
        device.connected = true;
        devices.push_back(std::move(device));
    }
    return devices;
}

std::uint32_t connection_signature(const std::vector<EvdevGamepadSnapshot>& pads)
{
    std::uint32_t signature = 0;
    for (const auto& pad : pads) {
        if (pad.connected && pad.node_index >= 0 && pad.node_index < kMaxEventNodes) {
            signature |= 1u << pad.node_index;
        }
    }
    return signature;
}

std::string devices_summary(const std::vector<GamepadDevice>& devices)
{
    if (devices.empty()) {
        return "no gamepad input device under /dev/input";
    }
    std::string summary = "gamepads: " + std::to_string(devices.size());
    for (const auto& device : devices) {
        summary += ", event" + std::to_string(device.index);
        if (!device.product_name.empty()) {
            summary += " (" + device.product_name + ")";
        }
    }
    return summary;
}

// --- The real evdev reader --------------------------------------------------

/// True when the node looks like a gamepad: it reports key events, absolute axes
/// and at least one of the face buttons. A keyboard, a mouse, a power button and
/// a lid switch all report EV_KEY but none of them has BTN_SOUTH/BTN_EAST/NORTH/WEST;
/// ydotoold's virtual device reports the button codes but no EV_ABS at all, so the
/// axis bit is what keeps it out of the controller list (measured 2026-10-11).
/// BTN_SOUTH and BTN_GAMEPAD are the same code (0x130).
bool is_gamepad(int descriptor)
{
    unsigned long event_bits = 0;
    if (::ioctl(descriptor, EVIOCGBIT(0, sizeof(event_bits)), &event_bits) < 0) {
        return false;
    }
    const bool has_key_events = (event_bits & (1UL << EV_KEY)) != 0;
    const bool has_abs_events = (event_bits & (1UL << EV_ABS)) != 0;
    std::vector<unsigned long> key_bits((KEY_MAX / (8 * sizeof(unsigned long))) + 1, 0);
    if (::ioctl(descriptor, EVIOCGBIT(EV_KEY, key_bits.size() * sizeof(unsigned long)), key_bits.data()) < 0) {
        return false;
    }
    const auto bit_set = [&key_bits](int code) {
        const auto index = static_cast<std::size_t>(code / (8 * sizeof(unsigned long)));
        const auto bit = static_cast<unsigned long>(code % (8 * sizeof(unsigned long)));
        return index < key_bits.size() && (key_bits[index] & (1UL << bit)) != 0;
    };
    const bool has_face_button = bit_set(BTN_SOUTH) || bit_set(BTN_GAMEPAD) || bit_set(BTN_EAST)
        || bit_set(BTN_NORTH) || bit_set(BTN_WEST);
    return evdev_node_is_gamepad(has_key_events, has_abs_events, has_face_button);
}

std::string read_product_name(int descriptor)
{
    char name[256] = {};
    if (::ioctl(descriptor, EVIOCGNAME(sizeof(name)), name) < 0) {
        return {};
    }
    return std::string(name);
}

/// One open evdev gamepad node and its current state.
struct EvdevNode {
    int descriptor = -1;
    int index = -1;
    std::string product_name;
    std::vector<std::uint16_t> pressed_keys;
    std::array<std::int32_t, kTrackedAxes.size()> axes{};

    ~EvdevNode()
    {
        if (descriptor >= 0) {
            ::close(descriptor);
        }
    }

    EvdevNode() = default;
    EvdevNode(const EvdevNode&) = delete;
    EvdevNode& operator=(const EvdevNode&) = delete;
    EvdevNode(EvdevNode&& other) noexcept
        : descriptor(std::exchange(other.descriptor, -1))
        , index(other.index)
        , product_name(std::move(other.product_name))
        , pressed_keys(std::move(other.pressed_keys))
        , axes(other.axes)
    {
    }

    EvdevNode& operator=(EvdevNode&& other) noexcept
    {
        if (this != &other) {
            if (descriptor >= 0) {
                ::close(descriptor);
            }
            descriptor = std::exchange(other.descriptor, -1);
            index = other.index;
            product_name = std::move(other.product_name);
            pressed_keys = std::move(other.pressed_keys);
            axes = other.axes;
        }
        return *this;
    }

    /// Applies one EV_KEY event to the held-button set.
    void set_key(std::uint16_t code, int value)
    {
        const auto found = std::find(pressed_keys.begin(), pressed_keys.end(), code);
        if (value == 0) {
            if (found != pressed_keys.end()) {
                pressed_keys.erase(found);
            }
            return;
        }
        // 1 is press, 2 is autorepeat: both mean "held".
        if (found == pressed_keys.end()) {
            pressed_keys.push_back(code);
        }
    }

    /// Applies one EV_ABS event for an axis the mapping reads.
    void set_axis(std::uint16_t code, std::int32_t value)
    {
        for (std::size_t i = 0; i < kTrackedAxes.size(); ++i) {
            if (kTrackedAxes[i] == code) {
                axes[i] = value;
                return;
            }
        }
    }

    /// Reads every pending event. Returns false when the node is gone (ENODEV)
    /// or unreadable, which is how a disconnected pad is detected.
    bool drain()
    {
        input_event event{};
        for (;;) {
            const ssize_t bytes = ::read(descriptor, &event, sizeof(event));
            if (bytes == static_cast<ssize_t>(sizeof(event))) {
                if (event.type == EV_KEY) {
                    set_key(static_cast<std::uint16_t>(event.code), event.value);
                } else if (event.type == EV_ABS) {
                    set_axis(static_cast<std::uint16_t>(event.code), event.value);
                }
                continue;
            }
            if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            }
            if (bytes < 0 && errno == EINTR) {
                continue;
            }
            // ENODEV (the pad was unplugged) or any other read error: the node
            // cannot be trusted any more, so it is dropped instead of being
            // reported as a held button forever.
            return false;
        }
    }

    EvdevGamepadSnapshot snapshot() const
    {
        EvdevGamepadSnapshot out;
        out.connected = true;
        out.node_index = index;
        out.product_name = product_name;
        out.pressed_keys = pressed_keys;
        out.axes.reserve(kTrackedAxes.size());
        for (std::size_t i = 0; i < kTrackedAxes.size(); ++i) {
            out.axes.push_back(EvdevAxisValue{kTrackedAxes[i], axes[i]});
        }
        return out;
    }
};

/// The stateful half of the production reader: the open nodes, their event
/// draining and a rate-limited rescan for newly attached pads.
class EvdevBus {
public:
    std::vector<EvdevGamepadSnapshot> poll()
    {
        const auto now = std::chrono::steady_clock::now();
        if (!scanned_ || now >= next_rescan_) {
            rescan();
            next_rescan_ = now + kEvdevRescanInterval;
            scanned_ = true;
        }

        std::vector<EvdevGamepadSnapshot> snapshots;
        snapshots.reserve(nodes_.size());
        for (auto it = nodes_.begin(); it != nodes_.end();) {
            if (!it->drain()) {
                it = nodes_.erase(it);
                continue;
            }
            snapshots.push_back(it->snapshot());
            ++it;
        }
        return snapshots;
    }

private:
    void rescan()
    {
        for (int index = 0; index < kMaxEventNodes; ++index) {
            const bool known = std::any_of(nodes_.begin(), nodes_.end(),
                [index](const EvdevNode& node) { return node.index == index; });
            if (known) {
                continue;
            }
            const std::string path = "/dev/input/event" + std::to_string(index);
            const int descriptor = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (descriptor < 0) {
                // EACCES/EPERM is the state a user who is not in the `input`
                // group and has no udev ACL is in; it is not an error by itself,
                // the pad simply is not visible.
                continue;
            }
            if (!is_gamepad(descriptor)) {
                ::close(descriptor);
                continue;
            }
            EvdevNode node;
            node.descriptor = descriptor;
            node.index = index;
            node.product_name = read_product_name(descriptor);
            nodes_.push_back(std::move(node));
        }
    }

    std::vector<EvdevNode> nodes_;
    std::chrono::steady_clock::time_point next_rescan_{};
    bool scanned_ = false;
};

} // namespace

bool evdev_node_is_gamepad(bool has_key_events, bool has_abs_events, bool has_face_buttons)
{
    // All three are required: a keyboard reports EV_KEY, a touchpad reports
    // EV_ABS, and only a pad reports both plus a face button. ydotoold's virtual
    // device is the live counter-example (2026-10-11).
    return has_key_events && has_abs_events && has_face_buttons;
}

GamepadDeviceReader evdev_gamepad_reader(){
    // One bus per reader. The service owns the reader for its whole lifetime, so
    // the nodes stay open across polls; a fake reader ignores this entirely.
    auto bus = std::make_shared<EvdevBus>();
    return [bus]() { return bus->poll(); };
}

// ---------------------------------------------------------------------------
// LinuxGamepadService
// ---------------------------------------------------------------------------

struct LinuxGamepadService::Impl {
    /// One dedicated, mutex-protected input state: the poll thread is the only
    /// writer and every reader copies a snapshot under `mutex`.
    struct PolledInput {
        std::vector<EvdevGamepadSnapshot> pads;
        std::vector<GamepadDevice> devices;
        std::string diagnostics;
        std::uint32_t signature = 0;
        bool signature_known = false;

        domain::GamepadBinding record_binding;
        domain::GamepadBinding cancel_binding;

        /// Debounced edge state.
        bool record_down = false;
        bool cancel_down = false;
        bool record_last = false;
        bool cancel_last = false;
        int record_streak = 0;
        int cancel_streak = 0;

        /// Capture wait, at most one waiter.
        bool capture_active = false;
        bool capture_completed = false;
        bool capture_has_result = false;
        domain::GamepadBinding capture_binding;
        Error capture_failure;

        int rescan_count = 0;
        std::chrono::steady_clock::time_point next_rescan{};
    };

    mutable std::mutex mutex;
    std::condition_variable settled;
    PolledInput state;

    mutable std::mutex sink_mutex;
    GamepadEventSink sink;

    GamepadDeviceReader reader;

    /// Sleep/wake for the poll loop, separate from `mutex` so stop() can wake an
    /// idle poll without touching the input state.
    std::mutex loop_mutex;
    std::condition_variable loop_wait;
    std::atomic<bool> stopping{false};

    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> started{false};
    std::atomic<int> debounce_samples{2};
    std::atomic<int> rescan_cooldown_ms{1000};
    std::atomic<int> poll_interval_ms{static_cast<int>(kGamepadPollIntervalMs)};
    std::atomic<int> shutdown_timeout_ms{kGamepadDefaultShutdownTimeoutMs};

    explicit Impl(GamepadDeviceReader device_reader)
        : reader(device_reader ? std::move(device_reader) : evdev_gamepad_reader())
    {
    }

    ~Impl()
    {
        stop();
    }

    void emit(const std::vector<GamepadEdge>& edges) noexcept
    {
        if (edges.empty()) {
            return;
        }
        GamepadEventSink copy;
        {
            const std::lock_guard<std::mutex> lock(sink_mutex);
            copy = sink;
        }
        if (!copy) {
            return;
        }
        for (const auto& edge : edges) {
            try {
                copy(edge);
            } catch (...) {
                // Exceptions cannot escape the sink nor cross the platform
                // boundary.
            }
        }
    }

    int clamp_debounce(int samples) const noexcept
    {
        return std::clamp(samples, 1, kMaxDebounceSamples);
    }

    /// One poll iteration. Edges are collected under the lock and delivered
    /// after it is released, so a sink that queries the service cannot deadlock.
    void poll_once()
    {
        std::vector<EvdevGamepadSnapshot> pads = reader();

        const std::uint32_t signature = connection_signature(pads);
        const int debounce = clamp_debounce(debounce_samples.load());
        const auto cooldown = std::chrono::milliseconds(rescan_cooldown_ms.load());
        std::vector<GamepadEdge> edges;
        bool capture_done = false;

        {
            const std::lock_guard<std::mutex> lock(mutex);
            state.pads = pads;

            // --- Hot-plug / device loss, rate limited. A pad that keeps failing
            // to appear must not be able to spin this loop: the signature change
            // is only acted on once per cooldown, so the re-enumeration count is
            // bounded by elapsed time, not by failures.
            const bool changed = !state.signature_known || signature != state.signature;
            if (changed) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= state.next_rescan) {
                    state.next_rescan = now + cooldown;
                    ++state.rescan_count;
                    state.signature = signature;
                    state.signature_known = true;
                    state.devices = devices_from(pads);
                    state.diagnostics = devices_summary(state.devices);
                }
            }

            // --- Collect this poll's pressed buttons.
            std::vector<domain::GamepadInput> inputs;
            int connected = 0;
            for (const auto& pad : pads) {
                if (!pad.connected) {
                    continue;
                }
                ++connected;
                const auto pressed = pressed_inputs(pad);
                inputs.insert(inputs.end(), pressed.begin(), pressed.end());
            }

            // --- Capture mode ends on the first press, or reports that every pad
            // went away. A second capture_next() is refused, so the previous
            // waiter is never orphaned the way the .NET StartCapture orphaned it.
            if (state.capture_active && !state.capture_completed) {
                if (!inputs.empty()) {
                    state.capture_completed = true;
                    state.capture_has_result = true;
                    state.capture_binding = domain::GamepadBinding{inputs.front().source, inputs.front().button_id};
                    capture_done = true;
                } else if (connected == 0) {
                    state.capture_completed = true;
                    state.capture_has_result = false;
                    state.capture_failure = Error(
                        ErrorCode::device_disconnected,
                        "every gamepad went away before a button was pressed");
                    capture_done = true;
                }
            }

            // A capture that just consumed this poll short-circuits the edge pass,
            // exactly as the .NET Process() returned straight after completing a
            // capture.
            if (!capture_done) {
                // --- Debounced edges.
                const bool record =
                    std::any_of(inputs.begin(), inputs.end(), [this](const auto& input) {
                        return matches(state.record_binding, input);
                    });
                const bool cancel =
                    std::any_of(inputs.begin(), inputs.end(), [this](const auto& input) {
                        return matches(state.cancel_binding, input);
                    });

                const auto advance = [debounce](bool& last, bool& down, int& streak, bool sample) {
                    if (sample == last) {
                        streak = std::min(streak + 1, debounce);
                    } else {
                        last = sample;
                        streak = 1;
                    }
                    bool edge = false;
                    if (sample && !down && streak >= debounce) {
                        down = true;
                        edge = true;
                    } else if (!sample && down && streak >= debounce) {
                        down = false;
                        edge = true;
                    }
                    return edge;
                };

                const bool record_edge = advance(state.record_last, state.record_down, state.record_streak, record);
                const bool cancel_edge = advance(state.cancel_last, state.cancel_down, state.cancel_streak, cancel);

                const auto edge_input = [&inputs](const domain::GamepadBinding& binding) {
                    domain::GamepadInput input;
                    for (const auto& candidate : inputs) {
                        if (matches(binding, candidate)) {
                            return candidate;
                        }
                    }
                    return input;
                };

                if (record_edge) {
                    GamepadEdge edge;
                    edge.action = state.record_down ? GamepadAction::record_pressed : GamepadAction::record_released;
                    edge.binding = state.record_binding;
                    edge.input = edge_input(state.record_binding);
                    edges.push_back(edge);
                }
                if (cancel_edge && state.cancel_down) {
                    GamepadEdge edge;
                    edge.action = GamepadAction::cancel_pressed;
                    edge.binding = state.cancel_binding;
                    edge.input = edge_input(state.cancel_binding);
                    edges.push_back(edge);
                }
            }
        }

        if (capture_done) {
            settled.notify_all();
        }
        emit(edges);
    }

    void poll_main()
    {
        running.store(true);
        std::unique_lock<std::mutex> lock(loop_mutex);
        while (!stopping.load()) {
            lock.unlock();
            poll_once();
            lock.lock();
            loop_wait.wait_for(lock, std::chrono::milliseconds(poll_interval_ms.load()),
                [this]() { return stopping.load(); });
        }
        running.store(false);
    }

    Status start_polling()
    {
        if (started.load()) {
            return Status::success();
        }
        stopping.store(false);
        try {
            thread = std::thread([this]() { poll_main(); });
        } catch (...) {
            return Status::failure(ErrorCode::resource_exhausted, "the gamepad poll thread could not be started");
        }
        started.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (!running.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!running.load()) {
            stop();
            return Status::failure(ErrorCode::timeout, "the gamepad poll thread did not start within 2000 ms");
        }
        return Status::success();
    }

    Status stop()
    {
        const bool was_started = started.exchange(false);
        stopping.store(true);
        loop_wait.notify_all();
        {
            const std::lock_guard<std::mutex> lock(mutex);
            // An active capture must never be left waiting, even when the
            // service was never started.
            if (state.capture_active && !state.capture_completed) {
                state.capture_active = false;
                state.capture_completed = true;
                state.capture_has_result = false;
            }
        }
        settled.notify_all();

        if (!was_started) {
            // Idempotent: stopping a service that never polled is a success.
            return Status::success();
        }
        if (!thread.joinable()) {
            return Status::success();
        }
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
            return Status::success();
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(shutdown_timeout_ms.load());
        while (running.load()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                thread.detach();
                return Status::failure(
                    ErrorCode::timeout,
                    "the gamepad poll thread did not join within "
                        + std::to_string(shutdown_timeout_ms.load()) + " ms");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        thread.join();
        return Status::success();
    }
};

LinuxGamepadService::LinuxGamepadService(GamepadDeviceReader reader)
    : impl_(std::make_unique<Impl>(std::move(reader)))
{
}

LinuxGamepadService::~LinuxGamepadService()
{
    if (impl_ != nullptr) {
        impl_->stop();
    }
}

Status LinuxGamepadService::set_event_sink(GamepadEventSink sink)
{
    if (impl_ == nullptr) {
        return Status::failure(ErrorCode::unavailable, "gamepad service is not constructed");
    }
    const std::lock_guard<std::mutex> lock(impl_->sink_mutex);
    impl_->sink = std::move(sink);
    return Status::success();
}

Status LinuxGamepadService::apply_settings(const AppSettings& settings)
{
    if (impl_ == nullptr) {
        return Status::failure(ErrorCode::unavailable, "gamepad service is not constructed");
    }

    const auto record = settings.record_gamepad_binding();
    const auto cancel = settings.cancel_gamepad_binding();
    if (record.is_error()) {
        return Status::failure(
            record.error().code(),
            "recordGamepadButton \"" + *settings.record_gamepad_button + "\" is not a valid binding: "
                + record.error().message());
    }
    if (cancel.is_error()) {
        return Status::failure(
            cancel.error().code(),
            "cancelGamepadButton \"" + *settings.cancel_gamepad_button + "\" is not a valid binding: "
                + cancel.error().message());
    }

    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        // A binding change resets the debounced edge state, so re-binding while a
        // button is held cannot produce a release edge for a gesture that is no
        // longer bound.
        impl_->state.record_binding = record.value();
        impl_->state.cancel_binding = cancel.value();
        impl_->state.record_down = false;
        impl_->state.cancel_down = false;
        impl_->state.record_last = false;
        impl_->state.cancel_last = false;
        impl_->state.record_streak = 0;
        impl_->state.cancel_streak = 0;
    }
    return impl_->start_polling();
}

Status LinuxGamepadService::stop()
{
    if (impl_ == nullptr) {
        return Status::success();
    }
    return impl_->stop();
}

Status LinuxGamepadService::cancel_capture()
{
    if (impl_ == nullptr) {
        return Status::success();
    }
    bool was_active = false;
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->state.capture_active && !impl_->state.capture_completed) {
            impl_->state.capture_active = false;
            impl_->state.capture_completed = true;
            impl_->state.capture_has_result = false;
            was_active = true;
        }
    }
    if (was_active) {
        impl_->settled.notify_all();
    }
    return Status::success();
}

Result<domain::GamepadBinding> LinuxGamepadService::capture_next(const CancellationToken& cancellation)
{
    if (impl_ == nullptr) {
        return Result<domain::GamepadBinding>::failure(
            ErrorCode::unavailable, "gamepad service is not constructed");
    }
    if (!impl_->started.load()) {
        return Result<domain::GamepadBinding>::failure(
            ErrorCode::invalid_state, "apply_settings() must start polling before a capture");
    }

    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->state.capture_active && !impl_->state.capture_completed) {
            // The .NET StartCapture overwrote the completion source and left the
            // previous caller waiting forever. One waiter at a time is a
            // deliberate, reported restriction.
            return Result<domain::GamepadBinding>::failure(
                ErrorCode::invalid_state, "a capture is already in progress");
        }
        impl_->state.capture_active = true;
        impl_->state.capture_completed = false;
        impl_->state.capture_has_result = false;
        impl_->state.capture_failure = Error();
    }
    impl_->settled.notify_all();

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(impl_->mutex);
            const bool answered = impl_->settled.wait_for(
                lock, std::chrono::milliseconds(50), [this]() { return impl_->state.capture_completed; });
            if (answered) {
                impl_->state.capture_active = false;
                if (!impl_->state.capture_failure.is_ok()) {
                    return Result<domain::GamepadBinding>(impl_->state.capture_failure);
                }
                if (impl_->state.capture_has_result) {
                    return impl_->state.capture_binding;
                }
                return Result<domain::GamepadBinding>::failure(ErrorCode::cancelled, "capture cancelled");
            }
        }
        if (cancellation.is_cancellation_requested()) {
            const std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->state.capture_active) {
                impl_->state.capture_active = false;
                impl_->state.capture_completed = true;
                impl_->state.capture_has_result = false;
            }
            return Result<domain::GamepadBinding>::failure(ErrorCode::cancelled, "capture cancelled");
        }
    }
}

std::vector<GamepadDevice> LinuxGamepadService::devices() const
{
    if (impl_ == nullptr) {
        return {};
    }
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state.devices;
}

void LinuxGamepadService::set_debounce_samples(int samples)
{
    if (impl_ != nullptr) {
        impl_->debounce_samples.store(impl_->clamp_debounce(samples));
    }
}

void LinuxGamepadService::set_rescan_cooldown(std::chrono::milliseconds cooldown)
{
    if (impl_ != nullptr) {
        impl_->rescan_cooldown_ms.store(cooldown.count() > 0 ? static_cast<int>(cooldown.count()) : 1);
    }
}

void LinuxGamepadService::set_poll_interval(std::chrono::milliseconds interval)
{
    if (impl_ != nullptr) {
        const auto count = interval.count();
        impl_->poll_interval_ms.store(count > 0 ? static_cast<int>(count) : 1);
    }
}

void LinuxGamepadService::set_shutdown_timeout(std::chrono::milliseconds timeout)
{
    if (impl_ != nullptr) {
        impl_->shutdown_timeout_ms.store(timeout.count() > 0 ? static_cast<int>(timeout.count()) : 1);
    }
}

int LinuxGamepadService::rescan_count() const
{
    if (impl_ == nullptr) {
        return 0;
    }
    // Read under the poll mutex: the count is written by the poll thread.
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state.rescan_count;
}

bool LinuxGamepadService::polling() const
{
    return impl_ != nullptr && impl_->started.load() && impl_->running.load();
}

std::string LinuxGamepadService::diagnostics() const
{
    if (impl_ == nullptr) {
        return "gamepad service is not constructed";
    }
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state.diagnostics;
}

} // namespace voicetyper::platform::linuxos
