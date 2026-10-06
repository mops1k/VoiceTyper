// Windows gamepad input: XInput polling on a dedicated worker thread.
//
// See win32_gamepad.hpp for the contract, the .NET evidence, the list of .NET
// defects this file does not reproduce, and the documented XInput-only scope.
// The header is OS-free, so everything here below the include block is Win32.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "platform/windows/win32_gamepad.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace voicetyper::platform::win32 {
namespace {

// --- XInput runtime binding -------------------------------------------------

/// XINPUT_STATE, spelled out so the build does not need <xinput.h>. The layout
/// is identical to the SDK declaration: a DWORD packet number followed by
/// {WORD buttons; BYTE lt; BYTE rt; SHORT lx, ly, rx, ry}, 16 bytes.
struct XInputStateCompat {
    std::uint32_t packet_number;
    std::uint16_t buttons;
    std::uint8_t left_trigger;
    std::uint8_t right_trigger;
    std::int16_t thumb_lx;
    std::int16_t thumb_ly;
    std::int16_t thumb_rx;
    std::int16_t thumb_ry;
};
static_assert(sizeof(XInputStateCompat) == 16, "XINPUT_STATE layout mismatch");

using XInputGetStateFn = DWORD(WINAPI*)(DWORD index, XInputStateCompat* state);

/// One resolved XInput runtime. Loaded once, in the documented order.
///
/// Non-copyable and non-movable on purpose: a module handle paired with a
/// destructor that frees it must never be duplicated, or the same DLL is freed
/// twice.
class XInputRuntime {
public:
    XInputRuntime() = default;
    XInputRuntime(const XInputRuntime&) = delete;
    XInputRuntime& operator=(const XInputRuntime&) = delete;
    XInputRuntime(XInputRuntime&&) = delete;
    XInputRuntime& operator=(XInputRuntime&&) = delete;

    ~XInputRuntime()
    {
        if (module != nullptr) {
            ::FreeLibrary(module);
        }
    }

