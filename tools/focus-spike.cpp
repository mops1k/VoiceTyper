// VoiceTyper - focus spike (Windows only, diagnostic, not part of the app).
//
// Answers two questions before the production port exists:
//   1. Can an ordinary unprivileged process tell whether the keyboard focus sits
//      in a text-editable field (Notepad, a browser page, an address bar, a
//      contenteditable element, a terminal), and
//   2. can it put text into that field without touching the clipboard?
//
// Detection uses UI Automation: the focused element plus the pattern-availability
// properties (UIA_IsTextPatternAvailablePropertyId, UIA_IsValuePatternAvailablePropertyId,
// UIA_ValueIsReadOnlyPropertyId) and the legacy MSAA role/state, so the verdict does
// not depend on a Win32 caret - Chromium-based browsers (Brave included) do not expose
// one, which is why caret-based tools see nothing there.
//
// Insertion uses SendInput with KEYEVENTF_UNICODE: one event pair per UTF-16 code unit.
// It is layout independent (Cyrillic included) and never writes the clipboard, which the
// tool verifies with GetClipboardSequenceNumber().
//
// Modes:
//   focus-spike --watch [seconds]        log the verdict every 500 ms (default 30 s)
//   focus-spike --insert-after <ms>      wait, probe twice, type the sample text
//   focus-spike --hotkey                 Ctrl+Alt+Shift+F9 probes and types, repeatable
//   --watch and --insert-after may be combined with --hotkey.
//
// Options:
//   --text "<sample>"     text to type (default: the RU+EN sample below)
//   --per-char-ms <n>     pause after every typed character (default 2)
//   --log <path>"         log file (default: focus-spike.log next to the executable)
//   --help

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <uiautomation.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// ------------------------------------------------------------------ logging ---

FILE* g_log = nullptr;

std::string to_utf8(const wchar_t* text, int length = -1)
{
    if (text == nullptr) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), needed, nullptr, nullptr);
    if (length < 0 && !out.empty() && out.back() == '\0') {
        // With length -1 the conversion counted the terminator into the result;
        // leaving it in would truncate every later concatenation at fputs().
        out.pop_back();
    }
    return out;
}

std::string trim_utf8(std::string value, std::size_t limit)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
        value.pop_back();
    }
    if (value.size() > limit) {
        value.resize(limit);
        value += "...";
    }
    return value;
}

std::string hex32(unsigned long value)
{
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", value);
    return buffer;
}

std::string stamp()
{
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u.%03u", now.wHour, now.wMinute, now.wSecond,
                  now.wMilliseconds);
    return buffer;
}

