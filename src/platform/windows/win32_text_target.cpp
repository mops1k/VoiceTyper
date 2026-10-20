#include "platform/windows/win32_text_target.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// TOKEN_QUERY, TokenIntegrityLevel and the SID layout need the security header,
// which windows.h does not pull in (same as win32_paste.cpp).
#include <winnt.h>
// The UI Automation client API (CLSID_CUIAutomation, IUIAutomation,
// IUIAutomationElement and the pattern-availability property ids).
#include <uiautomation.h>
#else
#include <cstdint>
#endif

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace voicetyper::platform {

#if defined(_WIN32)

namespace {

// The delivery check's window and poll intervals live in the header
// (kDirectDeliveryConfirmationWindow, kDirectDeliveryFastPhase, ...) because the
// live platform contract measures them.

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** put()
    {
        reset();
        return &pointer_;
    }
    T* get() const { return pointer_; }
    T* operator->() const { return pointer_; }
    explicit operator bool() const { return pointer_ != nullptr; }

    void reset()
    {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_ = nullptr;
};

bool read_bool(IUIAutomationElement* element, PROPERTYID id, bool* out)
{
    VARIANT value;
    ::VariantInit(&value);
    if (FAILED(element->GetCurrentPropertyValue(id, &value))) {
        ::VariantClear(&value);
        return false;
    }
    bool ok = false;
    if (value.vt == VT_BOOL) {
        *out = value.boolVal == VARIANT_TRUE;
        ok = true;
    }
    ::VariantClear(&value);
    return ok;
}

bool read_long(IUIAutomationElement* element, PROPERTYID id, long* out)
{
    VARIANT value;
    ::VariantInit(&value);
    if (FAILED(element->GetCurrentPropertyValue(id, &value))) {
        ::VariantClear(&value);
        return false;
    }
    bool ok = false;
    if (value.vt == VT_I4) {
        *out = value.lVal;
        ok = true;
    } else if (value.vt == VT_UI4) {
        *out = static_cast<long>(value.ulVal);
        ok = true;
    } else if (value.vt == VT_I2) {
        *out = value.iVal;
        ok = true;
    }
    ::VariantClear(&value);
    return ok;
}

/// Reads the elements traits and classifies them (VT-PLT-507). The caret
/// rectangle is filled when the GUI thread of the foreground window reports one;
/// the coordinates are the screen ones (MapWindowPoints from the caret window's
/// client space), best effort, for a caller that wants to position something
/// near the caret.
void read_and_classify(IUIAutomation* uia, TextTargetInfo* info, unsigned long* last_error)
{
    info->kind = TextTargetKind::unknown;
    info->reason = "no_focused_element";
    info->window = 0;
    info->has_caret_rect = false;

    ComPtr<IUIAutomationElement> element;
    const HRESULT hr = uia->GetFocusedElement(element.put());
    if (FAILED(hr) || !element) {
        *last_error = static_cast<unsigned long>(hr);
        return;
    }

    FocusedElementTraits traits;
    long value = 0;
    traits.has_control_type = read_long(element.get(), UIA_ControlTypePropertyId, &value);
    if (traits.has_control_type) {
        traits.control_type = value;
    }
    read_bool(element.get(), UIA_IsPasswordPropertyId, &traits.password);
    read_bool(element.get(), UIA_IsTextPatternAvailablePropertyId, &traits.text_pattern);
    read_bool(element.get(), UIA_IsValuePatternAvailablePropertyId, &traits.value_pattern);
    traits.value_read_only_known =
        read_bool(element.get(), UIA_ValueIsReadOnlyPropertyId, &traits.value_read_only);
    traits.legacy_role_known = read_long(element.get(), UIA_LegacyIAccessibleRolePropertyId, &value);
    if (traits.legacy_role_known) {
        traits.legacy_role = value;
    }
    read_long(element.get(), UIA_LegacyIAccessibleStatePropertyId, &traits.legacy_state);

    TextTargetInfo classified = classify_focused_element(traits);

    long native_window = 0;
    if (read_long(element.get(), UIA_NativeWindowHandlePropertyId, &native_window) && native_window > 0) {
        classified.window = static_cast<std::uintptr_t>(native_window);
    }

    // The Win32 caret is only a bonus: Chromium does not expose one, which is
    // exactly why the verdict above does not depend on it.
    GUITHREADINFO gui{};
    gui.cbSize = sizeof(GUITHREADINFO);
    if (::GetGUIThreadInfo(0, &gui) && gui.hwndCaret != nullptr) {
        POINT corners[2] = {{gui.rcCaret.left, gui.rcCaret.top},
                            {gui.rcCaret.right, gui.rcCaret.bottom}};
        // Best effort: a failure leaves the client-space values in place, and the
        // caret rectangle is not part of any delivery decision.
        ::MapWindowPoints(gui.hwndCaret, nullptr, corners, 2);
        classified.has_caret_rect = true;
        classified.caret_x = corners[0].x;
        classified.caret_y = corners[0].y;
        classified.caret_width = corners[1].x - corners[0].x;
        classified.caret_height = corners[1].y - corners[0].y;
    }

    *info = classified;
}

struct ProbeOutcome {
    TextTargetInfo info;
    unsigned long last_error = 0;
};

/// The whole COM conversation of one probe. Runs on its own MTA thread: UI
/// Automation needs an apartment, and the caller may be an STA UI thread where
/// CoInitializeEx would be refused with RPC_E_CHANGED_MODE.
void run_probe(ProbeOutcome* outcome)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = SUCCEEDED(com);

    ComPtr<IUIAutomation> uia;
    const HRESULT created = ::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_IUIAutomation,
                                               reinterpret_cast<void**>(uia.put()));
    if (FAILED(created) || !uia) {
        outcome->info.reason = "uia_unavailable";
        outcome->last_error = static_cast<unsigned long>(created);
        if (com_owned) {
            ::CoUninitialize();
        }
        return;
    }

    // A provider that does not answer must not stall the probe: UI Automation
    // waits 60 s by default.
    ComPtr<IUIAutomation2> uia2;
    if (SUCCEEDED(uia->QueryInterface(IID_IUIAutomation2, reinterpret_cast<void**>(uia2.put()))) && uia2) {
        uia2->put_ConnectionTimeout(
            static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(kTextTargetProbeTimeout).count()));
        uia2->put_TransactionTimeout(
            static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(kTextTargetProbeTimeout).count()));
    }

    TextTargetInfo first;
    read_and_classify(uia.get(), &first, &outcome->last_error);

    if (first.kind == TextTargetKind::editable) {
        // VT-PLT-508, amendment 2026-10-19: a first `editable` is already a positive
        // answer and does not need the wait. The second query exists for the verdicts
        // the first one cannot be trusted with - Chromium builds its accessibility tree
        // while answering, so an `unknown`/`none`/`read_only` answer may describe a tree
        // that did not exist yet. Skipping the 150 ms pause exactly where the first
        // answer was already positive took the probe from 165-206 ms to 15-56 ms per
        // dictation in the live run of 2026-10-19, while every difficult case keeps the
        // old two-query path.
        outcome->info = first;
    } else {
        std::this_thread::sleep_for(kTextTargetReprobeDelay);

        TextTargetInfo second;
        unsigned long second_error = 0;
        read_and_classify(uia.get(), &second, &second_error);

        // The second verdict wins when the two disagree, so the caller acts on the tree
        // the user is actually typing into.
        if (second.kind != first.kind) {
            outcome->info = second;
            outcome->last_error = second_error;
        } else {
            outcome->info = first;
        }
    }

    if (com_owned) {
        ::CoUninitialize();
    }
}

