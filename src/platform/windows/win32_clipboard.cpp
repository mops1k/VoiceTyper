#include "platform/windows/win32_clipboard.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace voicetyper::platform {

#if defined(_WIN32)

namespace {

/// A real hidden top-level window, not HWND_MESSAGE: a message-only window is
/// not a dependable clipboard owner, and the tray case needs an owner that exists
/// with no visible UI at all.
constexpr wchar_t kOwnerClassName[] = L"VoiceTyperWin32ClipboardOwner";
/// Private wake-up message: the owner thread drains its job queue when it sees it.
constexpr UINT kOwnerWakeMessage = WM_APP + 1;

/// CF_UNICODETEXT ceiling. Windows itself has no fixed limit, but a transcript is
/// a few kilobytes and the plan's largest fixture is 1 MB; refusing far above
/// that is better than a silent out-of-memory in GlobalAlloc.
constexpr std::size_t kMaxClipboardCharacters = 64u * 1024u * 1024u;

/// UTF-8 -> UTF-16.
///
/// Two Windows facts are load-bearing here, and getting either wrong makes
/// set_text() report success while putting an *empty* string on the clipboard -
/// which is precisely the silent-success gap this backend exists to close:
///
///   1. A size query (lpWideCharStr == nullptr) combined with WC_ERR_INVALID_CHARS
///      fails with ERROR_INVALID_FLAGS. The size is therefore queried with flags
///      0, which is always accepted.
///   2. WC_ERR_INVALID_CHARS is what makes malformed UTF-8 an error instead of a
///      silent U+FFFD substitution. It is applied on the fill call, and if the
///      running Windows build rejects the flag there too, the conversion falls
///      back to flags 0 rather than refusing to write at all - a transcript with
///      one odd byte must still reach the user's document.
std::u16string utf8_to_utf16(std::string_view text)
{
    std::u16string result;
    if (text.empty()) {
        return result;
    }
    const int size = static_cast<int>(std::min<std::size_t>(text.size(), 0x7FFFFFFF));
    const int required = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
    if (required <= 0) {
        return result;
    }
    result.resize(static_cast<std::size_t>(required));

    int written = ::MultiByteToWideChar(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        size,
        reinterpret_cast<wchar_t*>(result.data()),
        required);
    if (written == 0 && ::GetLastError() == ERROR_INVALID_FLAGS) {
        // This Windows build does not accept the strict flag; convert leniently
        // instead of dropping the transcript.
        written = ::MultiByteToWideChar(
            CP_UTF8,
            0,
            text.data(),
            size,
            reinterpret_cast<wchar_t*>(result.data()),
            required);
    }
    if (written <= 0) {
        result.clear();
    }
    return result;
}

/// UTF-16 -> UTF-8. A NUL inside the clipboard data is treated as the string
/// terminator, which is what every Windows text reader does.
std::string utf16_to_utf8(const wchar_t* text)
{
    if (text == nullptr) {
        return std::string();
    }
    const std::size_t length = ::wcslen(text);
    if (length == 0) {
        return std::string();
    }
    const int required = ::WideCharToMultiByte(
        CP_UTF8, 0, text, static_cast<int>(std::min<std::size_t>(length, 0x7FFFFFFF)), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::string();
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        text,
        static_cast<int>(std::min<std::size_t>(length, 0x7FFFFFFF)),
        result.data(),
        required,
        nullptr,
        nullptr);
    if (written <= 0) {
        return std::string();
    }
    result.resize(static_cast<std::size_t>(written));
    return result;
}

/// A clipboard error mapped the way clipboard.hpp freezes it:
///   * ERROR_ACCESS_DENIED after every attempt -> permission_denied (OS policy or
///     another process that owns the clipboard for good);
///   * anything else that survived the retries -> permission_denied as well,
///     because "the clipboard stayed locked" is exactly the condition the
///     contract names and it must never be reported as success;
///   * a refusal from the owner itself (no window) -> unavailable.
Status clipboard_error(unsigned long code, int attempts)
{
    return Status::failure(
        ErrorCode::permission_denied,
        "clipboard stayed locked after " + std::to_string(attempts) + " attempt(s) (win32 "
            + std::to_string(code) + ")");
}

} // namespace

