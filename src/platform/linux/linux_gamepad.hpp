#pragma once

// Linux implementation of the frozen platform::GamepadService contract on
// evdev (/dev/input/event*).
//
// Evidence and contract:
//   * src/platform/api/gamepad.hpp - the frozen contract implemented here by
//     `LinuxGamepadService`: 33 ms polling, debounced press/release edges
//     delivered on the poll thread, "press any button" capture that is
//     cancelable, an idempotent stop(), and device loss that is reported through
//     devices() instead of stopping the service.
//   * src/platform/windows/win32_gamepad.{hpp,cpp} - the reference semantics of
//     the same contract (XInput half). The observable behaviour is reproduced
//     here, with evdev as the transport.
//   * VoiceTyper.App/Services/GamepadInputService.cs:20 - the .NET reference the
//     Windows backend copies: trigger threshold 30, record/cancel edge detection,
//     the "press any button" capture mode.
//   * docs/migration/cpp/release-gates.md risk R8/R10 - on Wayland a client
//     cannot read global input through the display server, so the input devices
//     are read directly.
//
// Scope and intended-parity decisions:
//   * Only the XInput binding grammar is supported, exactly as on Windows:
//     "XInput|A" matches an evdev pad, and a well-formed
//     "DInput|<Product>|<index>" binding parses and loads but never matches,
//     because this backend does not enumerate DirectInput devices. The gap is
//     deliberate and documented (PAD-01 stays open) rather than reported as "no
//     controller".
//   * XInputPadButton -> evdev: A=BTN_SOUTH(0x130), B=BTN_EAST(0x131),
//     X=BTN_WEST(0x134), Y=BTN_NORTH(0x133), LB=BTN_TL(0x136), RB=BTN_TR(0x137),
//     LT=ABS_Z(0x02), RT=ABS_RZ(0x05), DPadUp/Down=ABS_HAT0Y(0x11) -1/+1,
//     DPadLeft/Right=ABS_HAT0X(0x10) -1/+1, Start=BTN_START(0x13b),
//     Back=BTN_SELECT(0x13a), LeftStick=BTN_THUMBL(0x13d),
//     RightStick=BTN_THUMBR(0x13e). BTN_SOUTH and BTN_GAMEPAD are the same code,
//     and the Guide button (BTN_MODE) is deliberately absent, as on Windows.
//   * Analog triggers: evdev drivers disagree about the range. A value in
//     0..255 is already the scale the .NET threshold (30) was measured on; a
//     negative value is the signed -32768..32767 range and is normalised to
//     0..255. Both are documented in linux_gamepad.cpp.
//   * An evdev node is a gamepad when it reports EV_KEY and one of the face
//     buttons (BTN_SOUTH/BTN_GAMEPAD, BTN_EAST, BTN_NORTH, BTN_WEST). A
//     keyboard, a mouse, a power button and a lid switch all fail that test.
//   * The device "slot" of the contract carries the evdev node index
//     (/dev/input/event<index>) and the product name from EVIOCGNAME.
//
// Threading: edges are delivered on the owned poll thread. The poll state is one
// mutex-protected struct; devices() and the capture wait copy a snapshot under
// that mutex and never hold it across a sink call. apply_settings/stop/
// cancel_capture are thread-safe.
//
// Platform boundary: this header is standard C++20 and free of <linux/input.h>,
// so the poll policy, the debouncer, the mapping table and the binding matching
// are testable on any host, and with a fake device reader the whole service is
// testable with no hardware at all. All evdev calls live in linux_gamepad.cpp.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/gamepad_binding.hpp"
#include "domain/settings.hpp"
#include "platform/api/gamepad.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace voicetyper::platform::linuxos {

using domain::AppSettings;
using domain::CancellationToken;
using domain::Error;
using domain::ErrorCode;
using domain::GamepadBinding;
using domain::GamepadInput;
using domain::GamepadSource;
using domain::Result;
using domain::Status;

/// Analog trigger press threshold, mirroring the Windows backend and
/// GamepadInputService.cs:20. The raw axis is normalised to 0..255 first, so a
/// real press is far above 30 and a resting trigger far below it.
inline constexpr int kGamepadTriggerPressThreshold = 30;

/// One EV_ABS axis value, in the raw units the driver reports.
struct EvdevAxisValue {
    std::uint16_t code = 0;
    std::int32_t value = 0;
};

