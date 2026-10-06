#pragma once

// Windows gamepad input: XInput polling with a worker thread.
//
// Evidence and contract:
//   * src/platform/api/gamepad.hpp - the frozen platform contract implemented
//     here by `Win32GamepadService`.
//   * VoiceTyper.App/Services/GamepadInputService.cs:15-389 - the .NET service:
//     XInput slots 0..3 plus DirectInput joysticks, a 33 ms poll, trigger
//     threshold 30, the deliberately excluded Guide button, the record/cancel
//     edge detection and the "press any button" capture mode.
//   * docs/migration/cpp/compatibility-contracts.md section 6 "Gamepads" and
//     docs/migration/cpp/feature-parity.md row "Gamepads" (PAD-01 in
//     docs/migration/cpp/parity-ledger.md).
//
// Defects of the .NET reference this backend deliberately does not reproduce
// (audit m_00379ac352a2):
//   * _recordDown, _cancelDown, _recordBinding and _cancelBinding are plain
//     fields written by the 33 ms poll thread and read or written by the UI
//     thread (GamepadInputService.cs:47-51,82-88,291-307) with no
//     synchronization. Here every one of those fields lives in one struct
//     guarded by a single mutex; the poll thread is the only writer of the
//     edge state and the UI only ever reads a snapshot.
//   * StartCapture (GamepadInputService.cs:54-59) overwrites the completion
//     source without completing the previous one, so the first capture's caller
//     waits forever. Here one capture waiter at a time is registered and a
//     second capture_next() fails with invalid_state instead of orphaning one.
//   * The catch-all at :110-114 calls ReloadJoysticks() on *any* exception in
//     the poll loop, so a controller that fails to enumerate hot-loops the
//     re-enumeration. Here re-enumeration is driven by an observed
//     connection-signature change and is rate-limited, so a disappearing
//     controller cannot spin the loop.
//
// Intended-parity decisions (differences from the current .NET build, on
// purpose):
//   * Button edges are debounced: an edge is emitted only after the same sample
//     has been observed `debounce_samples()` times in a row (default 2, i.e.
//     66 ms). One 33 ms poll already removes single-sample noise; two samples
//     also survive a controller that reports a button for one poll after it is
//     released. The latency is a documented, settable value rather than an
//     accident.
//   * XInput is bound through LoadLibraryW/GetProcAddress against the ordered
//     list xinput1_4, xinput1_3, xinput9_1_0, so a machine without any XInput
//     runtime reports `unavailable` with a reason instead of failing to start,
//     and the same "no silent substitution" rule the ASR backends use applies.
//   * Scope: this backend polls XInput only. DirectInput joysticks are not
//     enumerated yet, so a "DInput|Product|index" binding parses and loads but
//     never matches here, and devices() reports XInput devices only. That gap
//     is deliberate and documented rather than silently reported as "no
//     controller": PAD-01 stays open until DirectInput lands.
//
// Platform boundary: the header is standard-C++20 and free of <windows.h>, so
// the poll policy, the debouncer and the binding matching are testable on any
// host, and with a fake pad reader the whole service is testable with no
// hardware at all. All Win32 calls live in win32_gamepad.cpp.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/gamepad_binding.hpp"
#include "domain/settings.hpp"
#include "platform/api/gamepad.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace voicetyper::platform::win32 {

using domain::AppSettings;
using domain::CancellationToken;
using domain::Error;
using domain::ErrorCode;
using domain::GamepadBinding;
using domain::GamepadInput;
using domain::GamepadSource;
using domain::Result;
using domain::Status;

/// Analog trigger press threshold, copied from GamepadInputService.cs:20. XInput
/// reports triggers as 0..255, so a real press is far above 30 and a resting
/// trigger is far below it.
inline constexpr int kGamepadTriggerPressThreshold = 30;

