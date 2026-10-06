#include "platform/windows/win32_paste.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// TOKEN_QUERY, TokenIntegrityLevel and the SID layout need the security headers,
// which windows.h does not pull in.
#include <winnt.h>
#else
#include <cstdint>
#endif

#include <cstring>
#include <string>
#include <vector>

namespace voicetyper::platform {

#if defined(_WIN32)

namespace {

/// Reads the mandatory integrity RID from a token.
///
/// The label SID ends with the RID as its last subauthority, so the value is
/// taken from there instead of being compared SID-to-SID.
std::uint32_t integrity_level_from_token(HANDLE token) noexcept
{
    // A size query always returns FALSE with ERROR_INSUFFICIENT_BUFFER, so the
    // return value must be ignored; only the reported size matters. Treating the
    // FALSE as a failure would make every integrity probe read 0 and silently
    // disable the UAC classification.
    DWORD size = 0;
    static_cast<void>(::GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size));
    if (size == 0) {
        return 0;
    }
    std::vector<std::uint8_t> buffer(size);
    DWORD returned = 0;
    if (!::GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &returned)) {
        return 0;
    }
    auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
    const PSID sid = label->Label.Sid;
    if (sid == nullptr) {
        return 0;
    }
    // The RID is the last subauthority, i.e. the final four bytes of the SID. It
    // is read through GetLengthSid rather than through GetSidSubAuthority,
    // because that accessor is a macro in some SDKs and a function in others,
    // and this code has to compile identically under MinGW and MSVC.
    const DWORD length = ::GetLengthSid(sid);
    if (length < 12) {
        return 0;
    }
    std::uint32_t rid = 0;
    std::memcpy(&rid, reinterpret_cast<const std::uint8_t*>(sid) + length - sizeof(rid), sizeof(rid));
    return rid;
}

/// Closes a token handle exactly once.
class UniqueToken {
public:
    UniqueToken() = default;
    explicit UniqueToken(HANDLE handle) noexcept
        : handle_(handle)
    {
    }
    UniqueToken(const UniqueToken&) = delete;
    UniqueToken& operator=(const UniqueToken&) = delete;
    UniqueToken(UniqueToken&& other) noexcept
        : handle_(other.release())
    {
    }
    UniqueToken& operator=(UniqueToken&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = other.release();
        }
        return *this;
    }
    ~UniqueToken() { reset(); }

    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    HANDLE release() noexcept
    {
        const HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    void reset() noexcept
    {
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

private:
    HANDLE handle_ = nullptr;
};

/// True when an interactive input desktop exists. This is the "is there a
/// session to inject into" question, asked before any window lookup so a
/// service or session-0 run reports unavailable instead of a focused-window
/// mystery.
bool has_input_desktop() noexcept
{
    HDESK desktop = ::OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == nullptr) {
        return false;
    }
    ::CloseDesktop(desktop);
    return true;
}

} // namespace

std::uint32_t Win32PasteSimulator::own_integrity_level() noexcept
{
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw)) {
        return 0;
    }
    const UniqueToken token(raw);
    return integrity_level_from_token(token.get());
}

FocusedWindowProbe Win32PasteSimulator::probe_focused_window() const
{
    FocusedWindowProbe probe;

    const HWND window = ::GetForegroundWindow();
    if (window == nullptr) {
        // Nothing has focus: an unavailable injection target, not an integrity
        // problem.
        probe.last_error = ::GetLastError();
        return probe;
    }
    probe.window = reinterpret_cast<std::uintptr_t>(window);

    DWORD process_id = 0;
    const DWORD thread_id = ::GetWindowThreadProcessId(window, &process_id);
    if (thread_id == 0 || process_id == 0) {
        probe.last_error = ::GetLastError();
        return probe;
    }

    if (process_id == ::GetCurrentProcessId()) {
        probe.same_process = true;
        probe.resolved = true;
        probe.own_level = own_integrity_level();
        probe.target_level = probe.own_level;
        return probe;
    }

    probe.own_level = own_integrity_level();

    // PROCESS_QUERY_LIMITED_INFORMATION is the least privilege that can answer
    // the question. A refusal here is itself the UAC signal: a process at a
    // higher integrity level denies even a query.
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        probe.last_error = ::GetLastError();
        return probe;
    }

    HANDLE raw_token = nullptr;
    const BOOL opened = ::OpenProcessToken(process, TOKEN_QUERY, &raw_token);
    ::CloseHandle(process);
    if (!opened) {
        probe.last_error = ::GetLastError();
        return probe;
    }
    const UniqueToken token(raw_token);

    const std::uint32_t target = integrity_level_from_token(token.get());
    if (target == 0 && probe.own_level == 0) {
        probe.last_error = ::GetLastError();
        return probe;
    }
    probe.target_level = target;
    probe.resolved = true;
    probe.target_above_own = target > probe.own_level;
    return probe;
}

