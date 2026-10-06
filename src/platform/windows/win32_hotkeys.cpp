// Windows global hotkeys and the WH_KEYBOARD_LL capture hook.
//
// See win32_hotkeys.hpp for the contract, the .NET evidence and the list of
// .NET defects this file deliberately does not reproduce. Everything below the
// include block is Win32; the header stays OS-free so the mapping, the reason
// taxonomy and the diagnostics rendering are testable on any host.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "platform/windows/win32_hotkeys.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace voicetyper::platform::win32 {
namespace {

// --- Win32 constants not spelled out by <windows.h> in every SDK version ----

constexpr int kWhKeyboardLl = 13;
constexpr UINT kWmKeyDown = 0x0100;
constexpr UINT kWmKeyUp = 0x0101;
constexpr UINT kWmSysKeyDown = 0x0104;
constexpr UINT kWmSysKeyUp = 0x0105;
constexpr UINT kWmHotkey = 0x0312;
constexpr UINT kWmQuit = 0x0012;

constexpr int kVkShift = 0x10;
constexpr int kVkControl = 0x11;
constexpr int kVkAlt = 0x12;
constexpr int kVkEscape = 0x1B;
constexpr int kVkLShift = 0xA0;
constexpr int kVkRShift = 0xA1;
constexpr int kVkLControl = 0xA2;
constexpr int kVkRControl = 0xA3;
constexpr int kVkLAlt = 0xA4;
constexpr int kVkRAlt = 0xA5;
constexpr int kVkLWin = 0x5B;
constexpr int kVkRWin = 0x5C;
constexpr int kVkF1 = 0x70;
constexpr int kVkF24 = 0x87;


/// RegisterHotKey failure codes, spelled out so the mapping to a reason is
/// visible at the call site.
constexpr DWORD kErrorHotkeyAlreadyRegistered = 1409;
constexpr DWORD kErrorInvalidParameter = 87;
constexpr DWORD kErrorInvalidWindowHandle = 1400;

/// Registry paths of the two documented values that can disable hotkeys.
constexpr wchar_t kPoliciesExplorerPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
constexpr wchar_t kExplorerPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer";

std::string narrow(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::string to_lower_ascii(std::string text)
{
    for (char& ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') {
            ch = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return text;
}

/// File name of this process, e.g. "voicetyper-input-contract.exe".
std::string current_process_file_name()
{
    static const std::string cached = [] {
        std::wstring buffer(MAX_PATH, L'\0');
        for (int attempt = 0; attempt < 4; ++attempt) {
            const DWORD length = ::GetModuleFileNameW(
                nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                return std::string();
            }
            if (length < buffer.size()) {
                buffer.resize(length);
                const std::string name = narrow(buffer);
                const std::size_t slash = name.find_last_of("/\\");
                return slash == std::string::npos ? name : name.substr(slash + 1);
            }
            buffer.resize(buffer.size() * 2);
        }
        return std::string();
    }();
    return cached;
}

/// Reads a REG_DWORD value; false when absent or of another type.
bool read_dword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD& value)
{
    DWORD type = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = ::RegGetValueW(root, subkey, name, RRF_RT_REG_DWORD, &type, &value, &size);
    return status == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(value);
}

/// Reads a REG_SZ value into UTF-8; false when absent, empty or of another type.
bool read_string(HKEY root, const wchar_t* subkey, const wchar_t* name, std::string& value)
{
    DWORD type = 0;
    DWORD size = 0;
    const LSTATUS probe = ::RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, &type, nullptr, &size);
    if (probe != ERROR_SUCCESS || type != REG_SZ || size == 0 || size > 64 * 1024) {
        return false;
    }
    std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
    DWORD read = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    const LSTATUS status = ::RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, &type, buffer.data(), &read);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    const std::size_t length = buffer.find(L'\0');
    buffer.resize(length == std::wstring::npos ? buffer.size() : length);
    value = narrow(buffer);
    return !value.empty();
}

// --- Win32 key table (HotkeyService.cs:212-335) ----------------------------

bool is_function_key_code(int vk) noexcept
{
    return vk >= kVkF1 && vk <= kVkF24;
}

/// "F1".."F24" -> 0x70..0x87. Other spellings are rejected, as in the .NET
/// FunctionKey helper.
bool function_key_code(std::string_view key, int& vk) noexcept
{
    if (key.size() < 2 || key.size() > 3 || (key[0] != 'F' && key[0] != 'f')) {
        return false;
    }
    for (std::size_t i = 1; i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') {
            return false;
        }
    }
    const int number = std::atoi(std::string(key.substr(1)).c_str());
    if (number < 1 || number > 24) {
        return false;
    }
    vk = 0x70 + number - 1;
    return true;
}

/// "D0".."D9" -> 0x30..0x39 (the .NET mapping, where "D0" is the digit zero).
bool digit_key_code(std::string_view key, int& vk) noexcept
{
    if (key.size() != 2 || (key[0] != 'D' && key[0] != 'd') || key[1] < '0' || key[1] > '9') {
        return false;
    }
    vk = 0x30 + (key[1] - '0');
    return true;
}

/// "NumPad0".."NumPad9" -> 0x60..0x69, case-insensitive.
bool num_pad_key_code(std::string_view key, int& vk) noexcept
{
    const std::string lowered = to_lower_ascii(std::string(key));
    if (lowered.size() != 7 || lowered.compare(0, 6, "numpad") != 0) {
        return false;
    }
    if (lowered[6] < '0' || lowered[6] > '9') {
        return false;
    }
    vk = 0x60 + (lowered[6] - '0');
    return true;
}

/// The named non-OEM keys, a direct port of the .NET switch. The spelling is
/// compared exactly, as in C#, because the parser has already normalised the
/// first character.
int named_key_code(std::string_view key) noexcept
{
    if (key == "Space") return 0x20;
    if (key == "Enter") return 0x0D;
    if (key == "Escape") return 0x1B;
    if (key == "Tab") return 0x09;
    if (key == "Back") return 0x08;
    if (key == "Insert") return 0x2D;
    if (key == "Delete") return 0x2E;
    if (key == "Home") return 0x24;
    if (key == "End") return 0x23;
    if (key == "PageUp") return 0x21;
    if (key == "PageDown") return 0x22;
    if (key == "Left") return 0x25;
    if (key == "Up") return 0x26;
    if (key == "Right") return 0x27;
    if (key == "Down") return 0x28;
    if (key == "PrintScreen") return 0x2C;
    if (key == "Scroll") return 0x91;
    if (key == "Pause") return 0x13;
    if (key == "CapsLock") return 0x14;
    if (key == "NumLock") return 0x90;

    if (key == "OemPlus") return 0xBB;
    if (key == "OemMinus") return 0xBD;
    if (key == "OemComma") return 0xBC;
    if (key == "OemPeriod") return 0xBE;
    if (key == "OemQuestion") return 0xBF;
    if (key == "OemSemicolon") return 0xBA;
    if (key == "OemQuotes") return 0xDE;
    if (key == "OemOpenBrackets") return 0xDB;
    if (key == "OemCloseBrackets") return 0xDD;
    if (key == "OemPipe") return 0xDC;
    if (key == "OemTilde") return 0xC0;

    return 0;
}

/// The virtual-key-code -> key-name table, built once from the same list the
/// forward mapping accepts.
const std::unordered_map<int, std::string>& key_name_table()
{
    // Built once, in the same order as HotkeyService.BuildVkToName, so a code
    // with several spellings resolves to the same name as in the .NET build.
    static const std::unordered_map<int, std::string> table = [] {
        std::unordered_map<int, std::string> map;
        const auto add = [&map](std::string_view name) {
            const int vk = hotkey_virtual_key(name);
            if (vk != 0) {
                map.emplace(vk, std::string(name));
            }
        };

        static constexpr std::array<std::string_view, 32> kNamedKeys{
            "Space", "Enter", "Escape", "Tab", "Back", "Insert", "Delete",
            "Home", "End", "PageUp", "PageDown", "Left", "Up", "Right", "Down",
            "PrintScreen", "Scroll", "Pause", "CapsLock", "NumLock",
            "OemPlus", "OemMinus", "OemComma", "OemPeriod", "OemQuestion",
            "OemSemicolon", "OemQuotes", "OemOpenBrackets", "OemCloseBrackets",
            "OemPipe", "OemTilde",
        };
        for (const auto& name : kNamedKeys) {
            add(name);
        }
        for (int i = 1; i <= 24; ++i) {
            add("F" + std::to_string(i));
        }
        for (int i = 0; i <= 9; ++i) {
            add("NumPad" + std::to_string(i));
            add("D" + std::to_string(i));
        }
        for (char c = 'A'; c <= 'Z'; ++c) {
            add(std::string_view(&c, 1));
        }
        return map;
    }();
    return table;
}

} // namespace

