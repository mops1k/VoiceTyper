// Linux-only contract test for the global hotkey backend.
//
// What this proves, and what it deliberately does not:
//
//   Proves, on Linux, with no desktop session and no /dev/input access:
//     * the key-name map: the WPF key names the settings file stores
//       ("Space", "F12", "NumPad0", "OemTilde", "A") map to the Linux input
//       event codes, and the reverse map answers the capture hook with the same
//       names, so a captured gesture round-trips through settings unchanged;
//     * linux_hotkeys: a settings apply always reports both hotkeys, an
//       unparsable gesture is reported as not registered with its reason instead
//       of failing the whole call, unregister_all() is idempotent, and a session
//       without access to the input devices refuses registration instead of
//       pretending the hotkey works;
//     * the capture hook reports unavailable rather than blocking forever when
//       the input devices cannot be opened;
//     * the capability state: a service with nothing registered reports `none`,
//       an evdev registration reports `evdev` (press *and* release), and
//       unregistering returns to `none` (VT-PLT-1308).
//
//   Deliberately NOT exercised automatically: a real global key press. It would
//   require the developer to hold a key while the suite runs, and it belongs to
//   the physical Arch Linux gate (plan p_b5fbda6bfa1d, phase 5).

#include "domain/hotkey_gesture.hpp"
#include "domain/settings.hpp"
#include "platform/api/hotkeys.hpp"
#include "platform/linux/linux_hotkeys.hpp"
#include "platform/linux/linux_keymap.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace voicetyper;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

void check_keymap()
{
    using namespace voicetyper::platform::linuxos;

    check(hotkey_key_code("Space") == 57, "Space maps to KEY_SPACE");
    check(hotkey_key_code("Escape") == 1, "Escape maps to KEY_ESC");
    check(hotkey_key_code("F12") == 88, "F12 maps to KEY_F12");
    check(hotkey_key_code("F24") == 194, "F24 maps to KEY_F24");
    check(hotkey_key_code("A") == 30, "A maps to KEY_A");
    check(hotkey_key_code("Z") == 44, "Z maps to KEY_Z");
    check(hotkey_key_code("V") == 47, "V maps to KEY_V");
    check(hotkey_key_code("D0") == 11, "D0 maps to KEY_0");
    check(hotkey_key_code("D1") == 2, "D1 maps to KEY_1");
    check(hotkey_key_code("NumPad0") == 82, "NumPad0 maps to KEY_KP0");
    check(hotkey_key_code("OemTilde") == 41, "OemTilde maps to KEY_GRAVE");
    check(hotkey_key_code("OemQuestion") == 53, "OemQuestion maps to KEY_SLASH");
    check(hotkey_key_code("Enter") == 28, "Enter maps to KEY_ENTER");
    check(hotkey_key_code("PageDown") == 109, "PageDown maps to KEY_PAGEDOWN");
    check(hotkey_key_code("NoSuchKey") == 0, "an unknown key name maps to 0");
    check(hotkey_key_code("") == 0, "an empty key name maps to 0");

    // The reverse map is what the capture hook reports.
    check(hotkey_key_name(57) == "Space", "KEY_SPACE reads back as Space");
    check(hotkey_key_name(88) == "F12", "KEY_F12 reads back as F12");
    check(hotkey_key_name(30) == "A", "KEY_A reads back as A");
    check(hotkey_key_name(9999).empty(), "an unknown code has no key name");

    check(is_modifier_key_code(29), "KEY_LEFTCTRL is a modifier");
    check(is_modifier_key_code(125), "KEY_LEFTMETA is a modifier");
    check(!is_modifier_key_code(57), "KEY_SPACE is not a modifier");
}

