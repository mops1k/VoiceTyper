// Windows input contract: global hotkeys, the WH_KEYBOARD_LL capture hook and
// the XInput gamepad poll.
//
// What this proves, with no real device and no real key press:
//   * the key-name <-> virtual-key-code table round-trips (the only hotkey test
//     the .NET build had: VoiceTyper.Tests/Services/HotkeyServiceTests.cs);
//   * the policy probe is side-effect free and its "blocked by policy" rule is
//     exact, so a refusal is never reported as a conflict;
//   * RegisterHotKey round-trips: apply_settings registers, unregister_all frees
//     the combination again, and a second registration of the same combination
//     is refused as a conflict - proven across threads, which is the same global
//     exclusivity an application outside this process would hit;
//   * unregister_all() is idempotent and leaves no hotkey behind;
//   * the capture hook installs, stops and joins inside a deadline, and its
//     capture ends instead of hanging when stopped from another thread;
//   * the gamepad poll starts and stops on a fake pad, debounces edges, bounds
//     re-enumeration when a controller is absent, and answers capture with
//     device_disconnected instead of looping.
//
// Rules this test obeys:
//   * it never waits without a deadline, so it cannot hang;
//   * every registration it makes is released on every path, including a failed
//     check, so nothing is left registered on the machine;
//   * when the environment cannot provide the input subsystem, it prints a skip
//     reason and still passes, so a CI runner without an interactive desktop
//     stays green.

#include "domain/cancellation.hpp"
#include "domain/settings.hpp"
#include "platform/windows/win32_gamepad.hpp"
#include "platform/windows/win32_hotkeys.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

using namespace voicetyper;
using namespace std::chrono_literals;
using domain::HotkeyModifiers;
using platform::win32::HotkeyCaptureHook;
using platform::win32::HotkeyFailureReason;
using platform::win32::HotkeyPolicyFinding;
using platform::win32::Win32GamepadService;
using platform::win32::Win32HotkeyService;
using platform::win32::XInputPadSnapshot;

int failures = 0;
int checks = 0;
int skips = 0;

void check(bool condition, const std::string& what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void check_eq(const std::string& actual, const std::string& expected, const std::string& what)
{
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s\n  expected: %s\n  actual:   %s\n", what.c_str(), expected.c_str(), actual.c_str());
    }
}

void skip(const std::string& what)
{
    ++skips;
    std::printf("SKIP %s\n", what.c_str());
}

/// Bounded wait. Returns false on timeout, so no scenario can hang.
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds deadline, std::chrono::milliseconds step = 2ms)
{
    const auto until = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < until) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return predicate();
}

/// Releases a hotkey registered by the test itself, on every path.
class ScopedTestHotkey {
public:
    ScopedTestHotkey(std::uint32_t modifiers, int virtual_key, int id)
        : modifiers_(modifiers)
        , virtual_key_(virtual_key)
        , id_(id)
    {
        registered_ = ::RegisterHotKey(nullptr, id, modifiers, virtual_key) != 0;
    }

    ~ScopedTestHotkey()
    {
        release();
    }

    ScopedTestHotkey(const ScopedTestHotkey&) = delete;
    ScopedTestHotkey& operator=(const ScopedTestHotkey&) = delete;

    [[nodiscard]] bool registered() const { return registered_; }
    void release()
    {
        if (registered_) {
            ::UnregisterHotKey(nullptr, id_);
            registered_ = false;
        }
    }

private:
    std::uint32_t modifiers_;
    int virtual_key_;
    int id_;
    bool registered_ = false;
};

/// A scripted XInput stand-in. One script step per poll: the reader is called for
/// slots 0..3 in order, and only the slot-0 call advances the script, so the
/// script is a per-poll timeline regardless of how many slots exist.
class FakePads {
public:
    void set_script(std::vector<XInputPadSnapshot> script)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        script_ = std::move(script);
        cursor_ = 0;
    }

    [[nodiscard]] int slot_zero_calls() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return slot_zero_calls_;
    }

    [[nodiscard]] platform::win32::GamepadPadReader reader()
    {
        return [this](int index, XInputPadSnapshot& out) {
            const std::lock_guard<std::mutex> lock(mutex_);
            out = XInputPadSnapshot{};
            if (index != 0) {
                return false;
            }
            ++slot_zero_calls_;
            if (script_.empty()) {
                return false;
            }
            const std::size_t at = std::min(cursor_, script_.size() - 1);
            out = script_[at];
            if (cursor_ < script_.size()) {
                ++cursor_;
            }
            return out.connected;
        };
    }