    HMODULE module = nullptr;
    XInputGetStateFn get_state = nullptr;
    std::string name;
    std::string failure;
};

void load_xinput_runtime(XInputRuntime& runtime)
{
    // xinput1_4 ships with Windows 8+, xinput1_3 with Windows 7/Vista/XP SP3,
    // xinput9_1_0 is the reduced Windows 8+ set. Trying all three is the
    // documented way to support every machine without a redistributable.
    for (const wchar_t* candidate :
         {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        HMODULE module = ::LoadLibraryW(candidate);
        if (module == nullptr) {
            continue;
        }
        auto symbol = reinterpret_cast<XInputGetStateFn>(
            reinterpret_cast<void*>(::GetProcAddress(module, "XInputGetState")));
        if (symbol == nullptr) {
            ::FreeLibrary(module);
            continue;
        }
        runtime.module = module;
        runtime.get_state = symbol;
        runtime.name = std::string(candidate, candidate + std::wcslen(candidate));
        return;
    }
    runtime.failure = "no XInput runtime could be loaded (tried xinput1_4.dll, xinput1_3.dll, "
                      "xinput9_1_0.dll); the last Win32 error was "
        + std::to_string(::GetLastError());
}

XInputRuntime& xinput_runtime()
{
    static XInputRuntime runtime;
    static const bool initialized = [] {
        load_xinput_runtime(runtime);
        return true;
    }();
    (void)initialized;
    return runtime;
}

/// A Win32 handle owned by shared_ptr, so a thread that missed its shutdown
/// deadline still owns a valid handle. Closing the event in the service
/// destructor would otherwise be a use-after-close.
struct Win32HandleCloser {
    void operator()(HANDLE handle) const noexcept
    {
        if (handle != nullptr) {
            ::CloseHandle(handle);
        }
    }
};
using SharedEvent = std::shared_ptr<void>;

// --- Button table -----------------------------------------------------------

/// XInput wButtons bit for each of the 16 bindable buttons, in the order
/// GamepadInputService.AddXInputButtons produces them (LT, RT, then A, B, X, Y,
/// LB, RB, DPad, Start, Back, sticks). The order is observable: capture_next()
/// returns the *first* button of a poll, exactly as the .NET capture did.
///
/// The Guide button (0x0400) is deliberately absent - it is reserved by
/// GamepadInputService.cs:245 and is not a member of XInputPadButton, so it can
/// never be bound here either.
struct ButtonBit {
    std::uint16_t bit;
    domain::XInputPadButton button;
};

constexpr std::array<ButtonBit, 16> kButtonBits{{
    {0x1000, domain::XInputPadButton::a},           // A
    {0x2000, domain::XInputPadButton::b},           // B
    {0x4000, domain::XInputPadButton::x},           // X
    {0x8000, domain::XInputPadButton::y},           // Y
    {0x0100, domain::XInputPadButton::lb},          // LB
    {0x0200, domain::XInputPadButton::rb},          // RB
    {0x0001, domain::XInputPadButton::dpad_up},     // DPadUp
    {0x0002, domain::XInputPadButton::dpad_down},   // DPadDown
    {0x0004, domain::XInputPadButton::dpad_left},   // DPadLeft
    {0x0008, domain::XInputPadButton::dpad_right},  // DPadRight
    {0x0010, domain::XInputPadButton::start},       // Start
    {0x0020, domain::XInputPadButton::back},        // Back
    {0x0040, domain::XInputPadButton::left_stick},  // LeftThumb
    {0x0080, domain::XInputPadButton::right_stick}, // RightThumb
}};

/// Analog triggers, reported first exactly as the .NET service did.
constexpr std::array<domain::XInputPadButton, 2> kTriggerButtons{
    domain::XInputPadButton::lt,
    domain::XInputPadButton::rt,
};

/// The buttons pressed in one slot, in the .NET observation order.
std::vector<domain::GamepadInput> pressed_inputs(const XInputPadSnapshot& pad)
{
    std::vector<domain::GamepadInput> inputs;
    if (!pad.connected) {
        return inputs;
    }
    if (pad.left_trigger >= kGamepadTriggerPressThreshold) {
        inputs.push_back(domain::GamepadInput{
            domain::GamepadSource::xinput,
            std::string(domain::x_input_button_name(kTriggerButtons[0]).value())});
    }
    if (pad.right_trigger >= kGamepadTriggerPressThreshold) {
        inputs.push_back(domain::GamepadInput{
            domain::GamepadSource::xinput,
            std::string(domain::x_input_button_name(kTriggerButtons[1]).value())});
    }
    for (const auto& entry : kButtonBits) {
        if ((pad.buttons & entry.bit) != 0) {
            inputs.push_back(domain::GamepadInput{
                domain::GamepadSource::xinput,
                std::string(domain::x_input_button_name(entry.button).value())});
        }
    }
    return inputs;
}

/// The case-insensitive comparison of GamepadBindingMatcher.cs: same source and
/// the same button name, ignoring case. A DirectInput binding never matches
/// here because this backend does not enumerate DirectInput devices.
bool matches(const domain::GamepadBinding& binding, const domain::GamepadInput& input)
{
    if (!binding.is_assigned() || binding.source != input.source) {
        return false;
    }
    return domain::detail::equals_ignore_ascii_case(binding.button_id, input.button_id);
}

std::vector<GamepadDevice> devices_from(const std::array<XInputPadSnapshot, kXInputDeviceCount>& pads)
{
    std::vector<GamepadDevice> devices;
    for (int index = 0; index < kXInputDeviceCount; ++index) {
        if (!pads[static_cast<std::size_t>(index)].connected) {
            continue;
        }
        GamepadDevice device;
        device.source = domain::GamepadSource::xinput;
        device.index = index;
        device.product_name.clear(); // XInput exposes no product name
        device.connected = true;
        devices.push_back(device);
    }
    return devices;
}

std::uint32_t connection_signature(const std::array<XInputPadSnapshot, kXInputDeviceCount>& pads)
{
    std::uint32_t signature = 0;
    for (int index = 0; index < kXInputDeviceCount; ++index) {
        if (pads[static_cast<std::size_t>(index)].connected) {
            signature |= 1u << index;
        }
    }
    return signature;
}

constexpr int kMaxDebounceSamples = 16;

/// Bound of the stop() join. Not shared with the hotkey backend on purpose:
/// each backend owns its own constant rather than depending on the other's
/// header.
constexpr int kGamepadDefaultShutdownTimeoutMs = 2000;

} // namespace

GamepadPadReader xinput_pad_reader()
{
    XInputRuntime& runtime = xinput_runtime();
    if (runtime.get_state == nullptr) {
        // No runtime: every slot reports "no controller". This is not an error
        // for the service - it is the "no device" answer - and
        // xinput_runtime_failure() explains why.
        return [](int, XInputPadSnapshot& out) {
            out = XInputPadSnapshot{};
            return false;
        };
    }
    const XInputGetStateFn get_state = runtime.get_state;
    return [get_state](int index, XInputPadSnapshot& out) {
        out = XInputPadSnapshot{};
        if (index < 0 || index >= kXInputDeviceCount) {
            return false;
        }
        XInputStateCompat state{};
        if (get_state(static_cast<DWORD>(index), &state) != 0) {
            return false;
        }
        out.connected = true;
        out.packet_number = state.packet_number;
        out.buttons = state.buttons;
        out.left_trigger = state.left_trigger;
        out.right_trigger = state.right_trigger;
        return true;
    };
}

std::string xinput_runtime_name()
{
    return xinput_runtime().name;
}

std::string xinput_runtime_failure()
{
    return xinput_runtime().failure;
}

// ---------------------------------------------------------------------------
// Win32GamepadService
// ---------------------------------------------------------------------------

struct Win32GamepadService::Impl {
    /// One dedicated, mutex-protected input state.
    ///
    /// This single struct is the fix for the .NET service's unsynchronised
    /// fields (GamepadInputService.cs:47-51,82-88,291-307): _recordDown,
    /// _cancelDown and both binding fields were plain members written by the
    /// 33 ms poll thread and read by the UI thread. Here the poll thread is the
    /// only writer, and every reader copies a snapshot under `mutex`.
    struct PolledInput {
        std::array<XInputPadSnapshot, kXInputDeviceCount> pads{};
        std::vector<GamepadDevice> devices;
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