void log_line(const std::string& line)
{
    const std::string out = stamp() + " " + line;
    std::fputs(out.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (g_log != nullptr) {
        std::fputs(out.c_str(), g_log);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
}

// ---------------------------------------------------------------------- COM ---

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

bool read_property(IUIAutomationElement* element, PROPERTYID id, VARIANT* out)
{
    ::VariantInit(out);
    if (element == nullptr) {
        return false;
    }
    const HRESULT hr = element->GetCurrentPropertyValue(id, out);
    if (FAILED(hr)) {
        ::VariantClear(out);
        return false;
    }
    return true;
}

bool property_bool(IUIAutomationElement* element, PROPERTYID id, bool* out)
{
    VARIANT value;
    if (!read_property(element, id, &value)) {
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

bool property_long(IUIAutomationElement* element, PROPERTYID id, long* out)
{
    VARIANT value;
    if (!read_property(element, id, &value)) {
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

std::string property_string(IUIAutomationElement* element, PROPERTYID id)
{
    VARIANT value;
    if (!read_property(element, id, &value)) {
        return {};
    }
    std::string out;
    if (value.vt == VT_BSTR && value.bstrVal != nullptr) {
        out = to_utf8(value.bstrVal);
    }
    ::VariantClear(&value);
    return out;
}

// ------------------------------------------------------------- process names ---

std::string process_name(unsigned long process_id)
{
    if (process_id == 0) {
        return {};
    }
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        return "pid " + std::to_string(process_id) + " (access denied)";
    }
    wchar_t buffer[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::string out;
    if (::QueryFullProcessImageNameW(process, 0, buffer, &size)) {
        out = to_utf8(buffer, static_cast<int>(size));
        const std::size_t slash = out.find_last_of("\\/");
        if (slash != std::string::npos) {
            out = out.substr(slash + 1);
        }
    } else {
        out = "pid " + std::to_string(process_id);
    }
    ::CloseHandle(process);
    return out;
}

std::string foreground_class()
{
    const HWND window = ::GetForegroundWindow();
    if (window == nullptr) {
        return "(no foreground window)";
    }
    wchar_t buffer[256] = {};
    ::GetClassNameW(window, buffer, 255);
    DWORD process_id = 0;
    ::GetWindowThreadProcessId(window, &process_id);
    return to_utf8(buffer) + " / " + process_name(process_id);
}

// --------------------------------------------------------------------- probe ---

struct ProbeResult {
    std::string verdict = "unknown";
    std::string reason;
    std::string dump;
};

ProbeResult probe(IUIAutomation* uia)
{
    ProbeResult result;

    ComPtr<IUIAutomationElement> element;
    const HRESULT hr = uia->GetFocusedElement(element.put());
    if (FAILED(hr) || !element) {
        result.reason = "GetFocusedElement hr=" + hex32(static_cast<unsigned long>(hr));
        result.verdict = ::GetForegroundWindow() == nullptr ? "none" : "unknown";
        result.dump = "foreground: " + foreground_class();
        return result;
    }

    long control_type = -1;
    long process_id = 0;
    long native_window = 0;
    long legacy_role = -1;
    long legacy_state = 0;
    bool has_control_type = property_long(element.get(), UIA_ControlTypePropertyId, &control_type);
    property_long(element.get(), UIA_ProcessIdPropertyId, &process_id);
    property_long(element.get(), UIA_NativeWindowHandlePropertyId, &native_window);
    const std::string localized = property_string(element.get(), UIA_LocalizedControlTypePropertyId);
    const std::string class_name = property_string(element.get(), UIA_ClassNamePropertyId);
    const std::string name = trim_utf8(property_string(element.get(), UIA_NamePropertyId), 40);

    bool password = false;
    bool enabled = false;
    bool focusable = false;
    bool focused = false;
    bool offscreen = false;
    bool text_pattern = false;
    bool value_pattern = false;
    bool value_read_only = false;
    bool value_read_only_known = false;
    property_bool(element.get(), UIA_IsPasswordPropertyId, &password);
    property_bool(element.get(), UIA_IsEnabledPropertyId, &enabled);
    property_bool(element.get(), UIA_IsKeyboardFocusablePropertyId, &focusable);
    property_bool(element.get(), UIA_HasKeyboardFocusPropertyId, &focused);
    property_bool(element.get(), UIA_IsOffscreenPropertyId, &offscreen);
    property_bool(element.get(), UIA_IsTextPatternAvailablePropertyId, &text_pattern);
    property_bool(element.get(), UIA_IsValuePatternAvailablePropertyId, &value_pattern);
    value_read_only_known = property_bool(element.get(), UIA_ValueIsReadOnlyPropertyId, &value_read_only);
    const bool legacy_role_known =
        property_long(element.get(), UIA_LegacyIAccessibleRolePropertyId, &legacy_role);
    property_long(element.get(), UIA_LegacyIAccessibleStatePropertyId, &legacy_state);

    // The sampled text field roles. ROLE_SYSTEM_TEXT (42) is the MSAA role an
    // editable text field carries; STATE_SYSTEM_READONLY (0x40) marks it read only.
    constexpr long kRoleSystemText = 42;
    constexpr long kStateSystemReadOnly = 0x40;

    // The verdict. Password fields are never a typing target. Then the strongest
    // signal: an element that offers a text pattern is text editable. Chromium
    // (Brave included) exposes the text pattern for input/textarea/contenteditable,
    // and for its web content that pattern arrives through the MSAA bridge on the
    // first query - hence the caller probes twice.
    if (password) {
        result.verdict = "read_only";
        result.reason = "password";
    } else if (text_pattern) {
        result.verdict = "editable";
        result.reason = "text_pattern";
    } else if (has_control_type
               && (control_type == UIA_EditControlTypeId || control_type == UIA_DocumentControlTypeId)) {
        result.verdict = "editable";
        result.reason = "control_type";
    } else if (value_pattern) {
        if (value_read_only_known && value_read_only) {
            result.verdict = "read_only";
            result.reason = "value_pattern_read_only";
        } else {
            result.verdict = "editable";
            result.reason = "value_pattern";
        }
    } else if (legacy_role_known) {
        if (legacy_role == kRoleSystemText) {
            if ((legacy_state & kStateSystemReadOnly) != 0) {
                result.verdict = "read_only";
                result.reason = "legacy_read_only";
            } else {
                result.verdict = "editable";
                result.reason = "legacy_text_role";
            }
        } else {
            result.verdict = "none";
            result.reason = "legacy_role_" + std::to_string(legacy_role);
        }
    } else {
        result.verdict = "none";
        result.reason = "no_text_capability";
    }

    result.dump = "element[control_type=" + (has_control_type ? std::to_string(control_type) : std::string("?"))
        + " localized=\"" + localized + "\" class=\"" + class_name + "\" name=\"" + name + "\" pid="
        + std::to_string(process_id) + " (" + process_name(static_cast<unsigned long>(process_id))
        + ") hwnd=" + std::to_string(native_window) + " enabled=" + (enabled ? "1" : "0")
        + " focusable=" + (focusable ? "1" : "0") + " focused=" + (focused ? "1" : "0")
        + " offscreen=" + (offscreen ? "1" : "0") + " password=" + (password ? "1" : "0")
        + " text_pattern=" + (text_pattern ? "1" : "0") + " value_pattern=" + (value_pattern ? "1" : "0")
        + " value_read_only=" + (value_read_only_known ? (value_read_only ? "1" : "0") : "?")
        + " legacy_role=" + (legacy_role_known ? std::to_string(legacy_role) : std::string("?"))
        + " legacy_state=" + hex32(static_cast<unsigned long>(legacy_state)) + "] foreground: "
        + foreground_class();
    return result;
}

ProbeResult probe_and_log(IUIAutomation* uia, const char* tag)
{
    const ProbeResult result = probe(uia);
    log_line(std::string(tag) + " verdict=" + result.verdict + " reason=" + result.reason);
    log_line(std::string(tag) + " " + result.dump);
    return result;
}

// ---------------------------------------------------------------- injection ---

struct InjectResult {
    std::size_t typed = 0;
    std::size_t expected = 0;
    unsigned long last_error = 0;
    double elapsed_ms = 0.0;
    bool complete() const { return typed == expected; }
};

/// A real microsecond wait. Sleep() cannot express one: measured on this machine, Sleep(1) and
/// Sleep(2) both park the thread for ~15.6 ms, the Windows timer granularity.
void spin_wait(unsigned microseconds)
{
    LARGE_INTEGER frequency {};
    LARGE_INTEGER start {};
    ::QueryPerformanceFrequency(&frequency);
    ::QueryPerformanceCounter(&start);
    const long long target =
        frequency.QuadPart * static_cast<long long>(microseconds) / 1000000LL;
    LARGE_INTEGER now {};
    do {
        ::QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < target);
}

/// The control that currently has the keyboard focus, seen from this process.
HWND focused_control()
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

/// WM_CHAR straight to the focused control: no keyboard events, so no typematic repeat can appear
/// when the target's message loop is slow. This is the alternative the Notepad finding asks for.
InjectResult post_char_text(const std::wstring& text, unsigned per_char_ms, unsigned spin_us)
{
    InjectResult result;
    result.expected = text.size();
    const HWND target = focused_control();
    if (target == nullptr) {
        result.last_error = ERROR_INVALID_WINDOW_HANDLE;
        return result;
    }

    const ULONGLONG started = ::GetTickCount64();
    for (const wchar_t character : text) {
        if (::PostMessageW(target, WM_CHAR, static_cast<WPARAM>(character), 0) == 0) {
            result.last_error = ::GetLastError();
            result.elapsed_ms = static_cast<double>(::GetTickCount64() - started);
            return result;
        }
        ++result.typed;
        if (result.typed < text.size()) {
            if (per_char_ms > 0) {
                ::Sleep(per_char_ms);
            }
            if (spin_us > 0) {
                spin_wait(spin_us);
            }
        }
    }
    result.elapsed_ms = static_cast<double>(::GetTickCount64() - started);
    return result;
}

InjectResult type_text(
    const std::wstring& text, unsigned per_char_ms, unsigned batch, unsigned spin_us, bool vk_packet)
{
    InjectResult result;
    result.expected = text.size();

    const std::size_t chunk_size = batch == 0 ? 1 : batch;
    std::vector<INPUT> inputs(chunk_size * 2);
    const ULONGLONG started = ::GetTickCount64();
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t count = std::min(chunk_size, text.size() - index);
        for (std::size_t i = 0; i < count; ++i) {
            INPUT& down = inputs[i * 2];
            INPUT& up = inputs[i * 2 + 1];
            down = INPUT{};
            up = INPUT{};
            for (INPUT* input : {&down, &up}) {
                input->type = INPUT_KEYBOARD;
                input->ki.wVk = vk_packet ? 0xE7 : 0;
                input->ki.wScan = static_cast<WORD>(text[index + i]);
                input->ki.dwFlags = KEYEVENTF_UNICODE;
            }
            up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        }

        const UINT sent = ::SendInput(static_cast<UINT>(count * 2), inputs.data(), sizeof(INPUT));
        if (sent != count * 2) {
            result.last_error = ::GetLastError();
            result.typed += sent / 2;
            result.elapsed_ms = static_cast<double>(::GetTickCount64() - started);
            return result;
        }
        index += count;
        result.typed = index;
        if (index < text.size()) {
            if (per_char_ms > 0) {
                ::Sleep(per_char_ms);
            }
            if (spin_us > 0) {
                spin_wait(spin_us);
            }
        }
    }
    result.elapsed_ms = static_cast<double>(::GetTickCount64() - started);
    return result;
}

// --------------------------------------------------------------- read-back ---

/// What the focused element holds right now: ValuePattern first (classic edits, Chromium text
/// fields), then TextPattern (documents, contenteditable). This is the only honest way to check a
/// delivery: SendInput's return value counts events the input stream accepted, not characters a
/// control stored (measured 2026-10-16: an edit control without ES_AUTOVSCROLL dropped 21 of 300
/// units while every event was reported as inserted).

struct SpellCheck {
    std::wstring value;
    std::wstring text;
    bool has_value = false;
    bool has_text = false;
    std::string describe() const
    {
        return "value=" + std::string(has_value ? "yes" : "no") + "/" + std::to_string(value.size())
            + " text=" + std::string(has_text ? "yes" : "no") + "/" + std::to_string(text.size());
    }
};

void read_by_value(IUIAutomationElement* element, SpellCheck* out);
void read_by_text(IUIAutomationElement* element, SpellCheck* out);

bool focused_check(IUIAutomation* uia, SpellCheck* out)
{
    *out = SpellCheck{};
    ComPtr<IUIAutomationElement> element;
    if (FAILED(uia->GetFocusedElement(element.put())) || !element) {
        return false;
    }
    read_by_value(element.get(), out);
    read_by_text(element.get(), out);
    return out->has_value || out->has_text;
}

/// Polls until both read-backs stop changing: injected input reaches a control asynchronously.
SpellCheck wait_for_read_back(IUIAutomation* uia)
{
    SpellCheck previous;
    SpellCheck current;
    int stable = 0;
    for (int attempt = 0; attempt < 40 && stable < 3; ++attempt) {
        current = SpellCheck{};
        focused_check(uia, &current);
        const bool same = current.value == previous.value && current.text == previous.text;
        stable = same ? stable + 1 : 0;
        previous = current;
        ::Sleep(25);
    }
    return previous;
}

void read_by_value(IUIAutomationElement* element, SpellCheck* out)
{
    ComPtr<IUIAutomationValuePattern> value;
    const HRESULT hr = element->GetCurrentPatternAs(
        UIA_ValuePatternId, IID_IUIAutomationValuePattern, reinterpret_cast<void**>(value.put()));
    if (FAILED(hr) || !value) {
        return;
    }
    BSTR current = nullptr;
    if (SUCCEEDED(value->get_CurrentValue(&current)) && current != nullptr) {
        out->value = std::wstring(current, ::SysStringLen(current));
        out->has_value = true;
        ::SysFreeString(current);
    }
}

void read_by_text(IUIAutomationElement* element, SpellCheck* out)
{
    ComPtr<IUIAutomationTextPattern> pattern;
    const HRESULT hr = element->GetCurrentPatternAs(
        UIA_TextPatternId, IID_IUIAutomationTextPattern, reinterpret_cast<void**>(pattern.put()));
    if (FAILED(hr) || !pattern) {
        return;
    }
    ComPtr<IUIAutomationTextRange> range;
    if (FAILED(pattern->get_DocumentRange(range.put())) || !range) {
        return;
    }
    BSTR content = nullptr;
    if (SUCCEEDED(range->GetText(-1, &content)) && content != nullptr) {
        out->text = std::wstring(content, ::SysStringLen(content));
        out->has_text = true;
        ::SysFreeString(content);
    }
}

std::size_t occurrences_of(const std::wstring& haystack, const std::wstring& needle)
{
    std::size_t count = 0;
    if (needle.empty()) {
        return 0;
    }
    for (std::size_t at = haystack.find(needle); at != std::wstring::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

void report_read_back(IUIAutomation* uia, const SpellCheck& before, const std::wstring& typed,
    double elapsed_ms)
{
    const SpellCheck after = wait_for_read_back(uia);
    char timing[96] = {};
    std::snprintf(timing, sizeof(timing), "elapsed=%.1fms %.3f ms/unit", elapsed_ms,
        typed.empty() ? 0.0 : elapsed_ms / static_cast<double>(typed.size()));
    log_line("read-back: before[" + before.describe() + "] after[" + after.describe() + "] expected="
        + std::to_string(typed.size()) + " " + timing);
    const auto tail = [](const std::wstring& text) {
        return trim_utf8(to_utf8(text.c_str(), static_cast<int>(text.size())), 160);
    };
    log_line("read-back value tail: \"" + tail(after.value) + "\" occurrences="
        + std::to_string(occurrences_of(after.value, typed)));
    log_line("read-back text  tail: \"" + tail(after.text) + "\" occurrences="
        + std::to_string(occurrences_of(after.text, typed)));
}

// ---------------------------------------------------------------------- main ---

struct Options {
    std::string mode = "help"; // help | watch | insert
    unsigned watch_seconds = 30;
    unsigned insert_after_ms = 5000;
    unsigned per_char_ms = 2;
    /// Characters per SendInput call (the backend uses a bounded batch).
    unsigned batch = 1;
    /// Real busy wait after each call, in microseconds. Sleep cannot express short pauses.
    unsigned spin_us = 0;
    /// Send with wVk = VK_PACKET instead of 0 (the TSF-friendly Unicode path).
    bool vk_packet = false;
    /// Post WM_CHAR straight to the focused control instead of injecting keyboard input.
    bool post_char = false;
    bool hotkey = false;
    std::string log_path;
    std::wstring text = L"\u041f\u0440\u043e\u0432\u0435\u0440\u043a\u0430 \u0432\u0441\u0442\u0430\u0432\u043a\u0438 "
                        L"VoiceTyper: \u0442\u0435\u0441\u0442 123 \u2014 Test 123.";
};

std::wstring to_wide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

void print_usage()
{
    log_line("focus-spike --watch [seconds] | --insert-after <ms> [--hotkey]");
    log_line("  --text \"<sample>\"    text to type (default: RU+EN sample)");
    log_line("  --per-char-ms <n>     Sleep() pause after each call (default 2, really ~15.6 ms)");
    log_line("  --spin-us <n>         real busy wait after each call, microseconds (default 0)");
    log_line("  --batch <n>           characters per SendInput call (default 1)");
    log_line("  --vk-packet           send Unicode events with wVk = VK_PACKET instead of 0");
    log_line("  --post-char           post WM_CHAR to the focused control (no keyboard path)");
    log_line("  --log <path>          log file (default: focus-spike.log next to the exe)");
    log_line("  --help");
}

bool parse_options(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                log_line(std::string("missing value for ") + what);
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--watch") {
            options->mode = "watch";
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options->watch_seconds = static_cast<unsigned>(std::atoi(next("--watch")));
            }
        } else if (arg == "--insert-after") {
            const char* value = next("--insert-after");
            if (value == nullptr) {
                return false;
            }
            options->mode = "insert";
            options->insert_after_ms = static_cast<unsigned>(std::atoi(value));
        } else if (arg == "--hotkey") {
            options->hotkey = true;
        } else if (arg == "--per-char-ms") {
            const char* value = next("--per-char-ms");
            if (value == nullptr) {
                return false;
            }
            options->per_char_ms = static_cast<unsigned>(std::atoi(value));
        } else if (arg == "--batch") {
            const char* value = next("--batch");
            if (value == nullptr) {
                return false;
            }
            options->batch = static_cast<unsigned>(std::atoi(value));
        } else if (arg == "--spin-us") {
            const char* value = next("--spin-us");
            if (value == nullptr) {
                return false;
            }
            options->spin_us = static_cast<unsigned>(std::atoi(value));
        } else if (arg == "--vk-packet") {
            options->vk_packet = true;
        } else if (arg == "--post-char") {
            options->post_char = true;
        } else if (arg == "--text") {
            const char* value = next("--text");
            if (value == nullptr) {
                return false;
            }
            options->text = to_wide(value);
        } else if (arg == "--log") {
            const char* value = next("--log");
            if (value == nullptr) {
                return false;
            }
            options->log_path = value;
        } else if (arg == "--help" || arg == "-h") {
            options->mode = "help";
        } else {
            log_line("unknown argument: " + arg);
            return false;
        }
    }
    return true;
}

void open_log(const std::string& requested)
{
    std::string path = requested;
    if (path.empty()) {
        wchar_t module[MAX_PATH] = {};
        const DWORD length = ::GetModuleFileNameW(nullptr, module, MAX_PATH);
        std::string directory = length > 0 ? to_utf8(module, static_cast<int>(length)) : std::string();
        const std::size_t slash = directory.find_last_of("\\/");
        directory = slash == std::string::npos ? std::string() : directory.substr(0, slash + 1);
        path = directory + "focus-spike.log";
    }
    g_log = std::fopen(path.c_str(), "ab");
    if (g_log == nullptr) {
        std::fprintf(stdout, "cannot open log %s\n", path.c_str());
    } else {
        log_line("log file: " + path);
    }
}

constexpr int kHotkeyId = 1;

int run(int argc, char** argv)
{
    ::SetConsoleOutputCP(CP_UTF8);

    Options options;
    if (!parse_options(argc, argv, &options)) {
        print_usage();
        return 2;
    }
    if (options.mode == "help") {
        print_usage();
        return 0;
    }
    open_log(options.log_path);

    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) {
        log_line("CoInitializeEx failed hr=" + hex32(static_cast<unsigned long>(com)));
        return 3;
    }

    ComPtr<IUIAutomation> uia;
    const HRESULT created = ::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_IUIAutomation,
                                               reinterpret_cast<void**>(uia.put()));
    if (FAILED(created) || !uia) {
        log_line("CoCreateInstance(CUIAutomation) failed hr=" + hex32(static_cast<unsigned long>(created)));
        ::CoUninitialize();
        return 3;
    }

    // A provider that does not answer (a hung application, a browser building its
    // accessibility tree) must not stall the probe for the default 60 s.
    ComPtr<IUIAutomation2> uia2;
    if (SUCCEEDED(uia->QueryInterface(IID_IUIAutomation2, reinterpret_cast<void**>(uia2.put()))) && uia2) {
        uia2->put_ConnectionTimeout(2000);
        uia2->put_TransactionTimeout(2000);
    }

    if (options.hotkey) {
        if (!::RegisterHotKey(nullptr, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F9)) {
            log_line("RegisterHotKey(Ctrl+Alt+Shift+F9) failed error="
                     + std::to_string(::GetLastError()));
        } else {
            log_line("hotkey Ctrl+Alt+Shift+F9 registered: probe + type");
        }
    }

    if (options.mode == "insert") {
        log_line("switch to the field under test; typing in " + std::to_string(options.insert_after_ms)
                 + " ms");
        ::Sleep(options.insert_after_ms);

        // First probe: on Chromium this is the query that makes the browser build
        // its accessibility tree. Second probe (150 ms later) sees the tree.
        probe_and_log(uia.get(), "insert/first");
        ::Sleep(150);
        const ProbeResult second = probe_and_log(uia.get(), "insert/second");

        const DWORD clipboard_before = ::GetClipboardSequenceNumber();
        const SpellCheck before = wait_for_read_back(uia.get());
        const InjectResult injected = options.post_char
                ? post_char_text(options.text, options.per_char_ms, options.spin_us)
                : type_text(options.text, options.per_char_ms, options.batch, options.spin_us,
                      options.vk_packet);
        ::Sleep(120);
        const DWORD clipboard_after = ::GetClipboardSequenceNumber();

        log_line("type: characters " + std::to_string(injected.typed) + "/"
                 + std::to_string(injected.expected)
                 + (injected.complete() ? " complete" : " INCOMPLETE error=" + std::to_string(injected.last_error)));
        report_read_back(uia.get(), before, options.text, injected.elapsed_ms);
        log_line(std::string("clipboard: ") + (clipboard_before == clipboard_after ? "unchanged" : "CHANGED")
                 + " (sequence " + std::to_string(clipboard_before) + " -> "
                 + std::to_string(clipboard_after) + ")");
        log_line("result: verdict=" + second.verdict + " typed=" + std::to_string(injected.typed)
                 + " expected=" + std::to_string(injected.expected));
        ::CoUninitialize();
        return injected.complete() ? 0 : 4;
    }

    // --watch: a WM_TIMER driven loop, optionally serving the hotkey. Without the
    // hotkey the loop stops after the requested number of seconds.
    const UINT_PTR timer = ::SetTimer(nullptr, 0, 500, nullptr);
    if (timer == 0) {
        log_line("SetTimer failed error=" + std::to_string(::GetLastError()));
    }
    const ULONGLONG started = ::GetTickCount64();
    log_line("watching for " + std::to_string(options.watch_seconds)
             + " s (click into the fields under test)");

    MSG message = {};
    bool running = true;
    while (running) {
        const BOOL has_message = ::GetMessageW(&message, nullptr, 0, 0);
        if (has_message == 0 || has_message == -1) {
            break;
        }
        if (message.message == WM_TIMER) {
            const ProbeResult result = probe(uia.get());
            log_line("watch verdict=" + result.verdict + " reason=" + result.reason);
            log_line("watch " + result.dump);
            const ULONGLONG elapsed = (::GetTickCount64() - started) / 1000;
            if (elapsed >= options.watch_seconds) {
                running = false;
            }
        } else if (message.message == WM_HOTKEY && message.wParam == kHotkeyId) {
            probe_and_log(uia.get(), "hotkey/first");
            ::Sleep(150);
            const ProbeResult second = probe_and_log(uia.get(), "hotkey/second");
            const DWORD clipboard_before = ::GetClipboardSequenceNumber();
            const SpellCheck before = wait_for_read_back(uia.get());
            const InjectResult injected = options.post_char
                ? post_char_text(options.text, options.per_char_ms, options.spin_us)
                : type_text(options.text, options.per_char_ms, options.batch, options.spin_us,
                      options.vk_packet);
            ::Sleep(120);
            const DWORD clipboard_after = ::GetClipboardSequenceNumber();
            log_line("hotkey type: characters " + std::to_string(injected.typed) + "/"
                     + std::to_string(injected.expected)
                     + (injected.complete() ? " complete"
                                            : " INCOMPLETE error=" + std::to_string(injected.last_error)));
            report_read_back(uia.get(), before, options.text, injected.elapsed_ms);
            log_line(std::string("hotkey clipboard: ")
                     + (clipboard_before == clipboard_after ? "unchanged" : "CHANGED")
                     + " verdict=" + second.verdict);
        }
    }

    if (timer != 0) {
        ::KillTimer(nullptr, timer);
    }
    if (options.hotkey) {
        ::UnregisterHotKey(nullptr, kHotkeyId);
    }
    ::CoUninitialize();
    log_line("stopped");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const int code = run(argc, argv);
    if (g_log != nullptr) {
        std::fclose(g_log);
        g_log = nullptr;
    }
    return code;
}