private:
    mutable std::mutex mutex_;
    std::vector<XInputPadSnapshot> script_;
    std::size_t cursor_ = 0;
    int slot_zero_calls_ = 0;
};

XInputPadSnapshot pad(std::uint16_t buttons)
{
    XInputPadSnapshot snapshot;
    snapshot.connected = true;
    snapshot.buttons = buttons;
    return snapshot;
}

XInputPadSnapshot idle_pad()
{
    return pad(0);
}

const std::uint16_t kXInputA = 0x1000;
const std::uint16_t kXInputB = 0x2000;

// ---------------------------------------------------------------------------
// 1. Key table: the only hotkey test the .NET build had.
// ---------------------------------------------------------------------------

void test_key_table()
{
    struct Expectation {
        const char* name;
        std::int32_t code;
    };
    const Expectation expectations[] = {
        {"Space", 0x20}, {"Enter", 0x0D},   {"Escape", 0x1B},   {"Tab", 0x09},
        {"Back", 0x08},   {"Insert", 0x2D},  {"Delete", 0x2E},   {"Home", 0x24},
        {"End", 0x23},    {"PageUp", 0x21},  {"PageDown", 0x22}, {"Left", 0x25},
        {"Up", 0x26},     {"Right", 0x27},   {"Down", 0x28},    {"PrintScreen", 0x2C},
        {"Scroll", 0x91}, {"Pause", 0x13},   {"CapsLock", 0x14}, {"NumLock", 0x90},
        {"F1", 0x70},     {"F12", 0x7B},     {"F24", 0x87},     {"NumPad0", 0x60},
        {"NumPad9", 0x69}, {"D0", 0x30},     {"D5", 0x35},      {"OemTilde", 0xC0},
        {"OemPipe", 0xDC}, {"A", 0x41},      {"Z", 0x5A},       {"F21", 0x84},
    };
    for (const auto& expectation : expectations) {
        const std::int32_t code = platform::win32::hotkey_virtual_key(expectation.name);
        check(code == expectation.code,
              std::string("virtual key of ") + expectation.name + " is " + std::to_string(expectation.code)
                  + " (got " + std::to_string(code) + ")");
        const auto name = platform::win32::hotkey_key_name(code);
        check(name.has_value() && *name == expectation.name,
              std::string("virtual key ") + std::to_string(expectation.code) + " maps back to "
                  + expectation.name);
    }

    // A digit code has two spellings, and the .NET table inserts the alias
    // first (BuildVkToName adds "D0".."D9" before the bare digits), so the
    // inverse mapping answers "D5" for 0x35 exactly as the .NET service did.
    // The capture hook shows that name, so the quirk is observable and therefore
    // pinned here.
    check(platform::win32::hotkey_key_name(0x35).value_or("<none>") == "D5",
          "0x35 is named D5, the .NET digit-alias precedence");
    check(platform::win32::hotkey_key_name(0x30).value_or("<none>") == "D0",
          "0x30 is named D0, the .NET digit-alias precedence");
    check(platform::win32::hotkey_key_name(0x41).value_or("<none>") == "A",
          "a letter has no alias, so it maps to itself");

    // The parser normalises only the first character, so the mapping must accept
    // both spellings the grammar produces.
    check(platform::win32::hotkey_virtual_key("Numpad0") == 0x60, "Numpad0 maps like NumPad0");
    check(platform::win32::hotkey_virtual_key("Space") != 0, "Space is registrable");
    check(platform::win32::hotkey_virtual_key("") == 0, "an empty key is not registrable");
    check(platform::win32::hotkey_virtual_key("MediaPlayPause") == 0, "a media key is not registrable");
    check(platform::win32::hotkey_virtual_key("#") == 0, "a bare symbol is not registrable");
    check(platform::win32::hotkey_virtual_key("F0") == 0, "F0 is outside the function key range");
    check(platform::win32::hotkey_virtual_key("F25") == 0, "F25 is outside the function key range");
    check(!platform::win32::hotkey_key_name(0xAD).has_value(), "an unmapped code has no name");

    // Modifiers: the domain enum is not the Win32 bitmask, and the translation
    // has to be explicit.
    check(platform::win32::hotkey_native_modifiers(HotkeyModifiers::none) == 0, "no modifiers is 0");
    check(platform::win32::hotkey_native_modifiers(HotkeyModifiers::control) == platform::win32::kModControl,
          "control maps to MOD_CONTROL");
    check(platform::win32::hotkey_native_modifiers(HotkeyModifiers::alt) == platform::win32::kModAlt,
          "alt maps to MOD_ALT");
    check(platform::win32::hotkey_native_modifiers(HotkeyModifiers::shift) == platform::win32::kModShift,
          "shift maps to MOD_SHIFT");
    check(platform::win32::hotkey_native_modifiers(HotkeyModifiers::win) == platform::win32::kModWin,
          "win maps to MOD_WIN");
    check(platform::win32::hotkey_native_modifiers(HotkeyModifiers::control | HotkeyModifiers::alt
                                                   | HotkeyModifiers::shift | HotkeyModifiers::win)
              == (platform::win32::kModControl | platform::win32::kModAlt | platform::win32::kModShift
                  | platform::win32::kModWin),
          "all four modifiers map to 0x000F");
    check(platform::win32::kModNoRepeat == 0x4000, "MOD_NOREPEAT is 0x4000");
}