    GamepadPadReader reader;
    SharedEvent stop_event;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> started{false};
    std::atomic<int> debounce_samples{2};
    std::atomic<int> rescan_cooldown_ms{1000};
    std::atomic<int> poll_interval_ms{static_cast<int>(kGamepadPollIntervalMs)};
    std::atomic<int> shutdown_timeout_ms{kGamepadDefaultShutdownTimeoutMs};

    explicit Impl(GamepadPadReader pad_reader)
        : reader(pad_reader ? std::move(pad_reader) : xinput_pad_reader())
    {
        stop_event = SharedEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr), Win32HandleCloser{});
    }

    ~Impl()
    {
        stop();
        // The event handle is closed by the shared_ptr, not here: the poll thread
        // holds a copy, so a thread that missed its join deadline still waits on
        // a valid handle.
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
        std::array<XInputPadSnapshot, kXInputDeviceCount> pads{};
        for (int index = 0; index < kXInputDeviceCount; ++index) {
            if (!reader(index, pads[static_cast<std::size_t>(index)])) {
                pads[static_cast<std::size_t>(index)] = XInputPadSnapshot{};
            }
        }

        const std::uint32_t signature = connection_signature(pads);
        const int debounce = clamp_debounce(debounce_samples.load());
        const auto cooldown = std::chrono::milliseconds(rescan_cooldown_ms.load());
        std::vector<GamepadEdge> edges;
        bool capture_done = false;

        {
            const std::lock_guard<std::mutex> lock(mutex);
            state.pads = pads;

            // --- Hot-plug / device loss, rate limited. A controller that keeps
            // failing to appear must not be able to spin this loop: the
            // signature change is only acted on once per cooldown, so the
            // re-enumeration count is bounded by elapsed time, not by failures.
            const bool changed = !state.signature_known || signature != state.signature;
            if (changed) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= state.next_rescan) {
                    state.next_rescan = now + cooldown;
                    ++state.rescan_count;
                    state.signature = signature;
                    state.signature_known = true;
                    state.devices = devices_from(pads);
                }
            }

            // --- Collect this poll's pressed buttons.
            std::vector<domain::GamepadInput> inputs;
            int connected = 0;
            for (int index = 0; index < kXInputDeviceCount; ++index) {
                const auto& pad = pads[static_cast<std::size_t>(index)];
                if (!pad.connected) {
                    continue;
                }
                ++connected;
                const auto pressed = pressed_inputs(pad);
                inputs.insert(inputs.end(), pressed.begin(), pressed.end());
            }

            // --- Capture mode ends on the first press, or reports that every pad
            // went away. A second capture_next() is refused, so the previous
            // waiter is never orphaned the way StartCapture orphaned it. The
            // capture also short-circuits the edge pass, exactly as the .NET
            // Process() returned straight after completing a capture.
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

