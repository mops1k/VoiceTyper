// Linux-only contract test for the KGlobalAccel fallback hotkey backend.
//
// What this proves, and what it deliberately does not:
//
//   Proves, without touching the developer's desktop session:
//     * the WPF-key-name -> Qt key-code map the fallback needs: the names the
//       settings file stores map to the very Qt::Key_* values KGlobalAccel
//       expects (including the KeypadModifier of NumPad0..NumPad9), and the
//       gesture modifiers map to Qt::*Modifier bits;
//     * an unreachable session bus reports the fallback unavailable, refuses
//       both hotkeys per hotkey instead of failing the whole call, and never
//       pretends a shortcut is registered;
//     * the fallback is press-only *only* when the bus does not expose
//       globalShortcutReleased: with the release signal the seam delivers
//       record_pressed, cancel_pressed and record_released (push-to-talk
//       works); without it a release fed to the seam is ignored and the
//       composition degrades to toggle (VT-PLT-1307/1308).
//
//   Deliberately NOT exercised by ctest: a real registration. It writes into the
//   developer's kglobalshortcutsrc, so it runs only when the environment asks
//   for it (VOICETYPER_KGLOBALACCEL_LIVE=1) and releases the shortcut again
//   before returning. The live press itself belongs to the physical Arch Linux
//   gate (plan p_b5fbda6bfa1d, phase 5).

#include "domain/hotkey_gesture.hpp"
#include "domain/settings.hpp"
#include "platform/api/hotkeys.hpp"
#include "platform/linux/linux_hotkeys.hpp"
#include "platform/linux/linux_keymap.hpp"
#include "platform/linux/linux_kglobalaccel.hpp"

#include <QDBusConnection>
#include <QtGlobal>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

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

/// A bus address that cannot exist: no shortcut is ever registered in the real
/// session, which is exactly what makes this contract safe to run under ctest.
QDBusConnection isolated_bus(const QString& name)
{
    return QDBusConnection::connectToBus(QStringLiteral("unix:path=/tmp/voicetyper-no-such-bus"), name);
}