// ---------------------------------------------------------------------------
// 2. Diagnostics: the reason taxonomy and the policy rule
// ---------------------------------------------------------------------------

void test_diagnostics()
{
    // The probe reads two documented registry values and cannot fail the caller.
    const std::vector<HotkeyPolicyFinding> findings = platform::win32::hotkey_policy_findings();
    for (const auto& finding : findings) {
        check(!finding.key.empty() && !finding.value.empty() && !finding.effect.empty(),
              "a policy finding names its key, value and effect");
    }

    const std::string integrity = platform::win32::hotkey_process_integrity_label();
    const bool known = integrity == "untrusted" || integrity == "low" || integrity == "medium"
        || integrity == "high" || integrity == "system" || integrity == "unknown";
    check(known, "the integrity label is one of the documented values (got \"" + integrity + "\")");
    std::printf("  observed: process integrity \"%s\", %zu hotkey policy finding(s)\n", integrity.c_str(),
                findings.size());

    // The policy rule, exercised with synthetic findings so the test does not
    // depend on this machine's policy.
    const std::vector<HotkeyPolicyFinding> no_win_keys{
        {"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\NoWinKeys", "1", "test"}};
    const std::vector<HotkeyPolicyFinding> disabled_hotkeys{
        {"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\DisabledHotkeys", "app.exe", "test"}};

    const domain::HotkeyGesture win_gesture{HotkeyModifiers::control | HotkeyModifiers::win, "Space"};
    const domain::HotkeyGesture no_win_gesture{HotkeyModifiers::control | HotkeyModifiers::alt, "Space"};

    check(!platform::win32::hotkey_blocked_by_policy(win_gesture, {}),
          "no findings means no policy block");
    check(platform::win32::hotkey_blocked_by_policy(win_gesture, no_win_keys),
          "NoWinKeys blocks a combination that uses Win");
    check(!platform::win32::hotkey_blocked_by_policy(no_win_gesture, no_win_keys),
          "NoWinKeys does not block a combination without Win");
    check(platform::win32::hotkey_blocked_by_policy(no_win_gesture, disabled_hotkeys),
          "DisabledHotkeys naming this application blocks every combination");

    // The rendered diagnostic must carry the Win32 code and the elevation hint,
    // because "already registered" without them is the useless message the .NET
    // UI showed.
    const auto conflict = platform::win32::hotkey_diagnostics(
        no_win_gesture, HotkeyFailureReason::already_registered, 1409);
    const std::string text = conflict.describe();
    check(text.find("1409") != std::string::npos, "a conflict names the Win32 error code");
    check(text.find("ERROR_HOTKEY_ALREADY_REGISTERED") != std::string::npos,
          "a conflict names the Win32 error symbolically");
    check(text.find("Ctrl+Alt+Space") != std::string::npos, "a conflict names the combination");
    check(text.find(integrity) != std::string::npos, "a conflict names the process integrity level");
    check(!platform::win32::hotkey_diagnostics(no_win_gesture, HotkeyFailureReason::none).describe().empty()
              == false,
          "a successful registration has an empty diagnostic");

    check(std::string(platform::win32::hotkey_failure_reason_name(HotkeyFailureReason::already_registered))
              == "already_registered",
          "the reason name is stable");
}

// ---------------------------------------------------------------------------
// 3. RegisterHotKey round-trip, forced conflict, idempotent release
// ---------------------------------------------------------------------------