static LRESULT CALLBACK clipboard_owner_wnd_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    Win32Clipboard* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<Win32Clipboard*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Win32Clipboard*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (message) {
    case kOwnerWakeMessage:
        if (self != nullptr) {
            self->drain_jobs();
        }
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, w_param, l_param);
}

Win32Clipboard::Win32Clipboard(Win32ClipboardOptions options)
    : options_(options)
{
    if (options_.max_attempts < 1) {
        options_.max_attempts = 1;
    }
    if (options_.retry_delay < std::chrono::milliseconds::zero()) {
        options_.retry_delay = std::chrono::milliseconds::zero();
    }
    try {
        owner_ = std::thread([this] { owner_main(); });
    } catch (...) {
        // No owner thread means no clipboard. Every operation then reports
        // unavailable, which is the honest answer: nothing was written.
        owner_ready_.store(false);
        owner_failed_.store(true);
        return;
    }

    // Wait for the hand-off. Constructing an object that is not yet usable is how
    // a first transcript is lost to a startup race, so the constructor blocks
    // (briefly, and always with a bound) until the hidden window exists or the
    // owner thread has proved it cannot be created.
    std::unique_lock<std::mutex> lock(ready_mutex_);
    static_cast<void>(ready_signal_.wait_for(lock, kClipboardOwnerStartupTimeout, [this] {
        return owner_ready_.load(std::memory_order_acquire)
            || owner_failed_.load(std::memory_order_acquire);
    }));
}

Win32Clipboard::~Win32Clipboard()
{
    stop();
}

bool Win32Clipboard::is_owner_ready() const noexcept
{
    return owner_ready_.load(std::memory_order_acquire) && owner_window_.load(std::memory_order_acquire) != nullptr;
}

unsigned long Win32Clipboard::last_open_error() const noexcept
{
    return last_open_error_.load(std::memory_order_acquire);
}

int Win32Clipboard::last_attempts() const noexcept
{
    return last_attempts_.load(std::memory_order_acquire);
}

std::chrono::milliseconds Win32Clipboard::worst_case_retry_wait() const noexcept
{
    if (options_.max_attempts <= 1) {
        return std::chrono::milliseconds::zero();
    }
    return options_.retry_delay * (options_.max_attempts - 1);
}

void Win32Clipboard::publish_owner_state(bool ready)
{
    {
        std::lock_guard<std::mutex> lock(ready_mutex_);
        if (ready) {
            owner_ready_.store(true, std::memory_order_release);
        } else {
            owner_failed_.store(true, std::memory_order_release);
        }
    }
    ready_signal_.notify_all();
}

void Win32Clipboard::owner_main()
{
    const HINSTANCE module = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &clipboard_owner_wnd_proc;
    window_class.hInstance = module;
    window_class.lpszClassName = kOwnerClassName;
    if (::RegisterClassExW(&window_class) == 0) {
        const DWORD code = ::GetLastError();
        // A second instance in the same process legitimately finds the class
        // already registered; the window procedure is the same function, so the
        // class is reusable.
        if (code != ERROR_CLASS_ALREADY_EXISTS) {
            publish_owner_state(false);
            return;
        }
    }

    // WS_OVERLAPPED with no WS_VISIBLE: a real top-level window that is never
    // shown, so the clipboard has an owner while the application sits in the tray.
    HWND window = ::CreateWindowExW(
        0,
        kOwnerClassName,
        L"VoiceTyper clipboard",
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        module,
        this);
    if (window == nullptr) {
        // No window station (a service or session-0 context) or out of memory.
        // is_owner_ready() stays false and every call reports unavailable.
        publish_owner_state(false);
        return;
    }
    owner_window_.store(window, std::memory_order_release);
    publish_owner_state(true);

    MSG message{};
    while (!stop_requested_.load(std::memory_order_acquire)) {
        // Jobs posted before the window existed, and the belt-and-braces path
        // when a PostMessage was lost.
        drain_jobs();
        if (stop_requested_.load(std::memory_order_acquire)) {
            break;
        }
        const DWORD wait = ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        if (wait != WAIT_OBJECT_0) {
            continue;
        }
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            if (message.message == WM_QUIT) {
                stop_requested_.store(true, std::memory_order_release);
                break;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    // Finish whatever is already queued so no caller is left waiting on a
    // request the owner silently dropped.
    drain_jobs();
    owner_ready_.store(false, std::memory_order_release);
    ::DestroyWindow(window);
    owner_window_.store(nullptr, std::memory_order_release);
}

void Win32Clipboard::drain_jobs()
{
    while (true) {
        std::function<void()> work;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (jobs_.empty()) {
                return;
            }
            work = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try {
            work();
        } catch (...) {
            // A job that throws must never unwind through the Win32 window
            // procedure, which would take the whole owner thread down. Each job
            // converts its own failures into a Status; this is only the last
            // resort for an unexpected allocation failure.
        }
    }
}

void Win32Clipboard::post_job(std::function<void()> work) const
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_.load(std::memory_order_acquire)) {
            return;
        }
        jobs_.push_back(std::move(work));
    }
    const void* window = owner_window_.load(std::memory_order_acquire);
    if (window != nullptr) {
        ::PostMessageW(static_cast<HWND>(const_cast<void*>(window)), kOwnerWakeMessage, 0, 0);
    }
}