// ---------------------------------------------------------------------------
// Key table
// ---------------------------------------------------------------------------

std::int32_t hotkey_virtual_key(std::string_view key_name) noexcept
{
    if (key_name.empty()) {
        return 0;
    }
    if (key_name.size() == 1) {
        const char c = key_name.front();
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            return static_cast<std::int32_t>(c);
        }
        return 0;
    }

    int vk = 0;
    if (function_key_code(key_name, vk)) {
        return vk;
    }
    if (digit_key_code(key_name, vk)) {
        return vk;
    }
    if (num_pad_key_code(key_name, vk)) {
        return vk;
    }
    return named_key_code(key_name);
}

std::optional<std::string> hotkey_key_name(std::int32_t virtual_key) noexcept
{
    const auto& table = key_name_table();
    const auto it = table.find(virtual_key);
    if (it == table.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::uint32_t hotkey_native_modifiers(domain::HotkeyModifiers modifiers) noexcept
{
    std::uint32_t result = 0;
    if (domain::has_modifier(modifiers, domain::HotkeyModifiers::control)) {
        result |= kModControl;
    }
    if (domain::has_modifier(modifiers, domain::HotkeyModifiers::alt)) {
        result |= kModAlt;
    }
    if (domain::has_modifier(modifiers, domain::HotkeyModifiers::shift)) {
        result |= kModShift;
    }
    if (domain::has_modifier(modifiers, domain::HotkeyModifiers::win)) {
        result |= kModWin;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Policy probe and diagnostics
// ---------------------------------------------------------------------------

std::vector<HotkeyPolicyFinding> hotkey_policy_findings()
{
    std::vector<HotkeyPolicyFinding> findings;

    // "Windows key hotkeys disabled" policy. Only meaningful for a combination
    // that uses the Win modifier, which is why hotkey_blocked_by_policy() applies
    // the gesture test and this function only reports the fact.
    DWORD no_win_keys = 0;
    if (read_dword(HKEY_CURRENT_USER, kPoliciesExplorerPath, L"NoWinKeys", no_win_keys)
        && no_win_keys != 0) {
        findings.push_back(HotkeyPolicyFinding{
            "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\NoWinKeys",
            std::to_string(no_win_keys),
            "the Windows key hotkeys are disabled by policy, so no combination that uses Win can reach an "
            "application",
        });
    }

    // Explorer's application-scoped DisabledHotkeys list. A list that does not
    // name this executable is somebody else's policy and is not a finding here.
    std::string disabled;
    if (read_string(HKEY_CURRENT_USER, kExplorerPath, L"DisabledHotkeys", disabled)) {
        const std::string exe = to_lower_ascii(current_process_file_name());
        const std::string haystack = to_lower_ascii(disabled);
        if (!exe.empty() && haystack.find(exe) != std::string::npos) {
            findings.push_back(HotkeyPolicyFinding{
                "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\DisabledHotkeys",
                disabled,
                "Explorer lists this application in DisabledHotkeys, so its global hotkeys are disabled by policy",
            });
        }
    }

    return findings;
}

std::string hotkey_process_integrity_label()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return "unknown";
    }

    // The token buffer is variable length: TOKEN_MANDATORY_LABEL holds a pointer
    // to a SID whose own length depends on the subauthority count. So the size
    // the OS asks for is the only correct size, and the SID is bounds-checked
    // against what actually came back before it is walked.
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &needed);
    if (needed < sizeof(TOKEN_MANDATORY_LABEL)) {
        ::CloseHandle(token);
        return "unknown";
    }
    std::vector<unsigned char> buffer(needed);
    DWORD returned = 0;
    const BOOL ok = ::GetTokenInformation(
        token, TokenIntegrityLevel, buffer.data(), needed, &returned);
    ::CloseHandle(token);
    if (!ok || returned < sizeof(TOKEN_MANDATORY_LABEL) || returned > buffer.size()) {
        return "unknown";
    }

    // The buffer is local and writable, so a non-const SID pointer is safe here;
    // MinGW declares GetSidSubAuthority with a non-const PSID.
    auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
    auto* sid = static_cast<SID*>(label->Label.Sid);
    if (sid == nullptr || sid->SubAuthorityCount < 1) {
        return "unknown";
    }

    // The SID lives inside the buffer the OS filled; walking it must stay in
    // range. SID is {DWORD Revision; SIZE_T SubAuthorityCount; BYTE
    // IdentifierAuthority[6]; DWORD SubAuthority[1];}, so the subauthority array
    // ends at offsetof(SID, SubAuthority) + 4 * count.
    const auto* base = reinterpret_cast<const unsigned char*>(buffer.data());
    const auto* sid_start = reinterpret_cast<const unsigned char*>(sid);
    const std::size_t sid_bytes =
        offsetof(SID, SubAuthority) + sizeof(DWORD) * static_cast<std::size_t>(sid->SubAuthorityCount);
    if (sid_start < base || sid_start + sid_bytes > base + returned) {
        return "unknown";
    }

    const DWORD rid = *::GetSidSubAuthority(
        sid, static_cast<DWORD>(sid->SubAuthorityCount - 1));

    // The well-known mandatory label RIDs; the highest match wins.
    struct Entry {
        DWORD rid;
        const char* label;
    };
    static constexpr std::array<Entry, 5> kLabels{{
        {0x4000, "system"},
        {0x3000, "high"},
        {0x2000, "medium"},
        {0x1000, "low"},
        {0x0000, "untrusted"},
    }};
    for (const auto& entry : kLabels) {
        if (rid >= entry.rid) {
            return entry.label;
        }
    }
    return "unknown";
}

bool hotkey_blocked_by_policy(const HotkeyGesture& gesture,
                              const std::vector<HotkeyPolicyFinding>& findings)
{
    if (findings.empty()) {
        return false;
    }
    const bool uses_win = domain::has_modifier(gesture.modifiers, domain::HotkeyModifiers::win);
    for (const auto& finding : findings) {
        if (finding.key.find("NoWinKeys") != std::string::npos) {
            if (uses_win) {
                return true;
            }
            continue;
        }
        if (finding.key.find("DisabledHotkeys") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string HotkeyDiagnostics::describe() const
{
    if (reason == HotkeyFailureReason::none) {
        return {};
    }

    const std::string spelling = gesture.empty() ? std::string("the hotkey") : ("\"" + gesture + "\"");
    std::string text;
    switch (reason) {
    case HotkeyFailureReason::none:
        return {};
    case HotkeyFailureReason::invalid_gesture:
        text = spelling + " is not a gesture this build can register (unknown key name, or no key)";
        break;
    case HotkeyFailureReason::policy_disabled:
        text = spelling + " is disabled by a Windows hotkey policy";
        break;
    case HotkeyFailureReason::already_registered:
        text = spelling + " is already registered by another application; Windows does not name the owner";
        break;
    case HotkeyFailureReason::invalid_parameter:
        text = spelling + " was refused by Windows as a combination (a bare key is only allowed for F1..F12)";
        break;
    case HotkeyFailureReason::os_failure:
        text = spelling + " could not be registered (unclassified Win32 failure)";
        break;
    case HotkeyFailureReason::pump_timeout:
        text = spelling + " was not registered because the hotkey thread did not answer in time";
        break;
    }

    if (win32_error != 0) {
        text += " (Win32 error " + std::to_string(win32_error);
        if (win32_error == kErrorHotkeyAlreadyRegistered) {
            text += " ERROR_HOTKEY_ALREADY_REGISTERED";
        } else if (win32_error == kErrorInvalidParameter) {
            text += " ERROR_INVALID_PARAMETER";
        } else if (win32_error == kErrorInvalidWindowHandle) {
            text += " ERROR_INVALID_WINDOW_HANDLE";
        }
        text += ")";
    }

    // The most useful hint: a hotkey held by an application at a different
    // integrity level - most commonly an elevated one - is reported by
    // RegisterHotKey as a plain conflict, with no owner named.
    if (reason == HotkeyFailureReason::already_registered) {
        text += "; this process runs at " + integrity_label + " integrity, and a hotkey owned by another"
                " integrity level is reported as a conflict - run both at the same level or pick another"
                " combination";
    }

    for (const auto& finding : policy_findings) {
        text += "; policy " + finding.key + "=" + finding.value + " (" + finding.effect + ")";
    }
    return text;
}

HotkeyDiagnostics hotkey_diagnostics(const HotkeyGesture& gesture,
                                     HotkeyFailureReason reason,
                                     std::uint32_t win32_error)
{
    HotkeyDiagnostics diagnostics;
    diagnostics.reason = reason;
    diagnostics.win32_error = win32_error;
    diagnostics.gesture = gesture.to_string();
    diagnostics.integrity_label = hotkey_process_integrity_label();
    diagnostics.policy_findings = hotkey_policy_findings();
    return diagnostics;
}

// ---------------------------------------------------------------------------
// Hotkey pump: one owned thread owns both registrations
// ---------------------------------------------------------------------------

namespace {

/// Result of a bounded pump round trip.
enum class PumpWait {
    /// The pump answered; `completion` may be read.
    answered,
    /// The bound expired. `completion` must NOT be read: the pump may still be
    /// running the action. Reporting the timeout is the whole point - the .NET
    /// RunOnPump discarded its 3 s answer and returned "registration failed".
    timed_out,
    /// No pump thread, so nothing was attempted.
    unavailable,
};

/// Result slot shared with the caller that queued an action. A shared_ptr keeps
/// it alive when the caller gives up waiting, so a late pump answer can never
/// write into freed memory.
struct PumpCompletion {
    /// Written by the pump before `done` is set; read by the caller only after
    /// PumpWait::answered.
    HotkeyRegistrationReport report;
    HotkeyDiagnostics record_diagnostics;
    HotkeyDiagnostics cancel_diagnostics;
    bool success = false;
    bool done = false;
};

struct PumpAction {
    std::function<void()> body;
    std::shared_ptr<PumpCompletion> completion;
};

/// State shared between the service and its pump thread. A shared_ptr so a
/// shutdown whose join times out can detach instead of destroying state under a
/// running thread.
struct PumpState {
    /// Wake the pump when an action is queued.
    HANDLE wake_event = nullptr;
    /// Set to stop the pump. The pump also waits on it, so a lost WM_QUIT is
    /// never a hang.
    HANDLE stop_event = nullptr;

    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::atomic<DWORD> thread_id{0};

    std::mutex mutex;
    std::condition_variable settled;
    std::deque<PumpAction> actions;

    /// Only touched on the pump thread.
    int record_id = 0;
    int cancel_id = 0;
    std::int32_t record_vk = 0;
    bool record_down = false;
    bool release_poll_installed = false;

    std::mutex sink_mutex;
    HotkeyEventSink sink;

    std::chrono::milliseconds action_timeout{std::chrono::milliseconds(kDefaultPumpActionTimeoutMs)};
    std::chrono::milliseconds shutdown_timeout{std::chrono::milliseconds(kDefaultShutdownTimeoutMs)};

    ~PumpState()
    {
        if (wake_event != nullptr) {
            ::CloseHandle(wake_event);
        }
        if (stop_event != nullptr) {
            ::CloseHandle(stop_event);
        }
    }

    /// Queues an action for the pump thread without waiting for an answer. Used by
    /// the keyboard hook: blocking the hook thread would stall keyboard input for
    /// every application on the desktop.
    void post(const std::function<void()>& body)
    {
        if (!running.load()) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex);
            actions.push_back(PumpAction{body, nullptr});
        }
        if (wake_event != nullptr) {
            ::SetEvent(wake_event);
        }
    }

    /// Queues one action for the pump thread, without waiting: the hook thread may
    /// never block, or input stalls for every application.
    void post_action(HotkeyAction action)
    {
        if (!running.load()) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex);
            actions.push_back(PumpAction{[this, action] { emit(action); }, nullptr});
        }
        if (wake_event != nullptr) {
            ::SetEvent(wake_event);
        }
    }

    void emit(HotkeyAction action) noexcept
    {
        HotkeyEventSink copy;
        {
            const std::lock_guard<std::mutex> lock(sink_mutex);
            copy = sink;
        }
        if (!copy) {
            return;
        }
        // The contract says exceptions cannot escape the sink, and none may
        // cross the platform boundary either.
        try {
            copy(action);
        } catch (...) {
        }
    }

    void stop_release_poll() noexcept
    {
        release_poll_installed = false;
        record_vk = 0;
        record_down = false;
    }

    void install_release_poll(std::int32_t vk) noexcept
    {
        record_vk = vk;
        release_poll_installed = vk != 0;
    }

    /// Push-to-talk release edge. The .NET service used a static, token-less
    /// WaitForKeyRelease that could outlive the key it was waiting for; here the
    /// poll is bound to the currently registered record hotkey and dies with it.
    ///
    /// Called directly from the pump loop, not from a WM_TIMER: the first version
    /// installed a 30 ms timer and handled WM_TIMER, and on the target machine
    /// that message never arrived - measured 2026-10-01, a 4 s hold produced
    /// "hotkey record pressed" and no release at all, so recording never stopped
    /// and recognition never started. The pump wakes on the same
    /// kReleasePollIntervalMs cadence anyway, and a loop iteration cannot
    /// silently stop firing.
    void poll_release() noexcept
    {
        if (record_vk == 0) {
            return;
        }
        const bool down = (::GetAsyncKeyState(record_vk) & 0x8000) != 0;
        if (down == record_down) {
            return;
        }
        record_down = down;
        if (!down) {
            emit(HotkeyAction::record_released);
        }
    }
};

void drain_messages(PumpState& state)
{
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        switch (message.message) {
        case kWmHotkey: {
            const int id = static_cast<int>(static_cast<INT_PTR>(message.wParam));
            if (id != 0 && id == state.record_id) {
                // A press edge can arrive for a key that is already physically
                // up when another hotkey consumed the keystroke, so the release
                // poll is told the key is down and emits the falling edge.
                state.record_down = true;
                state.emit(HotkeyAction::record_pressed);
            } else if (id != 0 && id == state.cancel_id) {
                state.emit(HotkeyAction::cancel_pressed);
            }
            break;
        }
        default:
            break;
        }
    }
}

