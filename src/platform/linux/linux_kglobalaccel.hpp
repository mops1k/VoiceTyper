#pragma once

// KGlobalAccel (D-Bus) fallback for the global hotkey contract.
//
// Why this exists: on Wayland a client cannot register a global shortcut through
// the display server, so evdev (/dev/input) is the primary backend. When the
// session does not give the process access to the input devices (the user is not
// in the `input` group and no udev ACL applies), org.kde.kglobalaccel is the only
// mechanism left.
//
// Release edge: measured live on 2026-10-11 (kglobalaccel 6.30, Plasma 6.7.5,
// service owned by kwin_wayland), the Component interface exposes
// globalShortcutPressed, globalShortcutRepeated and globalShortcutReleased, and a
// registration made with the SetPresent flag receives both the press and the
// release edge. An older kglobalaccel has no released signal; then the capability
// is `kglobal_accel_press_only` and the composition degrades push-to-talk to
// toggle (VT-PLT-1307/1308).
//
// Two details of the D-Bus API that are easy to get wrong, both verified against
// the KGlobalAccel sources and the live session:
//   * setShortcut() MUST carry SetPresent (2), or kglobalaccel stores the
//     shortcut in kglobalshortcutsrc but keeps the component inactive
//     (Component.isActive == false) and emits no signal at all.
//   * The signals live on /component/<componentUnique>, so the client subscribes
//     there after doRegister(); that is exactly what the C++ API does through
//     getComponent(componentUnique, remember = true).
//
// Component identity: actionId = {"voicetyper", "<action>", "VoiceTyper",
// "<Action>"}.

#include "platform/api/hotkeys.hpp"
#include "platform/linux/linux_hotkeys.hpp"

#include <QDBusConnection>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace voicetyper::platform::linuxos {

class LinuxKGlobalAccelHotkeys final : public platform::HotkeyService {
public:
    /// Connects to the session bus.
    LinuxKGlobalAccelHotkeys();
    /// Uses an explicit connection. The contract test passes a bus that does not
    /// exist, so no shortcut is ever registered in the developer's session.
    explicit LinuxKGlobalAccelHotkeys(QDBusConnection connection);
    ~LinuxKGlobalAccelHotkeys() override;

    LinuxKGlobalAccelHotkeys(const LinuxKGlobalAccelHotkeys&) = delete;
    LinuxKGlobalAccelHotkeys& operator=(const LinuxKGlobalAccelHotkeys&) = delete;
    LinuxKGlobalAccelHotkeys(LinuxKGlobalAccelHotkeys&&) = delete;
    LinuxKGlobalAccelHotkeys& operator=(LinuxKGlobalAccelHotkeys&&) = delete;

    Status set_event_sink(platform::HotkeyEventSink sink) override;
    [[nodiscard]] Result<platform::HotkeyRegistrationReport> apply_settings(const AppSettings& settings) override;
    Status unregister_all() override;
    [[nodiscard]] std::int32_t record_key_code() const noexcept override;

    /// True when org.kde.kglobalaccel answers on this connection.
    [[nodiscard]] bool available() const;

    /// `kglobal_accel` when the bus exposes the release signal (push-to-talk
    /// works through the fallback), `kglobal_accel_press_only` when it does not,
    /// `none` when kglobalaccel itself is unavailable.
    [[nodiscard]] HotkeyCapability capability() const;

    /// Why registration failed, or what was registered. For the log.
    [[nodiscard]] std::string diagnostics() const;

    /// Feeds one globalShortcutPressed signal, exactly as D-Bus delivers it.
    /// Public so the contract test can prove the edge mapping without a desktop
    /// session.
    void handle_shortcut_pressed(std::string_view action_unique);

    /// Feeds one globalShortcutReleased signal. A press-only registration
    /// ignores it, which is the degradation the composition relies on.
    void handle_shortcut_released(std::string_view action_unique);

    /// Test seam: overrides the release-signal observation, so the press-only
    /// branch is contract-tested without an old kglobalaccel.
    void set_release_signal_available_for_test(bool available);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voicetyper::platform::linuxos
