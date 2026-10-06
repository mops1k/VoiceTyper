#pragma once

// Global hotkey service contract (platform half).
//
// Evidence: docs/migration/cpp/feature-parity.md rows "Global hotkeys" and
// "Hotkey capture"; docs/migration/cpp/compatibility-contracts.md §6 "Hotkeys";
// .NET reference VoiceTyper.App/Services/IHotkeyService.cs, HotkeyService.cs and
// HotkeyCaptureHook.cs.
//
// Frozen observable contract:
//   * Two global hotkeys: record and cancel. The service re-registers both from
//     the current settings and reports *per-hotkey* registration errors instead
//     of failing as a whole, because the .NET ApplySettings returns a list of
//     error strings and the UI shows all of them.
//   * Push-to-talk needs the release edge as well as the press edge, so the
//     service also reports that the physical key is no longer held. A
//     registration that only produces press edges cannot serve push-to-talk.
//   * Unregistering is unconditional and idempotent: after unregister_all() the
//     keys are free for other applications.
//
// Thread affinity: press/release events are delivered on a backend-owned
// message-pump/input thread, not the UI thread. The sink must be cheap and
// thread-safe; if it needs the UI thread it must marshal itself. Exceptions
// cannot escape the sink. Sinks must not call back into HotkeyService
// synchronously (use the posted/queued form for reconfiguration).
//
// Ownership: the sink is owned by the service for as long as it is registered.
// Unregistering invalidates pending events; the service guarantees no callback
// runs after unregister_all() returns.
//
// Platform mapping: HotkeyGesture (domain) is mapped to a native virtual key
// here. The mapping table and the "no modifier unless F1..F24" capture rule
// live in domain/hotkey_gesture.hpp; this interface only reports the outcome.

#include "domain/error.hpp"
#include "domain/hotkey_gesture.hpp"
#include "domain/settings.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace voicetyper::platform {

using domain::AppSettings;
using domain::ErrorCode;
using domain::HotkeyGesture;
using domain::Result;
using domain::Status;

/// Which global action fired.
enum class HotkeyAction : std::uint8_t {
    /// Record hotkey pressed (rising edge).
    record_pressed = 0,
    /// Record hotkey released (falling edge). Required by push-to-talk.
    record_released = 1,
    /// Cancel hotkey pressed (rising edge).
    cancel_pressed = 2,
};

[[nodiscard]] constexpr std::string_view hotkey_action_name(HotkeyAction action) noexcept
{
    switch (action) {
    case HotkeyAction::record_pressed: return "record_pressed";
    case HotkeyAction::record_released: return "record_released";
    case HotkeyAction::cancel_pressed: return "cancel_pressed";
    }
    return "unknown";
}

/// Receives hotkey edges on the service's input thread.
using HotkeyEventSink = std::function<void(HotkeyAction)>;

/// Per-hotkey registration result. Both hotkeys are always reported, so the UI
/// can show every conflict at once.
struct HotkeyRegistration {
    HotkeyAction action = HotkeyAction::record_pressed;
    /// The gesture that was requested, as parsed from settings.
    HotkeyGesture gesture;
    /// False when the OS refused the registration (already taken by another
    /// application, or the gesture is not registrable on this platform).
    bool registered = false;
    /// Native key code actually registered, 0 when not registered. The .NET
    /// contract exposes this as RecordKeyVk so the UI can show it; 0 also means
    /// "a gamepad is bound instead".
    std::int32_t native_key_code = 0;
    /// Human-readable reason when `registered` is false.
    std::string error;
};

/// Result of applying settings: one entry per hotkey, always two entries.
struct HotkeyRegistrationReport {
    std::vector<HotkeyRegistration> record;
    std::vector<HotkeyRegistration> cancel;

    /// True when both hotkeys registered successfully.
    [[nodiscard]] bool all_registered() const;

    /// All error strings, in record-then-cancel order, matching the .NET
    /// `IReadOnlyList<string> ApplySettings(...)` return shape.
    [[nodiscard]] std::vector<std::string> errors() const;
};

class HotkeyService {
public:
    virtual ~HotkeyService() = default;

    HotkeyService(const HotkeyService&) = delete;
    HotkeyService& operator=(const HotkeyService&) = delete;
    HotkeyService(HotkeyService&&) = delete;
    HotkeyService& operator=(HotkeyService&&) = delete;

    /// Installs the edge sink. Must be called before apply_settings(). The sink
    /// is owned by the service and is dropped by unregister_all().
    virtual Status set_event_sink(HotkeyEventSink sink) = 0;

    /// Re-registers both global hotkeys from `settings`.
    ///
    /// Never fails as a whole: a refused registration is reported per hotkey.
    /// Failure codes: invalid_argument (a hotkey string that does not parse),
    /// unavailable (no global hotkey mechanism on this platform). When
    /// `settings.record_hotkey` does not parse, the report marks that hotkey as
    /// not registered with the parse error as its reason.
    [[nodiscard]] virtual Result<HotkeyRegistrationReport> apply_settings(const AppSettings& settings) = 0;

    /// Releases both global hotkeys. Idempotent; after it returns no sink
    /// callback can still be in flight.
    virtual Status unregister_all() = 0;

    /// Native key code of the registered record hotkey, 0 when not registered
    /// or when a gamepad is bound instead. Mirrors the .NET `RecordKeyVk`.
    [[nodiscard]] virtual std::int32_t record_key_code() const noexcept = 0;

protected:
    HotkeyService() = default;
};

inline bool HotkeyRegistrationReport::all_registered() const
{
    const auto registered = [](const std::vector<HotkeyRegistration>& list) {
        return !list.empty() && list.front().registered;
    };
    return registered(record) && registered(cancel);
}

inline std::vector<std::string> HotkeyRegistrationReport::errors() const
{
    std::vector<std::string> messages;
    for (const auto& entry : record) {
        if (!entry.registered) {
            messages.push_back(entry.error);
        }
    }
    for (const auto& entry : cancel) {
        if (!entry.registered) {
            messages.push_back(entry.error);
        }
    }
    return messages;
}

} // namespace voicetyper::platform