// ------------------------------------------------------------------ integrity ---
//
// UIPI blocks synthetic input into a window of a higher integrity level, and
// SendInput does not say why it failed (its return value and GetLastError are
// documented as not reporting UIPI). The classification therefore has to be read
// from the tokens, exactly as the paste backend does before it sends Ctrl+V.
// The rule is duplicated from win32_paste.cpp on purpose: the paste copy is
// private to that translation unit and is already pinned by the Windows contract
// test, so extracting it would be a blind refactor of covered code.

std::uint32_t integrity_rid_from_token(HANDLE token) noexcept
{
    DWORD size = 0;
    static_cast<void>(::GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size));
    if (size == 0) {
        return 0;
    }
    std::vector<std::uint8_t> buffer(size);
    if (!::GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
        return 0;
    }
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    if (label->Label.Sid == nullptr) {
        return 0;
    }
    const auto* count = ::GetSidSubAuthorityCount(label->Label.Sid);
    if (count == nullptr || *count == 0) {
        return 0;
    }
    const DWORD* rid = ::GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(*count - 1));
    return rid == nullptr ? 0 : *rid;
}

std::uint32_t process_integrity_rid(DWORD process_id) noexcept
{
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        return 0;
    }
    HANDLE raw_token = nullptr;
    const BOOL opened = ::OpenProcessToken(process, TOKEN_QUERY, &raw_token);
    ::CloseHandle(process);
    if (!opened || raw_token == nullptr) {
        return 0;
    }
    const std::uint32_t rid = integrity_rid_from_token(raw_token);
    ::CloseHandle(raw_token);
    return rid;
}