/// One XInput slot, as this backend needs it.
struct XInputPadSnapshot {
    /// False when XInputGetState refused the slot: no pad, or the driver
    /// refused it. Never an error by itself - the .NET service skipped such
    /// slots silently too.
    bool connected = false;
    /// XINPUT_GAMEPAD.wButtons bitmask.
    std::uint16_t buttons = 0;
    std::uint8_t left_trigger = 0;
    std::uint8_t right_trigger = 0;
    /// Monotonic per-slot counter from the driver, 0 when unknown.
    std::uint32_t packet_number = 0;
};

/// Reads one XInput slot. Returns false when the slot holds no controller. The
/// seam exists so the poll policy, the debouncer and the device-loss path are
/// testable with no hardware and no XInput runtime: never throws, never blocks.
using GamepadPadReader = std::function<bool(int index, XInputPadSnapshot& out)>;

/// The real XInput reader: xinput1_4 -> xinput1_3 -> xinput9_1_0, loaded once.
/// When no runtime can be loaded it returns a reader that reports every slot as
/// disconnected; `xinput_runtime_available()` then tells the caller which DLL
/// actually answered, so "no controller" is never confused with "no XInput".
[[nodiscard]] GamepadPadReader xinput_pad_reader();

/// Which XInput runtime answered on this machine, e.g. "xinput1_4.dll", or an
/// empty string when none of them could be loaded.
[[nodiscard]] std::string xinput_runtime_name();

/// Why the real reader cannot work here, empty when it can.
[[nodiscard]] std::string xinput_runtime_failure();

/// The frozen `platform::GamepadService` contract on Win32, XInput half.
///
/// Threading: edges are delivered on the owned poll thread. The poll state is
/// one mutex-protected struct; `devices()`, `wait_for_record_release()` and the
/// capture wait take that mutex only to copy a snapshot, never across a sink
/// call. apply_settings/stop/cancel_capture are thread-safe.
class Win32GamepadService final : public GamepadService {
public:
    /// `reader` defaults to the real XInput reader. A test injects a fake one;
    /// the production call site passes nothing.
    explicit Win32GamepadService(GamepadPadReader reader = {});
    ~Win32GamepadService() override;

    Win32GamepadService(const Win32GamepadService&) = delete;
    Win32GamepadService& operator=(const Win32GamepadService&) = delete;
    Win32GamepadService(Win32GamepadService&&) = delete;
    Win32GamepadService& operator=(Win32GamepadService&&) = delete;

    Status set_event_sink(GamepadEventSink sink) override;

    /// Applies both bindings and starts polling. An empty or unparseable
    /// binding is a success meaning "no gamepad action" (a user may bind only
    /// hotkeys); a *present but malformed* binding is invalid_argument, as the
    /// frozen contract documents.
    Status apply_settings(const AppSettings& settings) override;

    /// Stops polling and drops the bindings. Idempotent; joins the poll thread
    /// within a bounded deadline.
    Status stop() override;

    Result<GamepadBinding> capture_next(const CancellationToken& cancellation) override;
    Status cancel_capture() override;
    std::vector<GamepadDevice> devices() const override;

    /// Push-to-talk support: resolves when the bound record button is no longer
    /// held, or when `cancellation` fires. This is the cancellation-safe
    /// replacement for the .NET WaitForRecordReleaseAsync, which read the
    /// unsynchronised `_recordDown` field and was called with no token
    /// (App.axaml.cs:562).
    [[nodiscard]] Status wait_for_record_release(const CancellationToken& cancellation) const;

    /// True while the bound record button is held, read under the poll mutex.
    /// Replaces the unsynchronised `RecordDown` read of the .NET service.
    [[nodiscard]] bool record_held() const;

    /// Consecutive identical samples required before an edge is emitted.
    /// Default 2 (66 ms at the 33 ms cadence). 1 restores the .NET behaviour.
    void set_debounce_samples(int samples);

    /// Minimum time between two device re-enumerations. Default 1000 ms, which
    /// is what stops a controller that fails to enumerate from hot-looping.
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voicetyper::platform::win32