void run_actions(PumpState& state)
{
    for (;;) {
        PumpAction action;
        {
            const std::lock_guard<std::mutex> lock(state.mutex);
            if (state.actions.empty()) {
                return;
            }
            action = std::move(state.actions.front());
            state.actions.pop_front();
        }
        bool success = false;
        try {
            action.body();
            success = true;
        } catch (...) {
            success = false;
        }
        if (action.completion) {
            {
                const std::lock_guard<std::mutex> lock(state.mutex);
                action.completion->success = success;
                action.completion->done = true;
            }
            state.settled.notify_all();
        }
    }
}

void pump_main(std::shared_ptr<PumpState> state)
{
    // Published as the very first statement: stop() posts WM_QUIT to this id
    // unconditionally, and the queue below exists before the first message, so a
    // thread that has not reached its pump can still be stopped. This is exactly
    // the hole in HotkeyCaptureHook.Stop (HotkeyCaptureHook.cs:218-234).
    state->thread_id.store(::GetCurrentThreadId());
    MSG probe{};
    ::PeekMessageW(&probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    state->running.store(true);

    while (!state->stop_requested.load(std::memory_order_acquire)) {
        HANDLE handles[1] = {state->stop_event};
        ::MsgWaitForMultipleObjects(1, handles, FALSE, kReleasePollIntervalMs, QS_ALLINPUT);
        drain_messages(*state);
        run_actions(*state);
        // The push-to-talk falling edge is polled here, on the same thread and
        // the same 25 ms cadence the message pump already has.
        state->poll_release();
        if (::WaitForSingleObject(state->stop_event, 0) == WAIT_OBJECT_0) {
            break;
        }
    }

    // Unconditional cleanup on the thread that registered: a hotkey released
    // from another thread is not released at all. The .NET Dispose did not even
    // join this thread (HotkeyService.cs:84-89).
    for (const int id : {state->record_id, state->cancel_id}) {
        if (id != 0) {
            ::UnregisterHotKey(nullptr, id);
        }
    }
    state->stop_release_poll();
    state->record_id = 0;
    state->cancel_id = 0;
    state->running.store(false);
}

HotkeyFailureReason reason_from_win32(DWORD error) noexcept
{
    switch (error) {
    case kErrorHotkeyAlreadyRegistered:
        return HotkeyFailureReason::already_registered;
    case kErrorInvalidParameter:
    case kErrorInvalidWindowHandle:
        return HotkeyFailureReason::invalid_parameter;
    default:
        return HotkeyFailureReason::os_failure;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Win32HotkeyService
// ---------------------------------------------------------------------------

/// The dictation hook's action sink: hops from the hook thread to the service's
/// pump thread, because the recording machine is thread-affine.
struct PumpActionSink {
    std::shared_ptr<PumpState> pump_state;

    void operator()(HotkeyAction action) const
    {
        if (pump_state != nullptr) {
            pump_state->post_action(action);
        }
    }
};

struct Win32HotkeyService::Impl {
    std::shared_ptr<PumpState> state = std::make_shared<PumpState>();
    std::thread pump;
    std::atomic<bool> pump_started{false};

    /// The permanent dictation hook (WH_KEYBOARD_LL). One per service, installed
    /// on the first apply_settings() and kept until stop().
    /// The hook-thread state, defined further down with the hook procedure. Held as
    /// void here so the service does not need the type before it exists; the
    /// definitions below cast it back.
    std::shared_ptr<void> dictation_state;
    std::thread dictation_thread;

    mutable std::mutex report_mutex;
    HotkeyRegistrationReport report;
    HotkeyDiagnostics record_diagnostics;
    HotkeyDiagnostics cancel_diagnostics;
    std::int32_t record_key_code_value = 0;

    Impl()
    {
        state->wake_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        state->stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (state->wake_event == nullptr || state->stop_event == nullptr) {
            return;
        }
        try {
            // A local copy: the thread must own the state, not borrow it from
            // this object, so a shutdown that misses its join deadline is safe.
            const std::shared_ptr<PumpState> pump_state = state;
            pump = std::thread([pump_state]() { pump_main(pump_state); });
        } catch (...) {
            return;
        }
        pump_started.store(true);

        // Bounded wait for the pump to be live, so the first apply_settings()
        // cannot observe "not running" and report a mechanism that does exist
        // as unavailable.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (!state->running.load() && std::chrono::steady_clock::now() < deadline) {
            ::Sleep(1);
        }
        if (!state->running.load()) {
            pump_started.store(false);
        }
    }

    ~Impl()
    {
        stop();
    }

    PumpWait run_on_pump(const std::function<void()>& body,
                         std::shared_ptr<PumpCompletion>& completion)
    {
        completion = std::make_shared<PumpCompletion>();
        if (!pump_started.load() || !state->running.load()) {
            return PumpWait::unavailable;
        }
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            state->actions.push_back(PumpAction{body, completion});
        }
        ::SetEvent(state->wake_event);

        std::unique_lock<std::mutex> lock(state->mutex);
        const bool answered = state->settled.wait_for(lock, state->action_timeout, [&completion] {
            return completion->done;
        });
        return answered ? PumpWait::answered : PumpWait::timed_out;
    }

    /// Unregisters both ids on the pump thread and waits for it to happen.
    /// Idempotent: nothing registered means success.
    Status unregister_on_pump()
    {
        if (!pump_started.load() || !state->running.load()) {
            return Status::success();
        }
        auto completion = std::make_shared<PumpCompletion>();
        auto state = this->state;
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            state->actions.push_back(PumpAction{
                [state]() {
                    for (const int id : {state->record_id, state->cancel_id}) {
                        if (id != 0) {
                            ::UnregisterHotKey(nullptr, id);
                        }
                    }
                    state->stop_release_poll();
                    state->record_id = 0;
                    state->cancel_id = 0;
                    state->record_down = false;
                },
                completion,
            });
        }
        ::SetEvent(state->wake_event);
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            const bool answered = state->settled.wait_for(lock, state->shutdown_timeout, [&completion] {
                return completion->done;
            });
            if (!answered) {
                return Status::failure(
                    ErrorCode::timeout,
                    "the hotkey thread did not confirm the unregister within "
                        + std::to_string(state->shutdown_timeout.count()) + " ms");
            }
        }
        return Status::success();
    }

    /// Stops the pump thread. Idempotent and bounded. A thread that misses the
    /// deadline is detached rather than killed: it holds the state through a
    /// shared_ptr and exits within one 25 ms tick, while a hotkey whose thread
    /// is forcibly terminated could never be released.
    /// Installs the dictation hook once and points it at the current gestures. The
    /// sink is marshalled to the pump thread, because the recording machine is
    /// thread-affine and the hook runs on its own thread. Defined with the hook
    /// procedure, where CaptureState is complete.
    void ensure_dictation_hook(const HotkeyGesture& record, const HotkeyGesture& cancel);
    /// True when the hook is really installed (bounded wait).
    bool dictation_hook_ready();
    /// Stops and drops the hook thread.
    void stop_dictation_hook();

    void stop()
    {
        stop_dictation_hook();
        if (!pump_started.exchange(false)) {
            return;
        }
        state->stop_requested.store(true, std::memory_order_release);
        ::SetEvent(state->stop_event);
        const DWORD id = state->thread_id.load();
        if (id != 0) {
            ::PostThreadMessageW(id, kWmQuit, 0, 0);
        }
        if (!pump.joinable()) {
            return;
        }
        if (pump.get_id() == std::this_thread::get_id()) {
            pump.detach();
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + state->shutdown_timeout;
        while (state->running.load()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                pump.detach();
                return;
            }
            ::Sleep(1);
        }
        if (pump.joinable()) {
            pump.join();
        }
    }
};