void check_qt_mapping()
{
    using namespace voicetyper::platform::linuxos;
    using domain::HotkeyModifiers;

    check(hotkey_qt_key_code("Space") == Qt::Key_Space, "Space maps to Qt::Key_Space");
    check(hotkey_qt_key_code("A") == Qt::Key_A, "A maps to Qt::Key_A");
    check(hotkey_qt_key_code("Z") == Qt::Key_Z, "Z maps to Qt::Key_Z");
    check(hotkey_qt_key_code("D0") == Qt::Key_0, "D0 maps to Qt::Key_0");
    check(hotkey_qt_key_code("D9") == Qt::Key_9, "D9 maps to Qt::Key_9");
    check(hotkey_qt_key_code("F1") == Qt::Key_F1, "F1 maps to Qt::Key_F1");
    check(hotkey_qt_key_code("F12") == Qt::Key_F12, "F12 maps to Qt::Key_F12");
    check(hotkey_qt_key_code("F24") == Qt::Key_F24, "F24 maps to Qt::Key_F24");
    check(hotkey_qt_key_code("Escape") == Qt::Key_Escape, "Escape maps to Qt::Key_Escape");
    check(hotkey_qt_key_code("Enter") == Qt::Key_Return, "Enter maps to Qt::Key_Return");
    check(hotkey_qt_key_code("KpEnter") == Qt::Key_Enter, "KpEnter maps to Qt::Key_Enter");
    check(hotkey_qt_key_code("Tab") == Qt::Key_Tab, "Tab maps to Qt::Key_Tab");
    check(hotkey_qt_key_code("Back") == Qt::Key_Backspace, "Back maps to Qt::Key_Backspace");
    check(hotkey_qt_key_code("Insert") == Qt::Key_Insert, "Insert maps to Qt::Key_Insert");
    check(hotkey_qt_key_code("Delete") == Qt::Key_Delete, "Delete maps to Qt::Key_Delete");
    check(hotkey_qt_key_code("Home") == Qt::Key_Home, "Home maps to Qt::Key_Home");
    check(hotkey_qt_key_code("End") == Qt::Key_End, "End maps to Qt::Key_End");
    check(hotkey_qt_key_code("PageUp") == Qt::Key_PageUp, "PageUp maps to Qt::Key_PageUp");
    check(hotkey_qt_key_code("PageDown") == Qt::Key_PageDown, "PageDown maps to Qt::Key_PageDown");
    check(hotkey_qt_key_code("Left") == Qt::Key_Left, "Left maps to Qt::Key_Left");
    check(hotkey_qt_key_code("Up") == Qt::Key_Up, "Up maps to Qt::Key_Up");
    check(hotkey_qt_key_code("Right") == Qt::Key_Right, "Right maps to Qt::Key_Right");
    check(hotkey_qt_key_code("Down") == Qt::Key_Down, "Down maps to Qt::Key_Down");
    check(hotkey_qt_key_code("PrintScreen") == Qt::Key_Print, "PrintScreen maps to Qt::Key_Print");
    check(hotkey_qt_key_code("Scroll") == Qt::Key_ScrollLock, "Scroll maps to Qt::Key_ScrollLock");
    check(hotkey_qt_key_code("Pause") == Qt::Key_Pause, "Pause maps to Qt::Key_Pause");
    check(hotkey_qt_key_code("CapsLock") == Qt::Key_CapsLock, "CapsLock maps to Qt::Key_CapsLock");
    check(hotkey_qt_key_code("NumLock") == Qt::Key_NumLock, "NumLock maps to Qt::Key_NumLock");

    // The keypad keys carry Qt::KeypadModifier: that is how Qt and KGlobalAccel
    // tell the numeric keypad from the digit row.
    check(hotkey_qt_key_code("NumPad0") == (Qt::Key_0 | Qt::KeypadModifier),
        "NumPad0 maps to Qt::Key_0 with the keypad modifier");
    check(hotkey_qt_key_code("NumPad9") == (Qt::Key_9 | Qt::KeypadModifier),
        "NumPad9 maps to Qt::Key_9 with the keypad modifier");

    check(hotkey_qt_key_code("OemTilde") == Qt::Key_QuoteLeft, "OemTilde maps to Qt::Key_QuoteLeft");
    check(hotkey_qt_key_code("OemQuestion") == Qt::Key_Question, "OemQuestion maps to Qt::Key_Question");
    check(hotkey_qt_key_code("OemSemicolon") == Qt::Key_Semicolon, "OemSemicolon maps to Qt::Key_Semicolon");
    check(hotkey_qt_key_code("OemQuotes") == Qt::Key_Apostrophe, "OemQuotes maps to Qt::Key_Apostrophe");
    check(hotkey_qt_key_code("OemPlus") == Qt::Key_Plus, "OemPlus maps to Qt::Key_Plus");
    check(hotkey_qt_key_code("OemMinus") == Qt::Key_Minus, "OemMinus maps to Qt::Key_Minus");
    check(hotkey_qt_key_code("OemComma") == Qt::Key_Comma, "OemComma maps to Qt::Key_Comma");
    check(hotkey_qt_key_code("OemPeriod") == Qt::Key_Period, "OemPeriod maps to Qt::Key_Period");
    check(hotkey_qt_key_code("OemOpenBrackets") == Qt::Key_BracketLeft,
        "OemOpenBrackets maps to Qt::Key_BracketLeft");
    check(hotkey_qt_key_code("OemCloseBrackets") == Qt::Key_BracketRight,
        "OemCloseBrackets maps to Qt::Key_BracketRight");
    check(hotkey_qt_key_code("OemPipe") == Qt::Key_Backslash, "OemPipe maps to Qt::Key_Backslash");

    check(hotkey_qt_key_code("NoSuchKey") == 0, "an unknown key name maps to 0");
    check(hotkey_qt_key_code("") == 0, "an empty key name maps to 0");

    check(hotkey_qt_modifier_flags(HotkeyModifiers::none) == 0, "no modifiers map to no flags");
    check(hotkey_qt_modifier_flags(HotkeyModifiers::control) == static_cast<int>(Qt::ControlModifier),
        "Ctrl maps to Qt::ControlModifier");
    check(hotkey_qt_modifier_flags(HotkeyModifiers::alt) == static_cast<int>(Qt::AltModifier),
        "Alt maps to Qt::AltModifier");
    check(hotkey_qt_modifier_flags(HotkeyModifiers::shift) == static_cast<int>(Qt::ShiftModifier),
        "Shift maps to Qt::ShiftModifier");
    check(hotkey_qt_modifier_flags(HotkeyModifiers::win) == static_cast<int>(Qt::MetaModifier),
        "Win maps to Qt::MetaModifier");
    check(hotkey_qt_modifier_flags(HotkeyModifiers::control | HotkeyModifiers::alt)
            == static_cast<int>(Qt::ControlModifier | Qt::AltModifier),
        "Ctrl+Alt maps to the combined Qt flags");
}