/// True only when the answer is certain: the read-only probe could read both
/// integrity levels and the target sits strictly above ours.
bool focused_window_is_above_our_integrity() noexcept
{
    const HWND window = ::GetForegroundWindow();
    if (window == nullptr) {
        return false;
    }
    DWORD process_id = 0;
    if (::GetWindowThreadProcessId(window, &process_id) == 0 || process_id == 0) {
        return false;
    }
    if (process_id == ::GetCurrentProcessId()) {
        return false;
    }
    const std::uint32_t own = process_integrity_rid(::GetCurrentProcessId());
    const std::uint32_t target = process_integrity_rid(process_id);
    if (own == 0 || target == 0) {
        return false;
    }
    return target > own;
}

std::wstring utf8_to_utf16(std::string_view utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                          wide.data(), needed);
    return wide;
}

std::string utf16_to_utf8(std::wstring_view wide)
{
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed,
                          nullptr, nullptr);
    return utf8;
}

/// Window classes of the consoles measured to ignore a posted `WM_CHAR`: legacy
/// conhost and a Windows Terminal window. The terminal's own text element reports
/// `TermControl` through UI Automation rather than as an HWND class, so it is named
/// here as well.
bool class_name_is_console(std::wstring_view class_name) noexcept
{
    // `ConsoleWindowClass` is legacy conhost; `TermControl` is the UI Automation class
    // of a Windows Terminal text element (measured 2026-10-19), never an HWND class.
    return class_name == L"ConsoleWindowClass" || class_name == L"TermControl";
}

/// Hosts whose focused HWND only says "a XAML surface": the terminal element behind it
/// has to be asked for by name. Measured 2026-10-19, Windows Terminal's focused HWND
/// reports `Windows.UI.Input.InputSite.WindowClass`. Every other class - a classic
/// `EDIT`, a `RICHEDIT50W`, a Chromium window - is answered by the cheap class name
/// alone, which keeps the extra UI Automation query out of the delivery path.
bool host_needs_element_class(std::wstring_view class_name) noexcept
{
    return class_name == L"Windows.UI.Input.InputSite.WindowClass"
        || class_name == L"WinUIDesktopWin32WindowClass"
        || class_name == L"CASCADIA_HOSTING_WINDOW_CLASS";
}

/// The control that currently owns the keyboard focus, as this process can see it. Posted text goes
/// here: the same window the keyboard path would type into.
HWND focused_control() noexcept
{
    const HWND foreground = ::GetForegroundWindow();
    if (foreground == nullptr) {
        return nullptr;
    }
    DWORD process = 0;
    const DWORD thread = ::GetWindowThreadProcessId(foreground, &process);
    GUITHREADINFO info {};
    info.cbSize = sizeof(info);
    if (thread != 0 && ::GetGUIThreadInfo(thread, &info) != 0 && info.hwndFocus != nullptr) {
        return info.hwndFocus;
    }
    return foreground;
}