Win32HotkeyService::Win32HotkeyService()
    : impl_(std::make_unique<Impl>())
{
}

Win32HotkeyService::~Win32HotkeyService()
{
    if (impl_ != nullptr) {
        impl_->unregister_on_pump();
        impl_->stop();
    }
}

Status Win32HotkeyService::set_event_sink(HotkeyEventSink sink)
{
    if (impl_ == nullptr) {
        return Status::failure(ErrorCode::unavailable, "hotkey service is not constructed");
    }
    const std::lock_guard<std::mutex> lock(impl_->state->sink_mutex);
    impl_->state->sink = std::move(sink);
    return Status::success();
}

void Win32HotkeyService::set_pump_action_timeout(std::chrono::milliseconds timeout) noexcept
{
    if (impl_ != nullptr) {
        impl_->state->action_timeout = timeout;
    }
}

void Win32HotkeyService::set_shutdown_timeout(std::chrono::milliseconds timeout) noexcept
{
    if (impl_ != nullptr) {
        impl_->state->shutdown_timeout = timeout;
    }
}

bool Win32HotkeyService::pump_running() const noexcept
{
    return impl_ != nullptr && impl_->pump_started.load() && impl_->state->running.load();
}

std::int32_t Win32HotkeyService::record_key_code() const noexcept
{
    return impl_ == nullptr ? 0 : impl_->record_key_code_value;
}

HotkeyRegistrationReport Win32HotkeyService::last_report() const
{
    const std::lock_guard<std::mutex> lock(impl_->report_mutex);
    return impl_->report;
}

HotkeyDiagnostics Win32HotkeyService::last_diagnostics(HotkeyAction action) const
{
    const std::lock_guard<std::mutex> lock(impl_->report_mutex);
    return action == HotkeyAction::cancel_pressed ? impl_->cancel_diagnostics : impl_->record_diagnostics;
}