void check_hotkey_service()
{
    using namespace voicetyper::platform;
    using namespace voicetyper::platform::linuxos;

    LinuxHotkeyService hotkeys;
    int events = 0;
    check(hotkeys.set_event_sink([&events](HotkeyAction) { ++events; }).is_ok(),
        "the event sink is installed");

    domain::AppSettings settings;
    settings.record_hotkey = "Ctrl+Alt+Space";
    settings.cancel_hotkey = "Ctrl+Alt+Escape";

    const auto report = hotkeys.apply_settings(settings);
    check(report.is_ok(), "applying settings never fails as a whole");
    if (report.is_ok()) {
        // Both hotkeys are always reported, registered or not: the UI shows
        // every conflict at once.
        check(!report.value().record.empty(), "the record hotkey is reported");
        check(!report.value().cancel.empty(), "the cancel hotkey is reported");
        if (report.value().all_registered()) {
            std::cout << "note: this session can read the input devices, hotkeys registered\n";
            check(hotkeys.record_key_code() == 57, "the registered record key code is reported");
        } else {
            for (const auto& error : report.value().errors()) {
                std::cout << "note: hotkey not registered: " << error << '\n';
            }
            check(hotkeys.record_key_code() == 0, "an unregistered hotkey reports code 0");
        }
    }

    // An unparsable gesture is reported per hotkey, with the parse reason.
    domain::AppSettings broken;
    broken.record_hotkey = "";
    broken.cancel_hotkey = "Ctrl+";
    const auto broken_report = hotkeys.apply_settings(broken);
    check(broken_report.is_ok(), "an unparsable gesture does not fail the whole apply");
    if (broken_report.is_ok()) {
        check(!broken_report.value().errors().empty(), "the unparsable gesture is reported as an error");
    }

    check(hotkeys.unregister_all().is_ok(), "unregister_all succeeds");
    check(hotkeys.unregister_all().is_ok(), "unregister_all is idempotent");
    check(hotkeys.record_key_code() == 0, "after unregistering there is no key code");
}

void check_capability()
{
    using namespace voicetyper::platform::linuxos;

    check(std::string_view(hotkey_capability_name(HotkeyCapability::none)) == "none",
        "the none capability is named");
    check(std::string_view(hotkey_capability_name(HotkeyCapability::evdev)) == "evdev",
        "the evdev capability is named");
    check(std::string_view(hotkey_capability_name(HotkeyCapability::kglobal_accel)) == "kglobal_accel",
        "the KGlobalAccel capability is named");

    LinuxHotkeyService hotkeys;
    check(hotkeys.capability() == HotkeyCapability::none,
        "a service with nothing registered has no capability");

    domain::AppSettings settings;
    settings.record_hotkey = "Ctrl+Alt+Space";
    settings.cancel_hotkey = "Ctrl+Alt+Escape";
    const auto report = hotkeys.apply_settings(settings);
    if (report.is_ok() && report.value().all_registered()) {
        // evdev is the only backend that delivers the release edge, so a
        // successful evdev registration is exactly the evdev capability.
        check(hotkeys.capability() == HotkeyCapability::evdev,
            "an evdev registration reports the evdev capability");
    } else {
        check(hotkeys.capability() == HotkeyCapability::none,
            "without input devices the capability is none");
    }

    static_cast<void>(hotkeys.unregister_all());
    check(hotkeys.capability() == HotkeyCapability::none,
        "after unregistering the capability is none again");
}

void check_capture_hook()
{
    using namespace voicetyper::platform::linuxos;

    HotkeyCaptureHook hook;
    const auto started = hook.start();
    if (started.is_error()) {
        // No /dev/input access is the normal state of this session: the hook has
        // to say so instead of blocking.
        std::cout << "note: hotkey capture unavailable: " << started.error().to_string() << '\n';
        check(started.code() == domain::ErrorCode::permission_denied
                || started.code() == domain::ErrorCode::unavailable,
            "an unavailable capture reports permission_denied or unavailable");
        static_cast<void>(hook.stop());
        return;
    }

    // Nothing is pressed while the test runs, so the capture is cancelled from
    // another thread: the hook has to return, not block forever.
    domain::CancellationSource source;
    std::thread canceller([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        source.request_cancellation();
    });
    const auto captured = hook.capture_next(source.token());
    canceller.join();
    check(captured.is_error() && captured.code() == domain::ErrorCode::cancelled,
        "a cancelled capture returns cancelled instead of blocking");
    static_cast<void>(hook.stop());
}

} // namespace

int main()
{
    check_keymap();
    check_hotkey_service();
    check_capability();
    check_capture_hook();

    if (failures != 0) {
        std::cerr << "linux-hotkeys-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "linux-hotkeys-contract: OK\n";
    return 0;
}