Win32PasteSimulator::~Win32PasteSimulator() = default;

void Win32PasteSimulator::set_suspended(bool suspended) noexcept
{
    suspended_.store(suspended, std::memory_order_release);
}

bool Win32PasteSimulator::paste_suspended() const noexcept
{
    return suspended_.load(std::memory_order_acquire);
}

bool Win32PasteSimulator::is_injection_supported() const noexcept
{
    return has_input_desktop();
}

Status Win32PasteSimulator::paste()
{
    if (suspended_.load(std::memory_order_acquire)) {
        // Hotkey capture is in progress: a synthetic Ctrl+V would be swallowed
        // by the capture hook. The transcript is already on the clipboard, so the
        // caller reports clipboard_only.
        return Status::failure(
            ErrorCode::unavailable, "paste injection is suspended while a hotkey is being captured");
    }

    if (!has_input_desktop()) {
        return Status::failure(
            ErrorCode::unavailable, "this session has no interactive input desktop");
    }

    const FocusedWindowProbe probe = probe_focused_window();
    if (probe.window == 0) {
        return Status::failure(ErrorCode::unavailable, "no window currently has focus");
    }
    if (!probe.resolved) {
        if (probe.last_error == ERROR_ACCESS_DENIED) {
            // A focused process that will not even answer a query is running at a
            // higher integrity level: the UAC case, reported as such.
            return Status::failure(
                ErrorCode::permission_denied,
                "the focused window denies a query, which means it runs at a higher integrity level");
        }
        // The integrity level is unreadable for an unrelated reason (a protected
        // process, a race with focus change). Trying anyway is correct: SendInput
        // itself is still the authority, and its refusal is reported below.
    } else if (probe.target_above_own) {
        return Status::failure(
            ErrorCode::permission_denied,
            "the focused window runs at integrity level "
                + std::to_string(probe.target_level) + ", above this process's "
                + std::to_string(probe.own_level));
    }

    INPUT events[4]{};
    events[0].type = INPUT_KEYBOARD;
    events[0].ki.wVk = VK_CONTROL;
    events[0].ki.dwExtraInfo = kWin32PasteInputTag;

    events[1].type = INPUT_KEYBOARD;
    events[1].ki.wVk = 'V';
    events[1].ki.dwExtraInfo = kWin32PasteInputTag;

    events[2].type = INPUT_KEYBOARD;
    events[2].ki.wVk = 'V';
    events[2].ki.dwFlags = KEYEVENTF_KEYUP;
    events[2].ki.dwExtraInfo = kWin32PasteInputTag;

    events[3].type = INPUT_KEYBOARD;
    events[3].ki.wVk = VK_CONTROL;
    events[3].ki.dwFlags = KEYEVENTF_KEYUP;
    events[3].ki.dwExtraInfo = kWin32PasteInputTag;

    const UINT inserted = ::SendInput(4, events, sizeof(INPUT));
    if (inserted == 4) {
        return Status::success();
    }

    const DWORD code = ::GetLastError();
    if (code == ERROR_ACCESS_DENIED) {
        return Status::failure(
            ErrorCode::permission_denied,
            "SendInput was refused by UIPI (the focused window runs elevated)");
    }
    return Status::failure(
        ErrorCode::unavailable,
        "SendInput inserted " + std::to_string(inserted) + " of 4 event(s) (win32 "
            + std::to_string(code) + ")");
}

#else // !_WIN32

// Unreachable in this build (the library target is WIN32-only). Present so an
// accidental non-Windows compile refuses explicitly instead of failing to link.
Win32PasteSimulator::~Win32PasteSimulator() = default;
Status Win32PasteSimulator::paste()
{
    return Status::failure(
        ErrorCode::unavailable, "the Win32 paste backend is not available on this platform");
}
bool Win32PasteSimulator::is_injection_supported() const noexcept { return false; }
void Win32PasteSimulator::set_suspended(bool suspended) noexcept { suspended_.store(suspended); }
bool Win32PasteSimulator::paste_suspended() const noexcept { return suspended_.load(); }
FocusedWindowProbe Win32PasteSimulator::probe_focused_window() const { return FocusedWindowProbe{}; }
std::uint32_t Win32PasteSimulator::own_integrity_level() noexcept { return 0; }

#endif // _WIN32

} // namespace voicetyper::platform