Result<HotkeyRegistrationReport> Win32HotkeyService::apply_settings(const AppSettings& settings)
{
    if (impl_ == nullptr) {
        return Result<HotkeyRegistrationReport>::failure(
            ErrorCode::unavailable, "hotkey service is not constructed");
    }
    if (!impl_->pump_started.load()) {
        return Result<HotkeyRegistrationReport>::failure(
            ErrorCode::unavailable,
            "the hotkey message pump thread could not be started, so this machine offers no global hotkey "
            "mechanism to the application");
    }

    HotkeyRegistrationReport report;

    // --- 1. Parse both gestures. A string that does not parse is reported per
    // hotkey *and* fails the call with invalid_argument, as the frozen contract
    // documents.
    std::optional<HotkeyGesture> record_gesture;
    std::optional<HotkeyGesture> cancel_gesture;
    std::vector<std::string> parse_errors;

    if (const auto parsed = domain::parse_hotkey(settings.record_hotkey); parsed.is_ok()) {
        record_gesture = parsed.value();
    } else {
        HotkeyRegistration entry;
        entry.action = HotkeyAction::record_pressed;
        entry.registered = false;
        entry.error = "record hotkey \"" + settings.record_hotkey
                      + "\" does not parse: " + parsed.error().message();
        report.record.push_back(entry);
        parse_errors.push_back(entry.error);
    }
    if (const auto parsed = domain::parse_hotkey(settings.cancel_hotkey); parsed.is_ok()) {
        cancel_gesture = parsed.value();
    } else {
        HotkeyRegistration entry;
        entry.action = HotkeyAction::cancel_pressed;
        entry.registered = false;
        entry.error = "cancel hotkey \"" + settings.cancel_hotkey
                      + "\" does not parse: " + parsed.error().message();
        report.cancel.push_back(entry);
        parse_errors.push_back(entry.error);
    }

    if (!parse_errors.empty()) {
        std::string message = parse_errors.front();
        if (parse_errors.size() > 1) {
            message += "; " + parse_errors[1];
        }
        const std::lock_guard<std::mutex> lock(impl_->report_mutex);
        impl_->report = report;
        impl_->record_diagnostics = HotkeyDiagnostics();
        impl_->cancel_diagnostics = HotkeyDiagnostics();
        impl_->record_key_code_value = 0;
        return Result<HotkeyRegistrationReport>::failure(ErrorCode::invalid_argument, std::move(message));
    }

    // --- 2. Map both gestures to native codes. A key the product table does not
    // know is a per-hotkey failure with a reason, not a silent no-op. A
    // combination a documented policy forbids is refused *before* any
    // RegisterHotKey call, so the user is told the real cause.
    const std::vector<HotkeyPolicyFinding> findings = hotkey_policy_findings();
    const std::string integrity = hotkey_process_integrity_label();

    HotkeyRegistration record_entry;
    record_entry.action = HotkeyAction::record_pressed;
    record_entry.gesture = *record_gesture;
    HotkeyRegistration cancel_entry;
    cancel_entry.action = HotkeyAction::cancel_pressed;
    cancel_entry.gesture = *cancel_gesture;

    HotkeyDiagnostics record_diag;
    HotkeyDiagnostics cancel_diag;
    std::int32_t record_vk = 0;
    std::int32_t cancel_vk = 0;

    const auto prepare = [&](const HotkeyGesture& gesture, std::int32_t& vk, HotkeyDiagnostics& diag) {
        vk = hotkey_virtual_key(gesture.key);
        if (!gesture.is_valid() || vk == 0) {
            diag = hotkey_diagnostics(gesture, HotkeyFailureReason::invalid_gesture);
            return false;
        }
        if (hotkey_blocked_by_policy(gesture, findings)) {
            diag = hotkey_diagnostics(gesture, HotkeyFailureReason::policy_disabled);
            return false;
        }
        diag = HotkeyDiagnostics();
        diag.reason = HotkeyFailureReason::none;
        diag.gesture = gesture.to_string();
        diag.integrity_label = integrity;
        diag.policy_findings = findings;
        return true;
    };

    const bool record_ready = prepare(*record_gesture, record_vk, record_diag);
    const bool cancel_ready = prepare(*cancel_gesture, cancel_vk, cancel_diag);

    // --- 3. One bounded pump round trip registers whatever is registrable, so a
    // settings change can never leave the record hotkey bound to the old
    // gesture while the cancel hotkey is already the new one.
    struct Outcome {
        bool record_registered = false;
        bool cancel_registered = false;
        DWORD record_error = 0;
        DWORD cancel_error = 0;
    };
    const auto outcome = std::make_shared<Outcome>();
    auto state = impl_->state;

    // The dictation hotkey is detected by the permanent low-level hook, exactly
    // like the .NET build: RegisterHotKey never sees a combination the OS reserves
    // (the default Alt+Win+Space is swallowed by the Win+Space layout switcher -
    // the app recorded 0 presses while the same combination injected
    // programmatically was handled in 2 ms).
    impl_->ensure_dictation_hook(*record_gesture, *cancel_gesture);
    const bool hook_ready = impl_->dictation_hook_ready();

    std::shared_ptr<PumpCompletion> completion;
    const PumpWait waited = impl_->run_on_pump(
        [state, outcome, record_ready, cancel_ready, record_gesture, cancel_gesture, record_vk, cancel_vk, hook_ready] {
            if (state->record_id != 0) {
                ::UnregisterHotKey(nullptr, state->record_id);
                state->record_id = 0;
            }
            if (state->cancel_id != 0) {
                ::UnregisterHotKey(nullptr, state->cancel_id);
                state->cancel_id = 0;
            }
            state->stop_release_poll();

            if (record_ready && hook_ready) {
                // The hook reports this gesture; registering it as well would fire
                // every action twice.
                outcome->record_registered = true;
            } else if (record_ready) {
                const std::uint32_t mods =
                    hotkey_native_modifiers(record_gesture->modifiers) | kModNoRepeat;
                if (::RegisterHotKey(nullptr, kRecordHotkeyId, mods, static_cast<int>(record_vk)) != 0) {
                    state->record_id = kRecordHotkeyId;
                    state->install_release_poll(record_vk);
                    outcome->record_registered = true;
                } else {
                    outcome->record_error = ::GetLastError();
                }
            }
            if (cancel_ready && hook_ready) {
                outcome->cancel_registered = true;
            } else if (cancel_ready) {
                const std::uint32_t mods =
                    hotkey_native_modifiers(cancel_gesture->modifiers) | kModNoRepeat;
                if (::RegisterHotKey(nullptr, kCancelHotkeyId, mods, static_cast<int>(cancel_vk)) != 0) {
                    state->cancel_id = kCancelHotkeyId;
                    outcome->cancel_registered = true;
                } else {
                    outcome->cancel_error = ::GetLastError();
                }
            }
        },
        completion);

    if (waited != PumpWait::answered) {
        // The completion is not read on a timeout: the pump may still be running
        // the action. The failure is reported truthfully, per hotkey, and the
        // call fails with the code that actually happened.
        const HotkeyFailureReason reason =
            waited == PumpWait::timed_out ? HotkeyFailureReason::pump_timeout
                                          : HotkeyFailureReason::os_failure;
        record_diag = hotkey_diagnostics(*record_gesture, reason);
        cancel_diag = hotkey_diagnostics(*cancel_gesture, reason);
        record_entry.registered = false;
        record_entry.error = record_diag.describe();
        cancel_entry.registered = false;
        cancel_entry.error = cancel_diag.describe();
        report.record.clear();
        report.record.push_back(record_entry);
        report.cancel.clear();
        report.cancel.push_back(cancel_entry);
        {
            const std::lock_guard<std::mutex> lock(impl_->report_mutex);
            impl_->report = report;
            impl_->record_diagnostics = record_diag;
            impl_->cancel_diagnostics = cancel_diag;
            impl_->record_key_code_value = 0;
        }
        return waited == PumpWait::timed_out
            ? Result<HotkeyRegistrationReport>::failure(
                  ErrorCode::timeout,
                  "the hotkey message pump did not answer within "
                      + std::to_string(state->action_timeout.count()) + " ms; no hotkey was registered")
            : Result<HotkeyRegistrationReport>::failure(
                  ErrorCode::unavailable, "the hotkey message pump is not running");
    }

    // --- 4. Turn the two outcomes into the per-hotkey report.
    if (record_ready) {
        record_entry.registered = outcome->record_registered;
        if (outcome->record_registered) {
            record_entry.native_key_code = record_vk;
            record_diag = HotkeyDiagnostics();
            record_diag.gesture = record_gesture->to_string();
            record_diag.integrity_label = integrity;
        } else {
            record_diag = hotkey_diagnostics(
                *record_gesture, reason_from_win32(outcome->record_error), outcome->record_error);
            record_entry.error = record_diag.describe();
        }
    } else {
        record_entry.registered = false;
        record_entry.error = record_diag.describe();
    }

    if (cancel_ready) {
        cancel_entry.registered = outcome->cancel_registered;
        if (outcome->cancel_registered) {
            cancel_entry.native_key_code = cancel_vk;
            cancel_diag = HotkeyDiagnostics();
            cancel_diag.gesture = cancel_gesture->to_string();
            cancel_diag.integrity_label = integrity;
        } else {
            cancel_diag = hotkey_diagnostics(
                *cancel_gesture, reason_from_win32(outcome->cancel_error), outcome->cancel_error);
            cancel_entry.error = cancel_diag.describe();
        }
    } else {
        cancel_entry.registered = false;
        cancel_entry.error = cancel_diag.describe();
    }

    report.record.clear();
    report.record.push_back(record_entry);
    report.cancel.clear();
    report.cancel.push_back(cancel_entry);

    {
        const std::lock_guard<std::mutex> lock(impl_->report_mutex);
        impl_->report = report;
        impl_->record_diagnostics = record_diag;
        impl_->cancel_diagnostics = cancel_diag;
        impl_->record_key_code_value = record_entry.registered ? record_vk : 0;
    }

    // The call succeeds even when a combination was refused: a conflict is a
    // per-hotkey result, exactly as the .NET ApplySettings list-of-strings
    // contract requires. Only "no global hotkey mechanism at all" fails it.
    return report;
}