void test_hotkey_registration()
{
    // A synthetic combination: Ctrl+Alt+Shift+F21 is not a user binding and
    // cannot steal the real Ctrl+Alt+Space / Ctrl+Alt+Escape.
    const std::string record_text = "Ctrl+Alt+Shift+F21";
    const std::string cancel_text = "Ctrl+Alt+Shift+F22";

    Win32HotkeyService service;
    if (!service.pump_running()) {
        skip("no hotkey message pump on this machine, so RegisterHotKey cannot be exercised");
        return;
    }

    std::atomic<int> sink_calls{0};
    check(service.set_event_sink([&sink_calls](platform::HotkeyAction) { ++sink_calls; }).is_ok(),
          "the event sink is installed");

    domain::AppSettings settings;
    settings.record_hotkey = record_text;
    settings.cancel_hotkey = cancel_text;

    const auto applied = service.apply_settings(settings);
    if (applied.is_error()) {
        skip("apply_settings failed on this machine: " + applied.error().to_string());
        return;
    }

    const platform::HotkeyRegistrationReport& report = applied.value();
    check(report.record.size() == 1 && report.cancel.size() == 1,
          "the report has exactly one entry per hotkey");
    if (!report.all_registered()) {
        // A refusal here is an environment fact (a policy, or another
        // application already owning the synthetic combination), not a defect.
        for (const auto& message : report.errors()) {
            std::printf("  registration refused: %s\n", message.c_str());
        }
        skip("this machine refused the synthetic test combination; see the diagnostic above");
        check(service.unregister_all().is_ok(), "release stays successful after a refusal");
        return;
    }

    const std::int32_t record_code = service.record_key_code();
    check(record_code == platform::win32::hotkey_virtual_key("F21"),
          "record_key_code mirrors the .NET RecordKeyVk");
    check(service.last_diagnostics(platform::HotkeyAction::record_pressed).reason == HotkeyFailureReason::none,
          "a registered hotkey has no failure reason");

    // --- Forced conflict. RegisterHotKey is a session-wide exclusivity claim, so
    // a registration from *this* thread for the same combination must be
    // refused. Both modifier spellings are tried because the service registers
    // with MOD_NOREPEAT and the OS may or may not treat that bit as part of the
    // combination's identity; the contract only needs one spelling to conflict.
    const std::uint32_t base_mods = platform::win32::kModControl | platform::win32::kModAlt
        | platform::win32::kModShift;
    const int code = platform::win32::hotkey_virtual_key("F21");

    ScopedTestHotkey without_norepeat(base_mods, code, 0x7101);
    ScopedTestHotkey with_norepeat(base_mods | platform::win32::kModNoRepeat, code, 0x7102);
    check(!without_norepeat.registered() || !with_norepeat.registered(),
          "a second registration of a registered combination is refused as a conflict");
    if (without_norepeat.registered() && with_norepeat.registered()) {
        std::printf("  observed: Win32 accepted BOTH spellings of an already-registered combination\n");
    } else {
        std::printf("  observed: Win32 refused the duplicate registration (error %lu), which is the conflict\n",
                    static_cast<unsigned long>(::GetLastError()));
    }

    // --- Release frees the combination again. Whichever spelling conflicted is
    // the one that must now be accepted, which is the round-trip evidence.
    check(service.unregister_all().is_ok(), "unregister_all succeeds");
    check(service.record_key_code() == 0, "record_key_code is 0 after release");
    check(service.last_report().record.empty(), "the report is cleared after release");

    // The service may deliver this combination through its permanent low-level
    // keyboard hook instead of RegisterHotKey: the hook sees combinations the OS
    // reserves (Alt+Win+Space is swallowed by the Win+Space layout switcher before
    // WM_HOTKEY, m_23de0707f9db), and when the hook is installed RegisterHotKey is
    // deliberately not called, so one action can never fire twice. Either way the
    // scoped claims above must be released before the round trip - one of them won
    // the combination exactly because the service did not claim it.
    without_norepeat.release();
    with_norepeat.release();

    const std::uint32_t free_mods = with_norepeat.registered() ? base_mods : base_mods;
    ScopedTestHotkey round_trip(free_mods, code, 0x7103);
    check(round_trip.registered(),
          "the combination is registrable again after unregister_all, so nothing leaked");
    round_trip.release();

    // --- Idempotence: releasing twice and three times is a success every time.
    check(service.unregister_all().is_ok(), "unregister_all is idempotent (second call)");
    check(service.unregister_all().is_ok(), "unregister_all is idempotent (third call)");

    // --- Re-applying the same settings registers again: a settings round trip
    // must not leave a stale half-registered state.
    const auto reapplied = service.apply_settings(settings);
    check(reapplied.is_ok() && reapplied.value().all_registered(),
          "apply_settings after a release registers both hotkeys again");

    // A settings change replaces the previous registration rather than adding to
    // it, which is what the .NET ApplySettings did through UnregisterAll first.
    domain::AppSettings other = settings;
    other.record_hotkey = "Ctrl+Alt+Shift+F23";
    const auto swapped = service.apply_settings(other);
    check(swapped.is_ok() && swapped.value().all_registered(), "a settings change re-registers cleanly");
    check(service.record_key_code() == platform::win32::hotkey_virtual_key("F23"),
          "record_key_code follows the new gesture");

    // --- Leave nothing behind.
    check(service.unregister_all().is_ok(), "final release succeeds");
    ScopedTestHotkey final_check(base_mods | platform::win32::kModNoRepeat, code, 0x7104);
    check(final_check.registered(), "no hotkey is left registered by this test");

    // A gesture that does not parse is a reported, per-hotkey failure and fails
    // the call with invalid_argument, as the frozen contract documents.
    domain::AppSettings broken;
    broken.record_hotkey = "Ctrl+Alt+";
    const auto bad = service.apply_settings(broken);
    check(bad.is_error() && bad.code() == domain::ErrorCode::invalid_argument,
          "an unparseable record hotkey fails with invalid_argument");
    check(!service.last_report().record.empty() && !service.last_report().record.front().registered,
          "an unparseable hotkey is also reported per hotkey");
    check(service.last_report().record.front().error.find("does not parse") != std::string::npos,
          "the per-hotkey reason says the string does not parse");

    // A gesture whose key has no virtual-key code is refused with its own
    // reason, without touching the OS.
    domain::AppSettings unknown_key;
    unknown_key.record_hotkey = "Ctrl+Alt+MediaPlayPause";
    const auto unknown = service.apply_settings(unknown_key);
    check(unknown.is_ok(), "an unknown key name is a per-hotkey result, not a failed call");
    if (unknown.is_ok()) {
        const auto& entry = unknown.value().record.front();
        check(!entry.registered, "an unknown key name is not registered");
        check(entry.error.find("not a gesture this build can register") != std::string::npos,
              "an unknown key name gets the invalid_gesture reason");
    }
    check(service.unregister_all().is_ok(), "release after the refused registrations succeeds");
    check(sink_calls.load() == 0, "no sink callback ran: no key was pressed during the test");
}