/// The UIA client used by the delivery verification. Creating one per injection would cost more than
/// the check itself, and UIA wants a consistent apartment per thread, so it lives in a thread_local
/// and is intentionally never released: it belongs to the delivery thread. Timeouts are short on
/// purpose - the verification runs on the delivery thread and must never become the reason a
/// dictation stalls.
IUIAutomation* verification_automation() noexcept
{
    static thread_local IUIAutomation* automation = nullptr;
    if (automation != nullptr) {
        return automation;
    }
    // An apartment that already exists is fine (the object works in both); a failure here only means
    // COM cannot be used on this thread, and then there is nothing to verify with.
    (void)::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* created = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
            reinterpret_cast<void**>(&created)))
        || created == nullptr) {
        return nullptr;
    }
    IUIAutomation2* timed = nullptr;
    if (SUCCEEDED(created->QueryInterface(
            IID_IUIAutomation2, reinterpret_cast<void**>(&timed)))
        && timed != nullptr) {
        timed->put_ConnectionTimeout(200);
        timed->put_TransactionTimeout(200);
        timed->Release();
    }
    automation = created;
    return automation;
}

/// The focused control's window class, or an empty view when it cannot be read.
std::wstring_view window_class_name(HWND window, wchar_t* buffer, std::size_t capacity) noexcept
{
    const int length = ::GetClassNameW(window, buffer, static_cast<int>(capacity));
    if (length <= 0) {
        return {};
    }
    return std::wstring_view(buffer, static_cast<std::size_t>(length));
}

/// True for the shapes measured to ignore a posted character: a console takes every
/// message and changes nothing (2026-10-16, legacy conhost) and Windows Terminal does
/// the same with its `TermControl` element (2026-10-19). Skipping the post there saves
/// the whole confirmation window (VT-PLT-519) - no burst can ever pass that check, and
/// the clipboard path takes over anyway (VT-PLT-517).
bool is_console_target(HWND control) noexcept
{
    wchar_t buffer[64] {};
    const std::wstring_view control_class = window_class_name(control, buffer, std::size(buffer));
    if (class_name_is_console(control_class)) {
        return true;
    }
    if (!host_needs_element_class(control_class)) {
        return false;
    }
    IUIAutomation* automation = verification_automation();
    if (automation == nullptr) {
        return false;
    }
    ComPtr<IUIAutomationElement> element;
    if (FAILED(automation->GetFocusedElement(element.put())) || !element) {
        return false;
    }
    BSTR name = nullptr;
    if (FAILED(element->get_CurrentClassName(&name)) || name == nullptr) {
        return false;
    }
    const bool console = class_name_is_console(std::wstring_view(name, ::SysStringLen(name)));
    ::SysFreeString(name);
    return console;
}

/// How many characters of the field the delivery check reads. The check only has to see the end of
/// what was posted, and reading costs time proportional to the text: measured 2026-10-16, a legacy
/// `EDIT` control answered about 10 ms per character, so reading a 300-character field took five
/// seconds. A bounded tail keeps the check cheap on a large document and on a console buffer.
constexpr std::size_t kVerificationTailUnits = 32;

/// How many text-capable candidates the window search examines before saying "there is
/// no field here" (VT-PLT-518). Bounded because the answer only decides whether the user
/// is warned about an unreadable paste, and the search must not become a stall.
constexpr int kTextInputSearchLimit = 12;

