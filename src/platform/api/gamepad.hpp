#pragma once

// Gamepad input contract (platform half).
//
// Evidence: docs/migration/cpp/feature-parity.md row "Gamepads";
// docs/migration/cpp/compatibility-contracts.md §6 "Gamepads"; .NET reference
// VoiceTyper.App/Services/IGamepadInputService.cs, GamepadInputService.cs,
// GamepadBindingParser.cs and GamepadBindingMatcher.cs.
//
// Frozen observable contract:
//   * XInput devices 0..3 plus DirectInput joysticks are polled; the .NET poll
//     interval is 33 ms, so that is the cadence a back-end must keep to feel
//     identical.
//   * The record/cancel bindings come from settings and are compared
//     case-insensitively (see domain/gamepad_binding.hpp for the grammar).
//   * Capture ("press any button to bind") is a distinct mode that returns the
//     first observed button and then ends. It is cancelable.
//   * No UI thread is required for polling or capture; the .NET interface
//     documents this explicitly and the C++ contract keeps it, so a blocking
//     capture may run on a worker thread.
//
// Thread affinity: binding edges are delivered on the poll thread. The sink must
// be cheap, thread-safe and must marshal to the UI thread itself if it needs one.
// Exceptions cannot escape the sink. Sinks must not call apply_settings()
// synchronously from the poll thread; queue the reconfiguration instead.
//
// Ownership: sinks and cancellation tokens are owned by the service for the
// duration of the call. Capture takes a snapshot of the poll state and releases
// it before returning.
//
// Device loss: losing a pad must not stop the service or fire spurious edges;
// it is reported through the next availability query and logged.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/gamepad_binding.hpp"
#include "domain/settings.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::platform {

using domain::AppSettings;
using domain::CancellationToken;
using domain::ErrorCode;
using domain::GamepadBinding;
using domain::GamepadInput;
using domain::GamepadSource;
using domain::Result;
using domain::Status;

/// Poll cadence of the .NET implementation, milliseconds. Frozen so a
/// controller feels the same after the migration.
inline constexpr std::int64_t kGamepadPollIntervalMs = 33;

/// Number of XInput controller slots Windows exposes.
inline constexpr int kXInputDeviceCount = 4;

/// Which action a gamepad edge triggered.
enum class GamepadAction : std::uint8_t {
    record_pressed = 0,
    record_released = 1,
    cancel_pressed = 2,
};

[[nodiscard]] constexpr std::string_view gamepad_action_name(GamepadAction action) noexcept
{
    switch (action) {
    case GamepadAction::record_pressed: return "record_pressed";
    case GamepadAction::record_released: return "record_released";
    case GamepadAction::cancel_pressed: return "cancel_pressed";
    }
    return "unknown";
}

/// A binding edge, carrying the raw press that produced it so the UI can show
/// which device/button was seen.
struct GamepadEdge {
    GamepadAction action = GamepadAction::record_pressed;
    GamepadBinding binding;
    GamepadInput input;
};

/// Receives binding edges on the poll thread.
using GamepadEventSink = std::function<void(const GamepadEdge&)>;

/// A connected controller.
struct GamepadDevice {
    GamepadSource source = GamepadSource::none;
    /// XInput slot index 0..3, or -1 for DirectInput devices.
    int index = -1;
    /// XInput: empty. DirectInput: the product name reported by the backend.
    std::string product_name;
    /// True when the device is usable right now.
    bool connected = false;
};

class GamepadService {
public:
    virtual ~GamepadService() = default;

    GamepadService(const GamepadService&) = delete;
    GamepadService& operator=(const GamepadService&) = delete;
    GamepadService(GamepadService&&) = delete;
    GamepadService& operator=(GamepadService&&) = delete;

    /// Installs the edge sink. Must be called before apply_settings().
    virtual Status set_event_sink(GamepadEventSink sink) = 0;

    /// Applies the record/cancel bindings from settings and starts polling.
    ///
    /// An empty or unparseable binding means "no gamepad action"; it is a
    /// success, not an error, because a user may bind only hotkeys. Failure
    /// codes: invalid_argument (a present but malformed binding string),
    /// unavailable (no input backend on this platform).
    virtual Status apply_settings(const AppSettings& settings) = 0;

    /// Stops polling and drops the bindings. Idempotent.
    virtual Status stop() = 0;

    /// Waits for the next button press and returns its binding.
    ///
    /// Blocks on a worker thread (no UI thread required). `cancellation` ends the
    /// wait with ErrorCode::cancelled. Failure codes: cancelled, unavailable
    /// (no input backend), device_disconnected (every pad went away before a
    /// press was seen).
    [[nodiscard]] virtual Result<GamepadBinding> capture_next(const CancellationToken& cancellation) = 0;

    /// Ends an active capture_next() wait early. Safe from another thread; the
    /// waiting call then returns cancelled.
    virtual Status cancel_capture() = 0;

    /// Devices the backend currently sees. May be empty; that is not an error.
    [[nodiscard]] virtual std::vector<GamepadDevice> devices() const = 0;

protected:
    GamepadService() = default;
};

} // namespace voicetyper::platform