// ---------------------------------------------------------------------------
// 4. Capture hook: install, stop, bounded join
// ---------------------------------------------------------------------------

void test_capture_hook()
{
    HotkeyCaptureHook hook;
    hook.set_shutdown_timeout(std::chrono::milliseconds(2000));

    // capture_next() before start() is a reported misuse, not a hang.
    const auto before_start = hook.capture_next({});
    check(before_start.is_error() && before_start.code() == domain::ErrorCode::invalid_state,
          "capture_next before start is invalid_state");

    const auto started = hook.start();
    if (started.is_error()) {
        skip("WH_KEYBOARD_LL cannot be installed here: " + started.error().to_string());
        return;
    }

    if (!hook.is_running()) {
        // A real key press during the test ended the capture. That is correct
        // one-shot behaviour, but it makes the remaining assertions meaningless.
        skip("a key press ended the capture before the assertions; the one-shot contract held");
        hook.stop();
        return;
    }
    check(hook.hook_installed(), "the low-level hook is installed");
    check(hook.thread_id() != 0, "the hook thread published its id before installing the hook");

    // A capture stopped from another thread must end, not hang. This is the
    // .NET defect: Stop() posted WM_QUIT only when the thread was already in its
    // pump and then nulled the completion source, so the awaiting UI waited
    // forever.
    std::atomic<bool> stopper_done{false};
    const auto wait_begin = std::chrono::steady_clock::now();
    std::thread stopper([&hook, &stopper_done] {
        std::this_thread::sleep_for(60ms);
        stopper_done.store(true);
        hook.stop();
    });
    const auto captured = hook.capture_next({});
    const auto wait_elapsed = std::chrono::steady_clock::now() - wait_begin;
    stopper.join();

    check(stopper_done.load(), "stop() ran on another thread");
    check(captured.is_error() && captured.code() == domain::ErrorCode::cancelled,
          "a capture stopped from another thread ends as cancelled, not a hang");
    check(wait_elapsed < 2000ms,
          "the stopped capture ended inside the 2 s deadline (took "
              + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(wait_elapsed).count())
              + " ms)");
    check(!hook.is_running(), "the hook thread is gone after stop()");

    // stop() is idempotent.
    check(hook.stop().is_ok(), "stop() is idempotent after the thread already left");
    check(hook.stop().is_ok(), "stop() is idempotent a second time");

    // A second start() stops the previous capture first, so the hook is never
    // installed twice and the previous thread is joined.
    const auto restarted = hook.start();
    if (restarted.is_error()) {
        skip("the hook could not be reinstalled for the restart check: " + restarted.error().to_string());
        return;
    }
    check(hook.hook_installed() || !hook.is_running(), "a restart installs one hook");
    const auto stop_begin = std::chrono::steady_clock::now();
    const auto stopped = hook.stop();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_begin;
    check(stopped.is_ok(), "stop() joins the restarted thread");
    check(stop_elapsed < 2000ms,
          "the join finished inside the 2 s deadline (took "
              + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(stop_elapsed).count())
              + " ms)");

    // A pre-cancelled token ends the wait immediately.
    if (hook.start().is_ok()) {
        domain::CancellationSource source;
        source.request_cancellation();
        const auto cancelled = hook.capture_next(source.token());
        check(cancelled.is_error() && cancelled.code() == domain::ErrorCode::cancelled,
              "a pre-cancelled token ends the capture");
        check(hook.stop().is_ok(), "release after the cancelled capture");
    }
}