/// The characters the caret sits after, read through `TextPattern`. That is exactly where a delivery
/// lands, so a bounded read is enough to see it. False when the target has no text pattern, no
/// selection, or nothing to anchor the read at.
bool read_caret_tail_wide(std::size_t units, std::wstring* out) noexcept
{
    out->clear();
    IUIAutomation* automation = verification_automation();
    if (automation == nullptr) {
        return false;
    }
    ComPtr<IUIAutomationElement> element;
    if (FAILED(automation->GetFocusedElement(element.put())) || !element) {
        return false;
    }
    ComPtr<IUIAutomationTextPattern> pattern;
    if (FAILED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
            reinterpret_cast<void**>(pattern.put())))
        || !pattern) {
        return false;
    }
    ComPtr<IUIAutomationTextRangeArray> selection;
    if (FAILED(pattern->GetSelection(selection.put())) || !selection) {
        return false;
    }
    int count = 0;
    if (FAILED(selection->get_Length(&count)) || count < 1) {
        return false;
    }
    ComPtr<IUIAutomationTextRange> range;
    if (FAILED(selection->GetElement(0, range.put())) || !range) {
        return false;
    }
    // Collapse the selection to its end (the caret after a delivery) and pull its start back over the
    // characters that were just written there.
    if (FAILED(range->MoveEndpointByRange(
            TextPatternRangeEndpoint_Start, range.get(), TextPatternRangeEndpoint_End))) {
        return false;
    }
    if (units > 0) {
        int moved = 0;
        if (FAILED(range->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character,
                -static_cast<int>(units), &moved))) {
            return false;
        }
    }
    BSTR content = nullptr;
    if (FAILED(range->GetText(-1, &content)) || content == nullptr) {
        return false;
    }
    *out = std::wstring(content, ::SysStringLen(content));
    ::SysFreeString(content);
    return true;
}

/// The focused element's whole value, through `ValuePattern` (classic edit controls, and the fields
/// of some applications). Used only when the target has no text pattern to anchor the bounded read.
bool read_focused_value_wide(std::wstring* out) noexcept
{
    out->clear();
    IUIAutomation* automation = verification_automation();
    if (automation == nullptr) {
        return false;
    }
    ComPtr<IUIAutomationElement> element;
    if (FAILED(automation->GetFocusedElement(element.put())) || !element) {
        return false;
    }
    ComPtr<IUIAutomationValuePattern> value;
    if (FAILED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
            reinterpret_cast<void**>(value.put())))
        || !value) {
        return false;
    }
    BSTR current = nullptr;
    if (FAILED(value->get_CurrentValue(&current)) || current == nullptr) {
        return false;
    }
    *out = std::wstring(current, ::SysStringLen(current));
    ::SysFreeString(current);
    return true;
}

} // namespace

Win32TextTargetProbe::~Win32TextTargetProbe() = default;

bool Win32TextTargetProbe::is_supported() const noexcept
{
    return true;
}

unsigned long Win32TextTargetProbe::last_error() const noexcept
{
    return last_error_;
}

TextTargetInfo Win32TextTargetProbe::probe()
{
    ProbeOutcome outcome;
    std::thread worker([&outcome]() { run_probe(&outcome); });
    worker.join();
    last_error_ = outcome.last_error;
    return outcome.info;
}

Win32DirectTextInjector::~Win32DirectTextInjector() = default;

bool Win32DirectTextInjector::is_supported() const noexcept
{
    return true;
}

unsigned long Win32DirectTextInjector::last_error() const noexcept
{
    return last_error_;
}