Status Win32HotkeyService::unregister_all()
{
    if (impl_ == nullptr) {
        return Status::success();
    }
    if (!impl_->pump_started.load()) {
        const std::lock_guard<std::mutex> lock(impl_->report_mutex);
        impl_->report = HotkeyRegistrationReport();
        impl_->record_key_code_value = 0;
        return Status::success();
    }

    // The unregister runs on the pump thread and this call waits for it, so
    // after it returns no id is registered and no further sink callback can be
    // started. The sink itself stays installed for the next apply_settings();
    // what is dropped is the binding, which is what stops delivery.
    const Status status = impl_->unregister_on_pump();
    {
        const std::lock_guard<std::mutex> lock(impl_->report_mutex);
        impl_->report = HotkeyRegistrationReport();
        impl_->record_diagnostics = HotkeyDiagnostics();
        impl_->cancel_diagnostics = HotkeyDiagnostics();
        impl_->record_key_code_value = 0;
    }
    return status;
}

// ---------------------------------------------------------------------------
// HotkeyCaptureHook
// ---------------------------------------------------------------------------

namespace {

/// Hook-thread state. The WH_KEYBOARD_LL procedure runs on the thread that
/// installed it, so `down` needs no synchronization: only the procedure touches
/// it. Everything the caller reads is under `mutex`.
struct CaptureState {
    HANDLE stop_event = nullptr;

    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::atomic<bool> hook_installed{false};
    std::atomic<DWORD> thread_id{0};

    std::mutex mutex;
    std::condition_variable settled;
    bool completed = false;
    bool has_result = false;
    HotkeyGesture gesture;
    Error failure;

    std::set<int> down;
    HotkeyCaptureHook::ModifierRequiredCallback on_modifier_required;

    /// Dictation mode: the same low-level hook, but permanent and reporting the
    /// dictation hotkey instead of capturing one. RegisterHotKey cannot deliver a
    /// combination Windows reserves - Alt+Win+Space never reached the app while the
    /// synthetic press did (m_23de0707f9db) - and the .NET build used a hook for
    /// the dictation hotkey for that reason.
    bool dictation = false;
    std::optional<HotkeyGesture> record_gesture;
    std::optional<HotkeyGesture> cancel_gesture;
    std::function<void(HotkeyAction)> on_action;
    /// Hook-thread only, like `down`.
    bool record_held = false;
    bool cancel_held = false;
    std::chrono::milliseconds shutdown_timeout{std::chrono::milliseconds(kDefaultShutdownTimeoutMs)};

    ~CaptureState()
    {
        // Closed here rather than in stop(): the thread holds the state through
        // a shared_ptr, so the handle outlives every use of it even when a
        // bounded stop had to detach.
        if (stop_event != nullptr) {
            ::CloseHandle(stop_event);
        }
    }

    /// One-shot completion. Also stops the pump, so the hook is uninstalled as
    /// soon as the capture ends.
    void complete(std::optional<HotkeyGesture> result) noexcept
    {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (completed) {
                return;
            }
            completed = true;
            has_result = result.has_value();
            if (result.has_value()) {
                gesture = *result;
            }
        }
        settled.notify_all();
        stop_requested.store(true, std::memory_order_release);
        if (stop_event != nullptr) {
            ::SetEvent(stop_event);
        }
        const DWORD id = thread_id.load();
        if (id != 0) {
            ::PostThreadMessageW(id, kWmQuit, 0, 0);
        }
    }
};

/// Active capture states, keyed by the owning thread id. A static table is the
/// only way a hook procedure can find its owner: the procedure receives no user
/// pointer from Win32.
std::mutex& active_captures_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<CaptureState*>& active_captures()
{
    static std::vector<CaptureState*> states;
    return states;
}

CaptureState* find_capture_state(DWORD thread_id)
{
    const std::lock_guard<std::mutex> lock(active_captures_mutex());
    for (CaptureState* state : active_captures()) {
        if (state != nullptr && state->thread_id.load() == thread_id) {
            return state;
        }
    }
    return nullptr;
}

bool is_modifier_key(int vk) noexcept
{
    return vk == kVkShift || vk == kVkControl || vk == kVkAlt || vk == kVkLShift || vk == kVkRShift
        || vk == kVkLControl || vk == kVkRControl || vk == kVkLAlt || vk == kVkRAlt || vk == kVkLWin
        || vk == kVkRWin;
}

domain::HotkeyModifiers read_modifiers(const std::set<int>& down) noexcept
{
    const auto held = [&down](int a, int b, int c) {
        return down.count(a) != 0 || down.count(b) != 0 || down.count(c) != 0;
    };
    domain::HotkeyModifiers mods = domain::HotkeyModifiers::none;
    if (held(kVkControl, kVkLControl, kVkRControl)) {
        mods |= domain::HotkeyModifiers::control;
    }
    if (held(kVkAlt, kVkLAlt, kVkRAlt)) {
        mods |= domain::HotkeyModifiers::alt;
    }
    if (held(kVkShift, kVkLShift, kVkRShift)) {
        mods |= domain::HotkeyModifiers::shift;
    }
    if (down.count(kVkLWin) != 0 || down.count(kVkRWin) != 0) {
        mods |= domain::HotkeyModifiers::win;
    }
    return mods;
}

/// Reports press/release/cancel for the dictation gestures from raw hook events.
/// Runs on the hook thread; the sink itself is invoked on the service's pump
/// thread, because the recording machine is thread-affine.
///
/// Returns true when the event belongs to a dictation gesture and must be
/// swallowed. Swallowing is what keeps Windows out of the way: Alt+Space is the
/// window's system menu and Win+Space switches the keyboard layout, so a hook that
/// only reports the combination still lets those menus appear (reported from the
/// running build: "у меня вылезает менюшка окна"). The modifiers themselves are
/// never swallowed - a plain Alt+Space or a plain Win tap must keep working.
bool dispatch_dictation(CaptureState& state, int vk, bool is_down)
{
    std::optional<HotkeyGesture> record;
    std::optional<HotkeyGesture> cancel;
    std::function<void(HotkeyAction)> sink;
    {
        const std::lock_guard<std::mutex> lock(state.mutex);
        record = state.record_gesture;
        cancel = state.cancel_gesture;
        sink = state.on_action;
    }
    if (!sink) {
        return false;
    }
    const auto name = hotkey_key_name(vk);
    if (!name.has_value()) {
        return false;
    }
    const domain::HotkeyModifiers mods = read_modifiers(state.down);
    const auto fire = [&sink](HotkeyAction action) {
        try {
            sink(action);
        } catch (...) {
            // A sink may never take the hook down.
        }
    };

    if (is_down) {
        if (record.has_value() && record->key == *name && record->modifiers == mods) {
            if (!state.record_held) {
                state.record_held = true;
                fire(HotkeyAction::record_pressed);
            }
            return true;
        }
        if (cancel.has_value() && cancel->key == *name && cancel->modifiers == mods) {
            if (!state.cancel_held) {
                state.cancel_held = true;
                fire(HotkeyAction::cancel_pressed);
            }
            return true;
        }
        return false;
    }

    // The release edge belongs to the key the gesture named, whatever the modifier
    // state is by then: the user usually lets the modifiers go first.
    if (state.record_held && record.has_value() && record->key == *name) {
        state.record_held = false;
        fire(HotkeyAction::record_released);
        return true;
    }
    if (state.cancel_held && cancel.has_value() && cancel->key == *name) {
        state.cancel_held = false;
        return true;
    }
    return false;
}