void check_unavailable_fallback()
{
    using namespace voicetyper::platform;
    using namespace voicetyper::platform::linuxos;

    const QDBusConnection bus = isolated_bus(QStringLiteral("voicetyper-contract-unreachable"));
    check(!bus.isConnected(), "the isolated bus is not connected");

    LinuxKGlobalAccelHotkeys hotkeys(bus);
    check(!hotkeys.available(), "an unreachable bus reports the fallback unavailable");
    check(hotkeys.capability() == HotkeyCapability::none,
        "an unavailable fallback has no capability");

    std::vector<HotkeyAction> seen;
    check(hotkeys.set_event_sink([&seen](HotkeyAction action) { seen.push_back(action); }).is_ok(),
        "the sink is installed");

    domain::AppSettings settings;
    settings.record_hotkey = "Ctrl+Alt+Space";
    settings.cancel_hotkey = "Ctrl+Alt+Escape";
    const auto report = hotkeys.apply_settings(settings);
    check(report.is_ok(), "an unavailable fallback does not fail the whole apply");
    if (report.is_ok()) {
        check(!report.value().record.empty(), "the record hotkey is reported");
        check(!report.value().cancel.empty(), "the cancel hotkey is reported");
        check(!report.value().all_registered(), "nothing is registered without kglobalaccel");
        check(!report.value().errors().empty(), "each refused hotkey carries its reason");
    }
    check(hotkeys.record_key_code() == 0, "no record key code without kglobalaccel");

    check(hotkeys.unregister_all().is_ok(), "unregister_all succeeds");
    check(hotkeys.unregister_all().is_ok(), "unregister_all is idempotent");
    check(seen.empty(), "an unavailable fallback delivers no edge");
}

void check_press_only_edges()
{
    using namespace voicetyper::platform;
    using namespace voicetyper::platform::linuxos;

    const QDBusConnection bus = isolated_bus(QStringLiteral("voicetyper-contract-seam"));
    LinuxKGlobalAccelHotkeys hotkeys(bus);

    std::vector<HotkeyAction> seen;
    check(hotkeys.set_event_sink([&seen](HotkeyAction action) { seen.push_back(action); }).is_ok(),
        "the seam sink is installed");

    // This is what the org.kde.kglobalaccel.Component.globalShortcutPressed
    // signal does; the seam feeds it without a session bus.
    hotkeys.handle_shortcut_pressed("record");
    hotkeys.handle_shortcut_pressed("cancel");
    hotkeys.handle_shortcut_pressed("something-else");

    check(seen.size() == 2, "only the two known actions reach the sink");
    check(!seen.empty() && seen[0] == HotkeyAction::record_pressed, "record maps to record_pressed");
    check(seen.size() > 1 && seen[1] == HotkeyAction::cancel_pressed, "cancel maps to cancel_pressed");

    // The degradation branch: an old kglobalaccel that has no release signal.
    // A release fed to the seam must be ignored, or push-to-talk would hang.
    hotkeys.set_release_signal_available_for_test(false);
    hotkeys.handle_shortcut_released("record");
    check(seen.size() == 2, "a press-only fallback ignores the release signal");

    // With globalShortcutReleased on the bus the release edge is delivered,
    // which is exactly what push-to-talk needs.
    hotkeys.set_release_signal_available_for_test(true);
    hotkeys.handle_shortcut_released("record");
    check(seen.size() == 3 && seen.back() == HotkeyAction::record_released,
        "with the release signal the release edge reaches the sink");
    hotkeys.handle_shortcut_released("cancel");
    check(seen.size() == 3, "a release of the cancel action is not a record release");
}