InsertResult Win32DirectTextInjector::insert(std::string_view utf8,
                                             const CancellationToken& cancellation)
{
    InsertResult result;
    last_error_ = 0;
    if (utf8.empty()) {
        return result;
    }

    if (focused_window_is_above_our_integrity()) {
        result.status = Status::failure(
            ErrorCode::permission_denied,
            "the focused window runs at a higher integrity level, so UIPI refuses synthetic input");
        return result;
    }

    const std::wstring wide = utf8_to_utf16(utf8);
    if (wide.empty()) {
        result.status =
            Status::failure(ErrorCode::unavailable, "the transcript is not valid UTF-8");
        return result;
    }

    // Delivery is a posted burst plus a check of what the field actually holds.
    //
    // Posting comes first because `PostMessageW(WM_CHAR)` never enters the keyboard queue, and a
    // queue is what turned fast synthetic typing into garbage on a slow target: measured 2026-10-16
    // on the target machine, the keyboard path needed a ~15.6 ms pause after every single character
    // to keep Notepad's editor correct (without it a 49-character sample arrived as 8 correct
    // characters followed by 41 dots, real busy waits of 0.5-2 ms did not help, and `VK_PACKET` broke
    // Cyrillic outright), while posted messages delivered exactly the same text in Notepad, in a
    // Chromium page field and in the address bar - instantly, one message per character.
    //
    // Posting is only as good as the target's willingness to read its queue, and a control can take
    // every message and show nothing: measured the same day, a console window accepted all 49
    // messages and its buffer never changed. The check therefore decides the outcome: the characters
    // the caret sits after must be the characters that were posted.
    //
    // The check is a deadline, not a fixed number of attempts (VT-PLT-519). A target exposes the
    // posted text asynchronously: measured 2026-10-20 on the target machine, genuine confirmations
    // arrived 13-360 ms after the post, while the old 12 x 20 ms budget ended at ~350 ms - just
    // before the field caught up. Those bursts were reported as "delivered nothing", the caller took
    // its clipboard fallback (buffer + Ctrl+V) and inserted the transcript twice. A burst that was
    // posted is therefore never reported as "delivered nothing": it is a possible partial delivery
    // (`via == InsertVia::unconfirmed`), and the caller must not paste over it.
    const HWND target = focused_control();
    if (target == nullptr) {
        result.status =
            Status::failure(ErrorCode::unavailable, "no focused control to deliver the text to");
        return result;
    }
    if (is_console_target(target)) {
        // Measured, not guessed: a console accepts every posted character and shows
        // none of them, so the honest answer is "this backend cannot deliver here" and
        // the caller's clipboard path (which now confirms itself, VT-DOM-618) delivers.
        result.status = Status::failure(ErrorCode::unavailable,
            "the focused control is a console window, which ignores posted characters");
        return result;
    }

    const std::size_t tail_units = std::min(kVerificationTailUnits, wide.size());
    const std::wstring expected_tail = wide.substr(wide.size() - tail_units);

    // Two readable shapes can carry a check: a text pattern with a caret (documents, Chromium fields,
    // consoles) or a value (classic edits). Without either there is no way to tell a delivery from a
    // silent ignore, and the caller's clipboard path is the honest answer. This refusal happens
    // before anything is posted, so it is safe for that path to run.
    std::wstring probe;
    const bool caret_readable = read_caret_tail_wide(0, &probe);
    std::wstring value_before;
    const bool value_readable = !caret_readable && read_focused_value_wide(&value_before);
    if (!caret_readable && !value_readable) {
        result.status = Status::failure(ErrorCode::unavailable,
            "the focused field exposes no text to verify a posted delivery against");
        return result;
    }

    std::size_t typed_units = 0;
    for (const wchar_t unit : wide) {
        if (cancellation.is_cancellation_requested()) {
            result.typed = typed_units;
            result.via = typed_units > 0 ? InsertVia::posted : InsertVia::none;
            result.status = Status::failure(ErrorCode::cancelled, "direct insertion was cancelled");
            return result;
        }
        if (::PostMessageW(target, WM_CHAR, static_cast<WPARAM>(unit), 0) == 0) {
            last_error_ = ::GetLastError();
            // The prefix is already in the target's queue. Reporting `typed = 0` here made the
            // caller paste the whole transcript over it; the partial result keeps that from
            // happening (VT-PLT-515).
            result.typed = typed_units;
            result.via = typed_units > 0 ? InsertVia::posted : InsertVia::none;
            result.status = Status::failure(ErrorCode::unavailable,
                "the focused control refused a posted character (error "
                    + std::to_string(last_error_) + ")");
            return result;
        }
        ++typed_units;
    }

    // The target reads its queue on its own thread and republishes its accessible text on its own
    // schedule, so the check waits for the field to catch up. A healthy target answers on the first
    // reads; after kDirectDeliveryFastPhase the polling turns sparse so it does not steal the time
    // the target needs to update its tree.
    const auto check_started = std::chrono::steady_clock::now();
    const auto check_deadline = check_started + kDirectDeliveryConfirmationWindow;
    for (;;) {
        if (cancellation.is_cancellation_requested()) {
            // The whole burst may already be posted, so this is a partial delivery: the
            // caller keeps the clipboard path but sends no paste (VT-PLT-515).
            result.typed = typed_units;
            result.via = InsertVia::posted;
            result.status = Status::failure(ErrorCode::cancelled, "direct insertion was cancelled");
            return result;
        }
        bool delivered = false;
        if (caret_readable) {
            std::wstring tail;
            delivered = read_caret_tail_wide(tail_units, &tail)
                && verification_tail_matches(expected_tail, tail);
        } else {
            std::wstring now;
            delivered = read_focused_value_wide(&now) && now != value_before
                && verification_value_contains(now, wide);
        }
        if (delivered) {
            result.typed = typed_units;
            result.via = InsertVia::posted;
            return result;
        }
        const auto checked = std::chrono::steady_clock::now();
        if (checked >= check_deadline) {
            break;
        }
        auto interval = checked - check_started < kDirectDeliveryFastPhase
            ? kDirectDeliveryFastInterval
            : kDirectDeliverySlowInterval;
        const auto remaining = check_deadline - checked;
        if (remaining < interval) {
            interval = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        }
        if (interval > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(interval);
        }
    }

    // Every unit was posted, so the text may well be in the field: the caller MUST NOT paste over
    // it (that fallback is exactly the 2026-10-20 duplicate). The transcript stays recoverable
    // through the clipboard path the caller runs instead.
    result.typed = typed_units;
    result.via = InsertVia::unconfirmed;
    result.status = Status::failure(ErrorCode::unavailable,
        "the posted text was not confirmed by the focused field in time, so the delivery may already "
        "have happened");
    return result;
}