bool Win32Clipboard::run_on_owner(std::function<void()> work, const std::shared_ptr<Call>& call) const
{
    if (!is_owner_ready()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_.load(std::memory_order_acquire)) {
            return false;
        }
    }

    post_job(std::move(work));

    std::unique_lock<std::mutex> lock(call->mutex);
    if (call->done_signal.wait_for(lock, options_.request_timeout, [&call] { return call->done; })) {
        return true;
    }
    // The owner thread has not answered in time. Abandoning the call is safe
    // because the state is shared: the owner may still write into it, and nobody
    // is left reading.
    call->done = true;
    return false;
}

unsigned long Win32Clipboard::open_clipboard_for_write(const CancellationToken& cancellation) const
{
    HWND window = static_cast<HWND>(owner_window_.load(std::memory_order_acquire));
    int attempt = 0;
    unsigned long last = ERROR_SUCCESS;
    while (attempt < options_.max_attempts) {
        ++attempt;
        if (cancellation.is_cancellation_requested()) {
            last_attempts_.store(attempt, std::memory_order_release);
            last_open_error_.store(static_cast<unsigned long>(ERROR_CANCELLED), std::memory_order_release);
            return static_cast<unsigned long>(ERROR_CANCELLED);
        }
        if (::OpenClipboard(window)) {
            last_attempts_.store(attempt, std::memory_order_release);
            last_open_error_.store(0, std::memory_order_release);
            return 0;
        }
        last = ::GetLastError();
        last_open_error_.store(last, std::memory_order_release);
        if (attempt < options_.max_attempts && options_.retry_delay.count() > 0) {
            // A bounded, cancellation-aware pause between attempts. The owner
            // thread sleeps, never the caller's thread, so the UI stays free.
            const auto deadline = std::chrono::steady_clock::now() + options_.retry_delay;
            while (std::chrono::steady_clock::now() < deadline) {
                if (cancellation.is_cancellation_requested()) {
                    last_attempts_.store(attempt, std::memory_order_release);
                    return static_cast<unsigned long>(ERROR_CANCELLED);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    last_attempts_.store(attempt, std::memory_order_release);
    return last;
}

Status Win32Clipboard::set_text(std::string_view text, const CancellationToken& cancellation)
{
    if (cancellation.is_cancellation_requested()) {
        return domain::check_cancelled(cancellation);
    }
    if (text.size() > kMaxClipboardCharacters) {
        return Status::failure(
            ErrorCode::resource_exhausted,
            "clipboard payload of " + std::to_string(text.size()) + " bytes exceeds the supported limit");
    }
    if (!is_owner_ready()) {
        // This is the .NET no-op gap the contract forbids copying: no owner window
        // is reported, never swallowed.
        return Status::failure(
            ErrorCode::unavailable,
            "no clipboard owner window: the write did not happen");
    }

    // The text is converted on the calling thread and the UTF-16 is moved into
    // the job, so nothing outlives this call and no other thread can see a
    // half-written clipboard payload.
    const std::u16string wide = utf8_to_utf16(text);
    auto call = std::make_shared<Call>();

    const bool answered = run_on_owner(
        [this, wide, call, cancellation] {
            Status result = Status::success();
            const unsigned long opened = open_clipboard_for_write(cancellation);
            if (opened == static_cast<unsigned long>(ERROR_CANCELLED)) {
                result = domain::check_cancelled(cancellation);
            } else if (opened != 0) {
                result = clipboard_error(opened, last_attempts());
            } else {
                if (!::EmptyClipboard()) {
                    result = Status::failure(
                        ErrorCode::unavailable,
                        "EmptyClipboard failed (win32 " + std::to_string(::GetLastError()) + ")");
                } else {
                    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
                    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (memory == nullptr) {
                        result = Status::failure(
                            ErrorCode::resource_exhausted, "GlobalAlloc failed for the clipboard payload");
                    } else {
                        void* target = ::GlobalLock(memory);
                        if (target == nullptr) {
                            ::GlobalFree(memory);
                            result = Status::failure(ErrorCode::io_failure, "GlobalLock failed");
                        } else {
                            std::memcpy(target, wide.c_str(), bytes);
                            ::GlobalUnlock(memory);
                            // On success the system owns the block; freeing it here
                            // would be a double free.
                            if (::SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
                                ::GlobalFree(memory);
                                result = Status::failure(
                                    ErrorCode::unavailable,
                                    "SetClipboardData failed (win32 " + std::to_string(::GetLastError()) + ")");
                            }
                        }
                    }
                }
                ::CloseClipboard();
            }

            std::lock_guard<std::mutex> lock(call->mutex);
            call->status = result;
            call->done = true;
            call->done_signal.notify_all();
        },
        call);

    if (!answered) {
        return Status::failure(
            ErrorCode::unavailable,
            "the clipboard owner thread did not answer: the write did not happen");
    }
    return call->status;
}

Result<std::optional<std::string>> Win32Clipboard::get_text() const
{
    if (!is_owner_ready()) {
        return Result<std::optional<std::string>>::failure(
            ErrorCode::unavailable, "no clipboard owner window");
    }
    auto call = std::make_shared<Call>();
    const bool answered = run_on_owner(
        [this, call] {
            Status result = Status::success();
            std::optional<std::string> text;
            // A read does not get the writer's 5 x 120 ms policy - that would stack
            // a second retry layer on top of the caller's own - but it does get a
            // short bounded retry. Windows reports a clipboard that is currently
            // open elsewhere as ERROR_ACCESS_DENIED, and that is measured to happen
            // for a few milliseconds right after this process's own CloseClipboard
            // (roughly one run in three here). Reporting a transient lock as a hard
            // read failure would make a perfectly readable clipboard look broken, so
            // the probe retries a few times inside a bounded window instead.
            constexpr int kReadAttempts = 3;
            Status last_error = Status::success();
            bool opened = false;
            for (int attempt = 0; attempt < kReadAttempts; ++attempt) {
                if (::OpenClipboard(static_cast<HWND>(owner_window_.load(std::memory_order_acquire)))) {
                    opened = true;
                    const HGLOBAL data = ::GetClipboardData(CF_UNICODETEXT);
                    if (data != nullptr) {
                        const auto* locked = static_cast<const wchar_t*>(::GlobalLock(data));
                        if (locked != nullptr) {
                            std::string value = utf16_to_utf8(locked);
                            ::GlobalUnlock(data);
                            // A clipboard holding an empty string is "no text",
                            // which is the same thing to every caller here.
                            if (!value.empty()) {
                                text = std::move(value);
                            }
                        } else {
                            result = Status::failure(
                                ErrorCode::io_failure,
                                "GlobalLock failed (win32 " + std::to_string(::GetLastError()) + ")");
                        }
                    }
                    // No CF_UNICODETEXT data is a success with nullopt, not an
                    // error: the clipboard simply holds something else.
                    ::CloseClipboard();
                    break;
                }
                const DWORD code = ::GetLastError();
                last_open_error_.store(static_cast<unsigned long>(code), std::memory_order_release);
                // Remembered rather than assigned: a later successful attempt must
                // not leave the earlier transient failure as the reported status.
                last_error = clipboard_error(static_cast<unsigned long>(code), attempt + 1);
                if (attempt + 1 < kReadAttempts) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
            // Only a run that never opened the clipboard is an error. Reporting
            // the transient failure of an attempt that a later one recovered from
            // would turn a successful read into a hard failure.
            if (!opened && result.is_ok()) {
                result = last_error;
            }

            std::lock_guard<std::mutex> lock(call->mutex);
            call->status = result;
            call->text = std::move(text);
            call->has_text = call->text.has_value();
            call->done = true;
            call->done_signal.notify_all();
        },
        call);

    if (!answered) {
        return Result<std::optional<std::string>>::failure(
            ErrorCode::unavailable, "the clipboard owner thread did not answer");
    }
    if (call->status.is_error()) {
        return call->status.error();
    }
    return call->text;
}

bool Win32Clipboard::has_text() const
{
    if (!is_owner_ready()) {
        return false;
    }
    auto call = std::make_shared<Call>();
    const bool answered = run_on_owner(
        [this, call] {
            // A single OpenClipboard can fail transiently while the previous owner
            // is still finishing its CloseClipboard, and the interface returns a
            // plain bool with no error channel. Reporting false for that would
            // make the settings UI's "clipboard has text" indicator flicker, so
            // the probe retries a few times inside a bounded window instead. The
            // last Win32 error is kept for the diagnostic either way.
            constexpr int kProbeAttempts = 3;
            bool present = false;
            for (int attempt = 0; attempt < kProbeAttempts; ++attempt) {
                if (::OpenClipboard(static_cast<HWND>(owner_window_.load(std::memory_order_acquire)))) {
                    const HGLOBAL data = ::GetClipboardData(CF_UNICODETEXT);
                    if (data != nullptr) {
                        const auto* locked = static_cast<const wchar_t*>(::GlobalLock(data));
                        if (locked != nullptr) {
                            present = locked[0] != L'\0';
                            ::GlobalUnlock(data);
                        }
                    }
                    ::CloseClipboard();
                    break;
                }
                const DWORD code = ::GetLastError();
                last_open_error_.store(static_cast<unsigned long>(code), std::memory_order_release);
                if (attempt + 1 < kProbeAttempts) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
            std::lock_guard<std::mutex> lock(call->mutex);
            call->has_text = present;
            call->done = true;
            call->done_signal.notify_all();
        },
        call);
    if (!answered) {
        return false;
    }
    return call->has_text;
}

void Win32Clipboard::stop()
{
    if (stop_requested_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const void* window = owner_window_.load(std::memory_order_acquire);
    if (window != nullptr) {
        // WM_QUIT is thread-specific, so this only wakes the owner thread.
        ::PostMessageW(static_cast<HWND>(const_cast<void*>(window)), WM_QUIT, 0, 0);
    }
    if (owner_.joinable()) {
        owner_.join();
    }
}

#else // !_WIN32

// Unreachable in this build (the library target is WIN32-only). Present so an
// accidental non-Windows compile refuses explicitly instead of failing to link:
// the same "never a silent success" rule the Windows path obeys.
namespace {

Status no_platform()
{
    return Status::failure(
        ErrorCode::unavailable, "the Win32 clipboard backend is not available on this platform");
}

Result<std::optional<std::string>> no_platform_value()
{
    return Result<std::optional<std::string>>::failure(
        ErrorCode::unavailable, "the Win32 clipboard backend is not available on this platform");
}

} // namespace

Win32Clipboard::Win32Clipboard(Win32ClipboardOptions options)
    : options_(options)
{
}

Win32Clipboard::~Win32Clipboard() = default;
Status Win32Clipboard::set_text(std::string_view, const CancellationToken&) { return no_platform(); }
Result<std::optional<std::string>> Win32Clipboard::get_text() const { return no_platform_value(); }
bool Win32Clipboard::has_text() const { return false; }
bool Win32Clipboard::is_owner_ready() const noexcept { return false; }
unsigned long Win32Clipboard::last_open_error() const noexcept { return 0; }
int Win32Clipboard::last_attempts() const noexcept { return 0; }
std::chrono::milliseconds Win32Clipboard::worst_case_retry_wait() const noexcept { return {}; }
void Win32Clipboard::owner_main() {}
void Win32Clipboard::drain_jobs() {}
void Win32Clipboard::stop() {}
bool Win32Clipboard::run_on_owner(std::function<void()>, const std::shared_ptr<Call>&) const { return false; }
void Win32Clipboard::post_job(std::function<void()>) const {}
unsigned long Win32Clipboard::open_clipboard_for_write(const CancellationToken&) const { return 0; }

#endif // _WIN32

} // namespace voicetyper::platform