/// One gamepad node as the reader sees it at one poll instant. The codes are raw
/// evdev codes (BTN_*/ABS_*); the mapping to XInputPadButton is this backend's
/// job, which is what makes the table testable without hardware.
struct EvdevGamepadSnapshot {
    bool connected = false;
    /// /dev/input/event<node_index>. The contract's device "slot".
    int node_index = -1;
    /// EVIOCGNAME, empty when the driver reports no name.
    std::string product_name;
    /// Raw EV_KEY codes currently held (BTN_*).
    std::vector<std::uint16_t> pressed_keys;
    /// The four axes the mapping uses: ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y.
    std::vector<EvdevAxisValue> axes;
};

/// Reads every gamepad node currently attached. Never throws, never blocks. The
/// seam exists so the poll policy, the debouncer, the mapping table and the
/// device-loss path are testable with no hardware and no /dev/input access.
using GamepadDeviceReader = std::function<std::vector<EvdevGamepadSnapshot>()>;

/// The real evdev reader: scans /dev/input/event*, keeps the gamepad nodes open
/// (O_RDONLY | O_NONBLOCK | O_CLOEXEC), drains their events and reports their
/// current state. A node that disappears is closed and dropped; a node that
/// appears is picked up by a rate-limited rescan. A machine with no gamepad and
/// a session without /dev/input access both report an empty list - that is the
/// "no device" answer, not an error.
[[nodiscard]] GamepadDeviceReader evdev_gamepad_reader();

/// True when an evdev node looks like a gamepad: it reports key events, absolute
/// axes and at least one face button. A virtual keyboard/mouse (ydotoold's
/// device, for example) reports the button codes but no EV_ABS at all, so it must
/// not be listed as a controller (measured live 2026-10-11).
[[nodiscard]] bool evdev_node_is_gamepad(bool has_key_events, bool has_abs_events, bool has_face_buttons);

/// The frozen `platform::GamepadService` contract on Linux, evdev transport.
class LinuxGamepadService final : public GamepadService {
public:
    /// `reader` defaults to the real evdev reader. A test injects a fake one;
    /// the production call site passes nothing.
    explicit LinuxGamepadService(GamepadDeviceReader reader = {});
    ~LinuxGamepadService() override;

    LinuxGamepadService(const LinuxGamepadService&) = delete;
    LinuxGamepadService& operator=(const LinuxGamepadService&) = delete;
    LinuxGamepadService(LinuxGamepadService&&) = delete;
    LinuxGamepadService& operator=(LinuxGamepadService&&) = delete;

    Status set_event_sink(GamepadEventSink sink) override;

    /// Applies both bindings and starts polling. An absent or empty binding is a
    /// success meaning "no gamepad action" (a user may bind only hotkeys); a
    /// *present but malformed* binding is invalid_argument, as the frozen
    /// contract documents. No gamepad attached is not an error either: polling
    /// starts and devices() stays empty.
    Status apply_settings(const AppSettings& settings) override;

    /// Stops polling and drops the bindings. Idempotent; joins the poll thread
    /// within a bounded deadline.
    Status stop() override;

    Result<GamepadBinding> capture_next(const CancellationToken& cancellation) override;
    Status cancel_capture() override;
    std::vector<GamepadDevice> devices() const override;

    /// Consecutive identical samples required before an edge is emitted.
    /// Default 2 (66 ms at the 33 ms cadence). 1 restores the .NET behaviour.
    void set_debounce_samples(int samples);

    /// Minimum time between two device re-enumerations. Default 1000 ms, which
    /// is what stops a node that keeps failing to enumerate from hot-looping.
    void set_rescan_cooldown(std::chrono::milliseconds cooldown);

    /// Poll cadence, fixed at the frozen 33 ms unless a test needs it faster.
    void set_poll_interval(std::chrono::milliseconds interval);

    /// Bound of the stop() join. Default 2000 ms.
    void set_shutdown_timeout(std::chrono::milliseconds timeout);

    /// How many times the poll thread has re-enumerated. Exposed because the
    /// bound is the contract: a device that stays absent must not make this
    /// number grow without limit.
    [[nodiscard]] int rescan_count() const;

    /// True while the poll thread is alive.
    [[nodiscard]] bool polling() const;

    /// What the last device enumeration saw, for the log.
    [[nodiscard]] std::string diagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voicetyper::platform::linuxos