Win32FocusedTextField::~Win32FocusedTextField() = default;

bool Win32FocusedTextField::is_supported() const noexcept
{
    return true;
}

bool Win32FocusedTextField::read_caret_tail(std::size_t units, std::string* utf8)
{
    std::wstring wide;
    if (!read_caret_tail_wide(units, &wide)) {
        return false;
    }
    *utf8 = utf16_to_utf8(wide);
    return true;
}

bool Win32FocusedTextField::read_value(std::string* utf8)
{
    std::wstring wide;
    if (!read_focused_value_wide(&wide)) {
        return false;
    }
    *utf8 = utf16_to_utf8(wide);
    return true;
}

bool Win32FocusedTextField::selection_present()
{
    // VT-DOM-620. A delivery replaces a non-empty selection, so the characters before
    // the caret's end are not the characters the transcript continues. Only a selection
    // this reader can compare is answered with false; every "cannot tell" is true.
    IUIAutomation* automation = verification_automation();
    if (automation == nullptr) {
        return true;
    }
    ComPtr<IUIAutomationElement> element;
    if (FAILED(automation->GetFocusedElement(element.put())) || !element) {
        return true;
    }
    ComPtr<IUIAutomationTextPattern> pattern;
    if (FAILED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
            reinterpret_cast<void**>(pattern.put())))
        || !pattern) {
        // A classic EDIT exposes no text pattern: whether one of its characters is
        // selected cannot be read here, and the caller must not guess.
        return true;
    }
    ComPtr<IUIAutomationTextRangeArray> selection;
    if (FAILED(pattern->GetSelection(selection.put())) || !selection) {
        return true;
    }
    int count = 0;
    if (FAILED(selection->get_Length(&count)) || count < 1) {
        return true;
    }
    ComPtr<IUIAutomationTextRange> range;
    if (FAILED(selection->GetElement(0, range.put())) || !range) {
        return true;
    }
    int compared = 0;
    if (FAILED(range->CompareEndpoints(TextPatternRangeEndpoint_Start, range.get(),
            TextPatternRangeEndpoint_End, &compared))) {
        return true;
    }
    return compared != 0;
}