LRESULT CALLBACK keyboard_ll_hook_proc(int code, WPARAM w_param, LPARAM l_param)
{
    if (code < 0) {
        return ::CallNextHookEx(nullptr, code, w_param, l_param);
    }
    CaptureState* state = find_capture_state(::GetCurrentThreadId());
    if (state == nullptr) {
        return ::CallNextHookEx(nullptr, code, w_param, l_param);
    }

    const UINT message = static_cast<UINT>(w_param);
    const bool is_down = message == kWmKeyDown || message == kWmSysKeyDown;
    const bool is_up = message == kWmKeyUp || message == kWmSysKeyUp;
    if (!is_down && !is_up) {
        return ::CallNextHookEx(nullptr, code, w_param, l_param);
    }

    KBDLLHOOKSTRUCT info{};
    std::memcpy(&info, reinterpret_cast<const void*>(l_param), sizeof(info));
    const int vk = static_cast<int>(info.vkCode);

    // Modifiers are tracked from the hook's own events, not GetAsyncKeyState:
    // the Win key is suppressed below and GetAsyncKeyState is unreliable for a
    // key the hook itself swallows (m_22804fda833e).
    if (is_down) {
        state->down.insert(vk);
    } else {
        state->down.erase(vk);
    }

    // Suppress Win only while a hotkey is being captured: the settings window must
    // not lose focus to the Start menu. In dictation mode Win is passed through, so
    // the Start menu keeps working - the hook sees the gesture before the shell
    // does, which is all the fix needs.
    if (!state->dictation && (vk == kVkLWin || vk == kVkRWin)) {
        return 1;
    }

    if (state->dictation) {
        // A gesture key is swallowed so the shell never sees it; every other key
        // passes through untouched.
        return dispatch_dictation(*state, vk, is_down) ? 1
                                                       : ::CallNextHookEx(nullptr, code, w_param, l_param);
    }

    if (!is_down || is_modifier_key(vk)) {
        return ::CallNextHookEx(nullptr, code, w_param, l_param);
    }

    const domain::HotkeyModifiers mods = read_modifiers(state->down);

    // Escape without modifiers cancels the capture.
    if (vk == kVkEscape && mods == domain::HotkeyModifiers::none) {
        state->complete(std::nullopt);
        return 1;
    }

    // A key without a modifier is rejected unless it is a function key.
    if (mods == domain::HotkeyModifiers::none && !is_function_key_code(vk)) {
        if (state->on_modifier_required) {
            try {
                state->on_modifier_required();
            } catch (...) {
            }
        }
        return ::CallNextHookEx(nullptr, code, w_param, l_param);
    }

    const auto name = hotkey_key_name(vk);
    if (!name.has_value()) {
        // A key the product cannot spell (media keys, IME keys) is passed
        // through instead of producing a gesture that cannot be re-parsed.
        return ::CallNextHookEx(nullptr, code, w_param, l_param);
    }

    state->complete(HotkeyGesture{mods, *name});
    return 1;
}

void capture_main(std::shared_ptr<CaptureState> state)
{
    // Published first, and the queue is created explicitly, so stop() can post
    // WM_QUIT even while the hook is still being installed.
    state->thread_id.store(::GetCurrentThreadId());
    MSG probe{};
    ::PeekMessageW(&probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    {
        const std::lock_guard<std::mutex> lock(active_captures_mutex());
        active_captures().push_back(state.get());
    }
    state->running.store(true);

    HHOOK hook = ::SetWindowsHookExW(kWhKeyboardLl, &keyboard_ll_hook_proc, ::GetModuleHandleW(nullptr), 0);
    state->hook_installed.store(hook != nullptr);
    if (hook == nullptr) {
        const DWORD error = ::GetLastError();
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            state->completed = true;
            state->has_result = false;
            state->failure = Error(
                ErrorCode::unavailable,
                "SetWindowsHookExW(WH_KEYBOARD_LL) failed with Win32 error " + std::to_string(error)
                    + "; a global low-level hook needs a desktop window station and cannot see input injected "
                      "by a higher-integrity process");
        }
        state->settled.notify_all();
        state->stop_requested.store(true, std::memory_order_release);
        if (state->stop_event != nullptr) {
            ::SetEvent(state->stop_event);
        }
    }

    while (!state->stop_requested.load(std::memory_order_acquire)) {
        HANDLE handles[1] = {state->stop_event};
        ::MsgWaitForMultipleObjects(1, handles, FALSE, kReleasePollIntervalMs, QS_ALLINPUT);
        if (::WaitForSingleObject(state->stop_event, 0) == WAIT_OBJECT_0) {
            break;
        }
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == kWmQuit) {
                state->stop_requested.store(true, std::memory_order_release);
                break;
            }
        }
    }

    if (hook != nullptr) {
        ::UnhookWindowsHookEx(hook);
    }
    {
        const std::lock_guard<std::mutex> lock(active_captures_mutex());
        std::vector<CaptureState*>& states = active_captures();
        states.erase(std::remove(states.begin(), states.end(), state.get()), states.end());
    }
    state->hook_installed.store(false);
    state->running.store(false);
}

/// The permanent dictation hook: the same procedure and registry as the capture
/// hook, but it lives for the whole session and never completes by itself.
void dictation_hook_main(std::shared_ptr<CaptureState> state)
{
    state->thread_id.store(::GetCurrentThreadId());
    MSG probe{};
    ::PeekMessageW(&probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    {
        const std::lock_guard<std::mutex> lock(active_captures_mutex());
        active_captures().push_back(state.get());
    }
    state->running.store(true);

    HHOOK hook = ::SetWindowsHookExW(kWhKeyboardLl, &keyboard_ll_hook_proc, ::GetModuleHandleW(nullptr), 0);
    state->hook_installed.store(hook != nullptr);
    if (hook == nullptr) {
        const DWORD error = ::GetLastError();
        const std::lock_guard<std::mutex> lock(state->mutex);
        state->failure = Error(
            ErrorCode::unavailable,
            "SetWindowsHookExW(WH_KEYBOARD_LL) failed with Win32 error " + std::to_string(error));
    }

    while (!state->stop_requested.load(std::memory_order_acquire)) {
        HANDLE handles[1] = {state->stop_event};
        ::MsgWaitForMultipleObjects(1, handles, FALSE, kReleasePollIntervalMs, QS_ALLINPUT);
        if (::WaitForSingleObject(state->stop_event, 0) == WAIT_OBJECT_0) {
            break;
        }
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == kWmQuit) {
                state->stop_requested.store(true, std::memory_order_release);
                break;
            }
        }
    }

    if (hook != nullptr) {
        ::UnhookWindowsHookEx(hook);
    }
    {
        const std::lock_guard<std::mutex> lock(active_captures_mutex());
        std::vector<CaptureState*>& states = active_captures();
        states.erase(std::remove(states.begin(), states.end(), state.get()), states.end());
    }
    state->hook_installed.store(false);
    state->running.store(false);
}

    /// Installs the dictation hook once and points it at the current gestures. The
    /// sink is marshalled to the pump thread, because the recording machine is
    /// thread-affine and the hook runs on its own thread.

} // namespace