            // A capture that just consumed this poll short-circuits the edge
            // pass, exactly as the .NET Process() returned straight after
            // completing a capture.
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
        const auto interval = std::chrono::milliseconds(poll_interval_ms.load());
        const HANDLE stop_handle = static_cast<HANDLE>(stop_event.get());
        while (::WaitForSingleObject(stop_handle, static_cast<DWORD>(interval.count())) != WAIT_OBJECT_0) {
            poll_once();
        }
        running.store(false);
    }

    Status start_polling()
    {
        if (started.load()) {
            return Status::success();
        }
        if (stop_event != nullptr) {
            ::ResetEvent(static_cast<HANDLE>(stop_event.get()));
        }
        // The thread keeps its own reference to the event, so a stop that misses
        // its deadline cannot leave it waiting on a closed handle.
        auto stop_handle = stop_event;
        try {
            thread = std::thread([this, stop_handle]() { poll_main(); });
        } catch (...) {
            return Status::failure(ErrorCode::resource_exhausted, "the gamepad poll thread could not be started");
        }
        started.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (!running.load() && std::chrono::steady_clock::now() < deadline) {
            ::Sleep(1);
        }
        if (!running.load()) {
            stop();
            return Status::failure(ErrorCode::timeout, "the gamepad poll thread did not start within 2000 ms");
        }
        return Status::success();
    }

    Status stop()
    {
        bool was_started = started.exchange(false);
        if (stop_event != nullptr) {
            ::SetEvent(static_cast<HANDLE>(stop_event.get()));
        }
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
            ::Sleep(1);
        }
        thread.join();
        return Status::success();
    }
};

Win32GamepadService::Win32GamepadService(GamepadPadReader reader)
    : impl_(std::make_unique<Impl>(std::move(reader)))
{
}

Win32GamepadService::~Win32GamepadService()
{
    if (impl_ != nullptr) {
        impl_->stop();
    }
}

Status Win32GamepadService::set_event_sink(GamepadEventSink sink)
{
    if (impl_ == nullptr) {
        return Status::failure(ErrorCode::unavailable, "gamepad service is not constructed");
    }
    const std::lock_guard<std::mutex> lock(impl_->sink_mutex);
    impl_->sink = std::move(sink);
    return Status::success();
}

Status Win32GamepadService::apply_settings(const AppSettings& settings)
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

Status Win32GamepadService::stop()
{
    if (impl_ == nullptr) {
        return Status::success();
    }
    return impl_->stop();
}

Status Win32GamepadService::cancel_capture()
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

Result<domain::GamepadBinding> Win32GamepadService::capture_next(const CancellationToken& cancellation)
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

std::vector<GamepadDevice> Win32GamepadService::devices() const
{
    if (impl_ == nullptr) {
        return {};
    }
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state.devices;
}

Status Win32GamepadService::wait_for_record_release(const CancellationToken& cancellation) const
{
    if (impl_ == nullptr) {
        return Status::success();
    }
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(impl_->mutex);
            const bool released = impl_->settled.wait_for(
                lock, std::chrono::milliseconds(50), [this]() { return !impl_->state.record_down; });
            if (released) {
                return Status::success();
            }
        }
        if (cancellation.is_cancellation_requested()) {
            return Status::failure(ErrorCode::cancelled, "waiting for the record button release was cancelled");
        }
    }
}

bool Win32GamepadService::record_held() const
{
    if (impl_ == nullptr) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state.record_down;
}

void Win32GamepadService::set_debounce_samples(int samples)
{
    if (impl_ != nullptr) {
        impl_->debounce_samples.store(impl_->clamp_debounce(samples));
    }
}

void Win32GamepadService::set_rescan_cooldown(std::chrono::milliseconds cooldown)
{
    if (impl_ != nullptr) {
        impl_->rescan_cooldown_ms.store(cooldown.count() > 0 ? static_cast<int>(cooldown.count()) : 1);
    }
}

void Win32GamepadService::set_poll_interval(std::chrono::milliseconds interval)
{
    if (impl_ != nullptr) {
        const auto count = interval.count();
        impl_->poll_interval_ms.store(count > 0 ? static_cast<int>(count) : 1);
    }
}

void Win32GamepadService::set_shutdown_timeout(std::chrono::milliseconds timeout)
{
    if (impl_ != nullptr) {
        impl_->shutdown_timeout_ms.store(timeout.count() > 0 ? static_cast<int>(timeout.count()) : 1);
    }
}

int Win32GamepadService::rescan_count() const
{
    if (impl_ == nullptr) {
        return 0;
    }
    // Read under the poll mutex: the count is written by the poll thread.
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state.rescan_count;
}

bool Win32GamepadService::polling() const
{
    return impl_ != nullptr && impl_->started.load() && impl_->running.load();
}

} // namespace voicetyper::platform::win32