bool Win32FocusedTextField::has_text_input_in_foreground_window()
{
    // VT-PLT-518. The search is bounded on purpose: it runs only when the paste could
    // not be read back, and it must never become the reason a dictation stalls.
    const HWND foreground = ::GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    IUIAutomation* automation = verification_automation();
    if (automation == nullptr) {
        // Cannot ask: staying silent is the conservative answer.
        return true;
    }
    ComPtr<IUIAutomationElement> window;
    if (FAILED(automation->ElementFromHandle(foreground, window.put())) || !window) {
        return true;
    }
    const auto by_control_type = [automation](long type, ComPtr<IUIAutomationCondition>* out) {
        VARIANT value {};
        value.vt = VT_I4;
        value.lVal = type;
        return automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, out->put());
    };
    ComPtr<IUIAutomationCondition> edits;
    ComPtr<IUIAutomationCondition> documents;
    ComPtr<IUIAutomationCondition> texts;
    if (FAILED(by_control_type(kUiaControlTypeEdit, &edits))
        || FAILED(by_control_type(kUiaControlTypeDocument, &documents))
        || FAILED(by_control_type(kUiaControlTypeText, &texts))) {
        return true;
    }
    ComPtr<IUIAutomationCondition> edit_or_document;
    if (FAILED(automation->CreateOrCondition(edits.get(), documents.get(), edit_or_document.put()))
        || !edit_or_document) {
        return true;
    }
    ComPtr<IUIAutomationCondition> any_text;
    if (FAILED(automation->CreateOrCondition(edit_or_document.get(), texts.get(), any_text.put()))
        || !any_text) {
        return true;
    }
    ComPtr<IUIAutomationElementArray> found;
    if (FAILED(window->FindAll(TreeScope_Descendants, any_text.get(), found.put())) || !found) {
        return true;
    }
    int count = 0;
    if (FAILED(found->get_Length(&count)) || count <= 0) {
        return false;
    }
    const int examined = count < kTextInputSearchLimit ? count : kTextInputSearchLimit;
    for (int index = 0; index < examined; ++index) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(found->GetElement(index, element.put())) || !element) {
            continue;
        }
        ComPtr<IUIAutomationTextPattern> text_pattern;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                reinterpret_cast<void**>(text_pattern.put())))
            && text_pattern) {
            return true;
        }
        ComPtr<IUIAutomationValuePattern> value_pattern;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                reinterpret_cast<void**>(value_pattern.put())))
            && value_pattern) {
            return true;
        }
    }
    return false;
}

#else // !_WIN32

// The off-Windows definitions exist so this translation unit stays compilable
// everywhere, exactly like win32_paste.cpp: a host that builds it gets the
// documented "no backend here" answer instead of a link error.
Win32TextTargetProbe::~Win32TextTargetProbe() = default;

bool Win32TextTargetProbe::is_supported() const noexcept
{
    return false;
}

unsigned long Win32TextTargetProbe::last_error() const noexcept
{
    return 0;
}

TextTargetInfo Win32TextTargetProbe::probe()
{
    TextTargetInfo info;
    info.kind = TextTargetKind::unknown;
    info.reason = "unsupported_platform";
    return info;
}

Win32DirectTextInjector::~Win32DirectTextInjector() = default;

bool Win32DirectTextInjector::is_supported() const noexcept
{
    return false;
}

unsigned long Win32DirectTextInjector::last_error() const noexcept
{
    return 0;
}

InsertResult Win32DirectTextInjector::insert(std::string_view, const CancellationToken&)
{
    InsertResult result;
    result.status = Status::failure(ErrorCode::unavailable,
                                    "direct insertion is a Windows-only backend");
    return result;
}

Win32FocusedTextField::~Win32FocusedTextField() = default;

bool Win32FocusedTextField::is_supported() const noexcept
{
    return false;
}

bool Win32FocusedTextField::read_caret_tail(std::size_t, std::string*)
{
    return false;
}

bool Win32FocusedTextField::read_value(std::string*)
{
    return false;
}

bool Win32FocusedTextField::selection_present()
{
    // No reader here: the conservative answer, so no caller guesses at a caret text.
    return true;
}

bool Win32FocusedTextField::has_text_input_in_foreground_window()
{
    return true;
}

#endif // _WIN32

} // namespace voicetyper::platform