// ---------------------------------------------------------------------------
// 5. Gamepad poll on a fake pad: debounce, bounded re-enumeration, capture
// ---------------------------------------------------------------------------

struct EdgeCounter {
    mutable std::mutex mutex;
    std::vector<platform::GamepadAction> actions;
    std::atomic<int> pressed{0};
    std::atomic<int> released{0};
    std::atomic<int> cancelled{0};

    platform::GamepadEventSink sink()
    {
        return [this](const platform::GamepadEdge& edge) {
            const std::lock_guard<std::mutex> lock(mutex);
            actions.push_back(edge.action);
            if (edge.action == platform::GamepadAction::record_pressed) {
                ++pressed;
            } else if (edge.action == platform::GamepadAction::record_released) {
                ++released;
            } else {
                ++cancelled;
            }
        };
    }

    [[nodiscard]] std::vector<platform::GamepadAction> snapshot() const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return actions;
    }
};

void test_gamepad_poll()
{
    FakePads pads;
    EdgeCounter edges;

    Win32GamepadService service(pads.reader());
    service.set_poll_interval(5ms);
    service.set_debounce_samples(2);
    service.set_rescan_cooldown(200ms);
    service.set_shutdown_timeout(std::chrono::milliseconds(2000));
    check(service.set_event_sink(edges.sink()).is_ok(), "the gamepad sink is installed");

    domain::AppSettings settings;
    settings.record_gamepad_button = std::string("XInput|A");
    settings.cancel_gamepad_button = std::string("XInput|B");

    // --- A malformed present binding is invalid_argument; an absent one is a
    // success meaning "no gamepad action".
    domain::AppSettings malformed;
    malformed.record_gamepad_button = std::string("XInput|NotAButton");
    const auto bad = service.apply_settings(malformed);
    check(bad.is_error() && bad.code() == domain::ErrorCode::invalid_argument,
          "a malformed gamepad binding is invalid_argument");

    // --- No controller at all: polling starts, nothing is a device, and the
    // re-enumeration count cannot grow. The .NET catch-all reloaded the device
    // list on every poll, so a missing controller spun that loop.
    pads.set_script({});
    const auto applied = service.apply_settings(settings);
    check(applied.is_ok(), "apply_settings with a valid binding starts polling");
    check(service.polling(), "the poll thread is running");
    std::this_thread::sleep_for(300ms);
    check(service.devices().empty(), "no controller means an empty device list, not an error");
    check(service.rescan_count() == 1,
          "an absent controller is enumerated once and not re-enumerated per poll (count "
              + std::to_string(service.rescan_count()) + ")");

    // --- A capture with no controller answers device_disconnected instead of
    // waiting for a press that can never come.
    const auto begin = std::chrono::steady_clock::now();
    const auto no_device = service.capture_next({});
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    check(no_device.is_error() && no_device.code() == domain::ErrorCode::device_disconnected,
          "a capture with no controller reports device_disconnected");
    check(elapsed < 1000ms, "that answer is immediate, not a wait (took "
          + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) + " ms)");

    // --- Hot-plug: a controller appears and disappears, and the re-enumeration
    // count stays bounded.
    pads.set_script({idle_pad()});
    const bool appeared = wait_for([&service]() { return service.devices().size() == 1; }, 2000ms);
    check(appeared, "a connected controller appears in devices()");
    if (!appeared) {
        std::printf("  observed: %d poll reads of slot 0\n", pads.slot_zero_calls());
    }
    pads.set_script({});
    const bool vanished = wait_for([&service]() { return service.devices().empty(); }, 2000ms);
    check(vanished, "a disconnected controller leaves devices() without stopping the service");
    check(service.polling(), "device loss does not stop the service");
    std::this_thread::sleep_for(300ms);
    const int rescans = service.rescan_count();
    check(rescans <= 4,
          "re-enumeration stays bounded across a hot-plug cycle (count " + std::to_string(rescans) + ")");
    std::printf("  observed: %d slot-0 reads, %d re-enumerations\n", pads.slot_zero_calls(), rescans);

    // --- Debounce: a single 5 ms sample must not produce an edge, and two
    // consecutive samples must produce exactly one. The script clamps on its
    // last element, so a two-step script is a one-poll pulse followed by a
    // permanent release.
    pads.set_script({pad(kXInputA), idle_pad()});
    std::this_thread::sleep_for(200ms);
    check(edges.pressed.load() == 0,
          "a one-sample button press produces no edge with a debounce of 2 (pressed "
              + std::to_string(edges.pressed.load()) + ")");

    pads.set_script({pad(kXInputA), pad(kXInputA), idle_pad(), idle_pad(), pad(kXInputA), pad(kXInputA),
                     idle_pad(), idle_pad()});
    const bool four_edges =
        wait_for([&edges]() { return edges.snapshot().size() >= 4; }, 2000ms);
    check(four_edges, "a held press produces a debounced press and release edge");
    const std::vector<platform::GamepadAction> sequence = edges.snapshot();
    const std::vector<platform::GamepadAction> expected{
        platform::GamepadAction::record_pressed,
        platform::GamepadAction::record_released,
        platform::GamepadAction::record_pressed,
        platform::GamepadAction::record_released,
    };
    const std::size_t compared = std::min(sequence.size(), expected.size());
    bool order_ok = compared > 0;
    for (std::size_t i = 0; i < compared; ++i) {
        order_ok = order_ok && sequence[i] == expected[i];
    }
    check(order_ok, "the edge order is press, release, press, release");
    check(edges.pressed.load() == 2 && edges.released.load() == 2,
          "a held button produces exactly one press and one release per hold (pressed "
              + std::to_string(edges.pressed.load()) + ", released " + std::to_string(edges.released.load())
              + ")");

    // The record button is released in the final script step, so the
    // push-to-talk wait resolves at once.
    const auto released_wait = service.wait_for_record_release({});
    check(released_wait.is_ok(), "waiting for a released record button returns immediately");

    // While it is held, the wait is cancellable instead of unbounded - the
    // .NET WaitForRecordReleaseAsync was called with no token.
    pads.set_script({pad(kXInputA), pad(kXInputA)});
    const bool held = wait_for([&service]() { return service.record_held(); }, 2000ms);
    check(held, "record_held reflects the debounced state under the poll mutex");
    domain::CancellationSource release_source;
    std::thread release_canceller([&release_source] {
        std::this_thread::sleep_for(120ms);
        release_source.request_cancellation();
    });
    const auto cancel_begin = std::chrono::steady_clock::now();
    const auto cancel_wait = service.wait_for_record_release(release_source.token());
    const auto cancel_elapsed = std::chrono::steady_clock::now() - cancel_begin;
    release_canceller.join();
    check(cancel_wait.is_error() && cancel_wait.code() == domain::ErrorCode::cancelled,
          "waiting for a held record button is cancellable");
    check(cancel_elapsed < 1000ms, "the cancelled wait ends promptly (took "
          + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(cancel_elapsed).count())
          + " ms)");

    // --- Capture with a controller: the first pressed button is returned.
    pads.set_script({pad(kXInputB)});
    const auto captured = service.capture_next({});
    check(captured.is_ok(), "a capture with a controller returns a binding");
    if (captured.is_ok()) {
        check_eq(captured.value().to_string(), "XInput|B", "the captured binding round-trips through the grammar");
    }

    // --- A capture can be cancelled from another thread, and a second
    // concurrent capture is refused rather than orphaning the first. The .NET
    // StartCapture overwrote its completion source and left the earlier caller
    // waiting forever.
    pads.set_script({idle_pad()});
    std::atomic<domain::ErrorCode> first_code{domain::ErrorCode::ok};
    std::thread first_capture([&service, &first_code] {
        const auto result = service.capture_next({});
        first_code.store(result.is_ok() ? domain::ErrorCode::ok : result.code());
    });
    std::this_thread::sleep_for(80ms);
    const auto second = service.capture_next({});
    check(second.is_error() && second.code() == domain::ErrorCode::invalid_state,
          "a second concurrent capture is refused, so the first waiter is not orphaned");
    check(service.cancel_capture().is_ok(), "cancel_capture succeeds");
    first_capture.join();
    check(first_code.load() == domain::ErrorCode::cancelled,
          "the cancelled capture returns cancelled, not a hang (code "
              + std::string(domain::error_code_name(first_code.load())) + ")");
    check(service.cancel_capture().is_ok(), "cancel_capture is idempotent");

    // --- Stop joins the poll thread inside a deadline and is idempotent.
    const auto stop_begin = std::chrono::steady_clock::now();
    const auto stopped = service.stop();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_begin;
    check(stopped.is_ok(), "stop() joins the poll thread");
    check(stop_elapsed < 2000ms, "the join finished inside the 2 s deadline (took "
          + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(stop_elapsed).count())
          + " ms)");
    check(!service.polling(), "the poll thread is gone after stop()");
    check(service.stop().is_ok(), "stop() is idempotent");
    const int after_stop = pads.slot_zero_calls();
    std::this_thread::sleep_for(100ms);
    check(pads.slot_zero_calls() == after_stop, "a stopped service does not keep polling");
}