void Win32HotkeyService::Impl::ensure_dictation_hook(const HotkeyGesture& record, const HotkeyGesture& cancel)
{
    {
        if (dictation_state == nullptr) {
            auto created = std::make_shared<CaptureState>();
            created->dictation = true;
            created->stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (created->stop_event == nullptr) {
                return;
            }
            const std::shared_ptr<CaptureState> hook_state = created;
            try {
                dictation_thread = std::thread(dictation_hook_main, hook_state);
            } catch (...) {
                // The state owns the event handle; dropping the pointer releases it.
                return;
            }
            dictation_state = created;
        }
        const std::shared_ptr<PumpState> pump_state = state;
        auto hook_state = std::static_pointer_cast<CaptureState>(dictation_state);
        const std::lock_guard<std::mutex> lock(hook_state->mutex);
        hook_state->record_gesture = record;
        hook_state->cancel_gesture = cancel;
        hook_state->on_action = PumpActionSink{pump_state};
    }
}

    /// True when the hook is really installed. Bounded wait: the install happens on
    /// the hook thread, and a refusal must fall back to RegisterHotKey instead of
    /// silently producing no events at all.
bool Win32HotkeyService::Impl::dictation_hook_ready()
{
    if (dictation_state == nullptr) {
        return false;
    }
    auto hook_state = std::static_pointer_cast<CaptureState>(dictation_state);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!hook_state->hook_installed.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < deadline) {
        ::Sleep(1);
    }
    return hook_state->hook_installed.load(std::memory_order_acquire);
}

void Win32HotkeyService::Impl::stop_dictation_hook()
{
    if (dictation_state == nullptr) {
        return;
    }
    const std::shared_ptr<CaptureState> hook_state = std::static_pointer_cast<CaptureState>(dictation_state);
    dictation_state.reset();
        hook_state->stop_requested.store(true, std::memory_order_release);
        if (hook_state->stop_event != nullptr) {
            ::SetEvent(hook_state->stop_event);
        }
        const DWORD id = hook_state->thread_id.load();
        if (id != 0) {
            ::PostThreadMessageW(id, kWmQuit, 0, 0);
        }
        if (dictation_thread.joinable()) {
            if (dictation_thread.get_id() == std::this_thread::get_id()) {
                dictation_thread.detach();
            } else {
                dictation_thread.join();
            }
        }
    }

struct HotkeyCaptureHook::Impl {
    std::shared_ptr<CaptureState> state = std::make_shared<CaptureState>();
    std::thread thread;
    HotkeyCaptureHook::ModifierRequiredCallback on_modifier_required;
    std::chrono::milliseconds shutdown_timeout{std::chrono::milliseconds(kDefaultShutdownTimeoutMs)};
    std::atomic<bool> started{false};

    explicit Impl(HotkeyCaptureHook::ModifierRequiredCallback callback)
        : on_modifier_required(std::move(callback))
    {
    }

    ~Impl()
    {
        stop();
    }

    Status start()
    {
        stop();

        state = std::make_shared<CaptureState>();
        state->on_modifier_required = on_modifier_required;
        state->shutdown_timeout = shutdown_timeout;
        state->stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (state->stop_event == nullptr) {
            return Status::failure(
                ErrorCode::resource_exhausted,
                "CreateEventW failed with Win32 error " + std::to_string(::GetLastError()));
        }
        try {
            // A local copy: the thread must own the state, not borrow it.
            const std::shared_ptr<CaptureState> capture_state = state;
            thread = std::thread([capture_state]() { capture_main(capture_state); });
        } catch (...) {
            return Status::failure(ErrorCode::resource_exhausted, "the hotkey capture thread could not be started");
        }
        started.store(true);

        // Bounded wait for the install to be decided. A capture whose thread died
        // must not leave the caller on a promise that can never complete
        // (HotkeyCaptureHook.cs:218-234 and the _tcs = null in Stop()).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        for (;;) {
            if (state->hook_installed.load()) {
                return Status::success();
            }
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                if (state->completed) {
                    const Error failure = state->failure;
                    lock.unlock();
                    stop();
                    return failure.is_ok()
                        ? Status::failure(ErrorCode::internal, "the capture ended without a result")
                        : Status::failure(failure);
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                stop();
                return Status::failure(
                    ErrorCode::timeout, "the WH_KEYBOARD_LL hook was not installed within 2000 ms");
            }
            ::Sleep(1);
        }
    }

    Status stop()
    {
        if (!started.exchange(false)) {
            return Status::success();
        }
        auto state_copy = state;
        state_copy->stop_requested.store(true, std::memory_order_release);
        if (state_copy->stop_event != nullptr) {
            ::SetEvent(state_copy->stop_event);
        }
        // Unconditional: the thread id is published before the hook is installed
        // and the queue exists from the first instruction of the thread, so
        // WM_QUIT cannot be lost because the thread was "not yet pumping".
        const DWORD id = state_copy->thread_id.load();
        if (id != 0) {
            ::PostThreadMessageW(id, kWmQuit, 0, 0);
        }
        {
            const std::lock_guard<std::mutex> lock(state_copy->mutex);
            if (!state_copy->completed) {
                state_copy->completed = true;
                state_copy->has_result = false;
            }
        }
        state_copy->settled.notify_all();

        if (!thread.joinable()) {
            return Status::success();
        }
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
            return Status::success();
        }
        const auto deadline = std::chrono::steady_clock::now() + state_copy->shutdown_timeout;
        while (state_copy->running.load()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                // Bounded: never hang a UI on a wedged hook thread. The thread
                // owns its state through the shared_ptr, so detaching is safe
                // and it exits within one 25 ms tick.
                thread.detach();
                return Status::failure(
                    ErrorCode::timeout,
                    "the hotkey capture thread did not join within "
                        + std::to_string(state_copy->shutdown_timeout.count()) + " ms");
            }
            ::Sleep(1);
        }
        thread.join();
        return Status::success();
    }
};

HotkeyCaptureHook::HotkeyCaptureHook(ModifierRequiredCallback on_modifier_required)
    : impl_(std::make_unique<Impl>(std::move(on_modifier_required)))
{
}

HotkeyCaptureHook::~HotkeyCaptureHook()
{
    if (impl_ != nullptr) {
        impl_->stop();
    }
}

Status HotkeyCaptureHook::start()
{
    return impl_->start();
}

bool HotkeyCaptureHook::is_running() const noexcept
{
    return impl_ != nullptr && impl_->started.load() && impl_->state->running.load();
}

bool HotkeyCaptureHook::hook_installed() const noexcept
{
    return impl_ != nullptr && impl_->state->hook_installed.load();
}

std::uint32_t HotkeyCaptureHook::thread_id() const noexcept
{
    return impl_ == nullptr ? 0u : impl_->state->thread_id.load();
}

void HotkeyCaptureHook::set_shutdown_timeout(std::chrono::milliseconds timeout) noexcept
{
    if (impl_ != nullptr) {
        impl_->shutdown_timeout = timeout;
        impl_->state->shutdown_timeout = timeout;
    }
}

Result<HotkeyGesture> HotkeyCaptureHook::capture_next(const CancellationToken& cancellation)
{
    if (impl_ == nullptr) {
        return Result<HotkeyGesture>::failure(ErrorCode::invalid_state, "capture hook is not constructed");
    }
    if (impl_->thread.joinable() && impl_->thread.get_id() == std::this_thread::get_id()) {
        return Result<HotkeyGesture>::failure(
            ErrorCode::invalid_state, "capture_next() must not be called on the hook thread");
    }
    if (!impl_->started.load()) {
        return Result<HotkeyGesture>::failure(
            ErrorCode::invalid_state, "start() must succeed before capture_next()");
    }

    const auto state = impl_->state;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            const bool answered = state->settled.wait_for(
                lock, std::chrono::milliseconds(50), [&state]() { return state->completed; });
            if (answered) {
                if (!state->failure.is_ok()) {
                    return Result<HotkeyGesture>(state->failure);
                }
                if (state->has_result) {
                    return state->gesture;
                }
                return Result<HotkeyGesture>::failure(ErrorCode::cancelled, "capture cancelled with Escape");
            }
        }
        if (cancellation.is_cancellation_requested()) {
            return Result<HotkeyGesture>::failure(ErrorCode::cancelled, "capture cancelled");
        }
        if (!impl_->started.load()) {
            return Result<HotkeyGesture>::failure(ErrorCode::cancelled, "capture stopped");
        }
    }
}

Status HotkeyCaptureHook::stop()
{
    return impl_->stop();
}

} // namespace voicetyper::platform::win32