void check_release_model()
{
    using namespace voicetyper::platform::linuxos;

    check(std::string_view(hotkey_capability_name(HotkeyCapability::kglobal_accel_press_only))
            == "kglobal_accel_press_only",
        "the press-only KGlobalAccel capability is named");

    // Reading the capability registers nothing, so this is safe to run in the
    // developer's session; without kglobalaccel the branch is skipped.
    LinuxKGlobalAccelHotkeys live;
    if (!live.available()) {
        std::cout << "note: org.kde.kglobalaccel is not on this session bus; "
                     "the release-signal model is not checked\n";
        return;
    }
    const HotkeyCapability observed = live.capability();
    check(observed == HotkeyCapability::kglobal_accel
            || observed == HotkeyCapability::kglobal_accel_press_only,
        "a reachable kglobalaccel reports a KGlobalAccel capability");

    live.set_release_signal_available_for_test(false);
    check(live.capability() == HotkeyCapability::kglobal_accel_press_only,
        "without the release signal the capability is press-only");
    live.set_release_signal_available_for_test(true);
    check(live.capability() == HotkeyCapability::kglobal_accel,
        "with the release signal the capability is press and release");
}

void check_live_registration()
{
    using namespace voicetyper::platform;
    using namespace voicetyper::platform::linuxos;

    // Opt-in: registering a shortcut writes into kglobalshortcutsrc, so ctest
    // never does it. Run with VOICETYPER_KGLOBALACCEL_LIVE=1 to exercise the
    // real session; the registration is released before returning.
    if (qEnvironmentVariableIsEmpty("VOICETYPER_KGLOBALACCEL_LIVE")) {
        std::cout << "note: live KGlobalAccel registration skipped "
                     "(set VOICETYPER_KGLOBALACCEL_LIVE=1)\n";
        return;
    }

    LinuxKGlobalAccelHotkeys hotkeys;
    if (!hotkeys.available()) {
        std::cout << "note: org.kde.kglobalaccel is not on this session bus\n";
        return;
    }
    check(hotkeys.capability() == HotkeyCapability::kglobal_accel,
        "a reachable kglobalaccel with the release signal reports press and release");

    domain::AppSettings settings;
    // Safe combinations on purpose: Ctrl+Alt+Space belongs to plasma-keyboard on
    // this machine and Ctrl+Alt+F* switches the virtual console, which hangs the
    // Wayland session (both hit during the first live check, 2026-10-11).
    settings.record_hotkey = "Ctrl+Alt+J";
    settings.cancel_hotkey = "Ctrl+Alt+K";
    const auto report = hotkeys.apply_settings(settings);
    check(report.is_ok(), "the live registration does not fail as a whole");
    if (report.is_ok()) {
        check(report.value().all_registered(), "both hotkeys register through kglobalaccel");
        check(hotkeys.record_key_code() != 0, "the live registration reports the record key code");
    }
    check(hotkeys.unregister_all().is_ok(), "the live registration is released");
}

} // namespace

int main()
{
    check_qt_mapping();
    check_unavailable_fallback();
    check_press_only_edges();
    check_release_model();
    check_live_registration();

    if (failures != 0) {
        std::cerr << "linux-kglobalaccel-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "linux-kglobalaccel-contract: OK\n";
    return 0;
}