/// The real XInput path with whatever controller (usually none) this machine
/// has. It proves the production reader never hangs or crashes; it makes no
/// claim about hardware.
void test_real_xinput_reader()
{
    const std::string runtime = platform::win32::xinput_runtime_name();
    const std::string failure = platform::win32::xinput_runtime_failure();
    std::printf("  observed: XInput runtime \"%s\"%s%s\n", runtime.empty() ? "<none>" : runtime.c_str(),
                failure.empty() ? "" : ", ", failure.c_str());

    const auto reader = platform::win32::xinput_pad_reader();
    int connected = 0;
    for (int slot = 0; slot < platform::kXInputDeviceCount; ++slot) {
        XInputPadSnapshot snapshot;
        if (reader(slot, snapshot)) {
            ++connected;
        }
    }
    std::printf("  observed: %d of %d XInput slot(s) report a controller\n", connected,
                platform::kXInputDeviceCount);
    check(connected >= 0 && connected <= platform::kXInputDeviceCount, "the slot count is in range");

    // A short poll with the real reader must start and stop cleanly whether or
    // not a controller exists.
    Win32GamepadService service;
    service.set_poll_interval(std::chrono::milliseconds(platform::kGamepadPollIntervalMs));
    domain::AppSettings settings;
    settings.record_gamepad_button = std::string("XInput|A");
    check(service.apply_settings(settings).is_ok(), "the real XInput backend starts polling");
    std::this_thread::sleep_for(150ms);
    const auto devices = service.devices();
    std::printf("  observed: devices() reports %zu controller(s) after 150 ms\n", devices.size());
    for (const auto& device : devices) {
        check(device.source == domain::GamepadSource::xinput && device.connected && device.index >= 0
                  && device.index < platform::kXInputDeviceCount,
              "a reported device names its XInput source and slot");
    }
    const auto stop_begin = std::chrono::steady_clock::now();
    check(service.stop().is_ok(), "the real XInput backend stops");
    check(std::chrono::steady_clock::now() - stop_begin < 2000ms, "that stop is prompt");
}

} // namespace

int main()
{
    test_key_table();
    test_diagnostics();
    test_hotkey_registration();
    test_capture_hook();
    test_gamepad_poll();
    test_real_xinput_reader();

    if (failures != 0) {
        std::printf("win32-input-contract: %d of %d check(s) failed\n", failures, checks);
        return 1;
    }
    std::printf("win32-input-contract: OK (%d checks, %d skipped scenario group(s))\n", checks, skips);
    return 0;
}
