#pragma once

// Linux implementation of the frozen platform::HotkeyService contract, with
// evdev as the primary mechanism.
//
// Evidence and contract:
//   * src/platform/api/hotkeys.hpp - two global hotkeys (record and cancel), a
//     press *and* a release edge (push-to-talk needs the release), per-hotkey
//     registration errors instead of one failure for the whole call, and an
//     unconditional idempotent unregister_all().
//   * src/platform/windows/win32_hotkeys.cpp - the Windows backend of the same
//     contract; the observable behaviour is reproduced here.
//   * docs/migration/cpp/release-gates.md risk R8 and R10 - on Wayland a client
//     cannot register a global shortcut through the display server, so the input
//     devices are read directly ("evdev"), with an explicit capability state
//     when /dev/input is not readable.
//
// Why evdev and not the display server: only a raw input device read gives the
// *release* edge, and push-to-talk is a first-class recording mode of this
// product. KGlobalAccel (through D-Bus) is the fallback for a session without
// /dev/input access; when the bus exposes `globalShortcutReleased` it delivers
// the release edge too (measured 2026-10-11 on kglobalaccel 6.30), otherwise the
// registration is press-only and the composition reports that capability instead
// of pretending push-to-talk works.
//
// Device selection: every /dev/input/event* node that reports EV_KEY and the
// letter keys is treated as a keyboard. The set is re-scanned when a session
// starts, so a keyboard plugged in after launch is picked up.
//
// Thread affinity: apply_settings()/unregister_all() are called from the UI
// thread and block only for the duration of the device scan; the edges arrive on
// the backend-owned reader thread, exactly as the contract requires.
//
// Ownership: the object owns its reader thread and the open file descriptors.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/hotkey_gesture.hpp"
#include "domain/settings.hpp"
#include "platform/api/hotkeys.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace voicetyper::platform::linuxos {

using domain::AppSettings;
using domain::CancellationToken;
using domain::ErrorCode;
using domain::HotkeyGesture;
using domain::Result;
using domain::Status;

/// What the current registration can deliver.
///
/// evdev reads `/dev/input` directly and therefore delivers the release edge that
/// push-to-talk needs. KGlobalAccel (the D-Bus fallback for a session without
/// `/dev/input` access) delivers the release edge too when the bus exposes
/// `globalShortcutReleased`; an older kglobalaccel has only the press signal, so
/// that registration is `kglobal_accel_press_only` and the composition degrades
/// push-to-talk to toggle instead of pretending the release exists. `none` means
/// neither mechanism is available (VT-PLT-1308).
enum class HotkeyCapability : std::uint8_t {
    none = 0,
    evdev = 1,
    kglobal_accel = 2,
    kglobal_accel_press_only = 3,
};

[[nodiscard]] constexpr std::string_view hotkey_capability_name(HotkeyCapability capability) noexcept
{
    switch (capability) {
    case HotkeyCapability::none: return "none";
    case HotkeyCapability::evdev: return "evdev";
    case HotkeyCapability::kglobal_accel: return "kglobal_accel";
    case HotkeyCapability::kglobal_accel_press_only: return "kglobal_accel_press_only";
    }
    return "unknown";
}

class LinuxHotkeyService final : public platform::HotkeyService {
public:
    LinuxHotkeyService();
    ~LinuxHotkeyService() override;

    LinuxHotkeyService(const LinuxHotkeyService&) = delete;
    LinuxHotkeyService& operator=(const LinuxHotkeyService&) = delete;
    LinuxHotkeyService(LinuxHotkeyService&&) = delete;
    LinuxHotkeyService& operator=(LinuxHotkeyService&&) = delete;

    Status set_event_sink(platform::HotkeyEventSink sink) override;
    [[nodiscard]] Result<platform::HotkeyRegistrationReport> apply_settings(const AppSettings& settings) override;
    Status unregister_all() override;
    [[nodiscard]] std::int32_t record_key_code() const noexcept override;

    /// True when at least one keyboard device could be opened. False means the
    /// session has no /dev/input access (the user is not in the `input` group and
    /// no udev ACL applies), and the composition reports the degraded capability.
    [[nodiscard]] bool input_devices_available() const;

    /// What the current registration delivers: `evdev` while keyboards are open
    /// (press *and* release), `none` otherwise. The KGlobalAccel fallback is a
    /// separate service (linux_kglobalaccel.hpp) because it is press-only.
    [[nodiscard]] HotkeyCapability capability() const;

    /// Why registration failed, or which devices were opened. For the log.
    [[nodiscard]] std::string diagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Captures the next key combination for the settings window's "record" field.
///
/// It reads the same input devices as the service, but its own reader: the
/// settings dialog runs while the service is still registered, and stealing its
/// events would make the record hotkey fire while the user is assigning one.
///
/// Escape cancels the capture (ErrorCode::cancelled), exactly like the .NET
/// hook; a gesture without a modifier is rejected unless the key is F1..F24,
/// which is the frozen capture rule of src/domain/hotkey_gesture.hpp.
class HotkeyCaptureHook final {
public:
    HotkeyCaptureHook();
    ~HotkeyCaptureHook();

    HotkeyCaptureHook(const HotkeyCaptureHook&) = delete;
    HotkeyCaptureHook& operator=(const HotkeyCaptureHook&) = delete;
    HotkeyCaptureHook(HotkeyCaptureHook&&) = delete;
    HotkeyCaptureHook& operator=(HotkeyCaptureHook&&) = delete;

    /// Opens the input devices. Failure: permission_denied (no readable
    /// /dev/input node), unavailable (no keyboard found).
    Status start();

    /// Blocks until a combination is pressed, the token is cancelled, or the
    /// hook is stopped. Returns the captured gesture, ErrorCode::cancelled for
    /// Escape or a cancellation, and ErrorCode::invalid_state when it was never
    /// started.
    [[nodiscard]] Result<HotkeyGesture> capture_next(const CancellationToken& cancellation);

    /// Releases the devices. Idempotent.
    Status stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voicetyper::platform::linuxos
