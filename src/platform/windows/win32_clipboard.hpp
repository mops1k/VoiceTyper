#pragma once

// Win32 implementation of the frozen platform::Clipboard contract.
//
// Evidence and contract:
//   * src/platform/api/clipboard.hpp - the five allowed attempts with a 120 ms
//     gap, permission_denied when the clipboard stays locked,
//     unavailable when there is no clipboard owner, and the explicit note that
//     the .NET writer can return silently with no main window - a no-op gap this
//     backend must not copy, and for which a regression test is required.
//   * docs/migration/cpp/compatibility-contracts.md §3 - a nonempty transcript
//     always reaches the clipboard, a UAC paste failure never removes it, and
//     the Linux branch of the .NET project already reports "clipboard-only".
//   * src/domain/text_output.* - RetryingClipboard decorates this backend, and
//     TextOutputService treats a failed clipboard write as an error while a
//     failed paste is clipboard_only.
//
// Why a dedicated hidden window on its own thread, rather than the main window:
//   * The tray case is the product's normal hidden state ("Close hides the
//     window; process remains in tray", compatibility-contracts.md §2). A
//     clipboard owner must be a live top-level window, and a hidden-but-existing
//     Avalonia/Qt window is not something the platform layer may assume exists.
//   * OpenClipboard associates the clipboard with the calling thread's window, so
//     a worker thread (the recording worker) must not open the clipboard against
//     a window it does not own. Marshalling to one owner thread is what makes
//     set_text() callable from any thread, as clipboard.hpp requires.
//   * The window is a real hidden top-level window, not HWND_MESSAGE: a
//     message-only window is not a reliable clipboard owner.
//
// The contract gap this backend exists to close: the .NET Avalonia writer needs
// the UI thread and the main window, and can return successfully without writing
// anything when the window is missing. Here, a missing owner thread/window is
// ErrorCode::unavailable - never a silent success - and a clipboard that stays
// locked across all five attempts is ErrorCode::permission_denied. A lost
// transcript is therefore always visible to the caller, which is what lets
// TextOutputService report clipboard_failed instead of pretending it delivered.
//
// Retry interaction, stated so nobody has to rediscover it: the frozen contract
// gives *the Windows writer* five attempts at 120 ms, and domain::RetryingClipboard
// decorates whatever backend it is given with the same policy. Composed, the
// worst case is 5 x 5 attempts instead of 5. That is intentional: the inner loop
// is what makes this backend usable on its own, the outer loop is what the
// portable policy layer promises, and the total wait is still under two seconds.
// A caller that composes both and wants a single policy sets Win32ClipboardOptions::max_attempts
// to 1.
//
// Platform boundary: this header is standard-C++20 and includes no Windows
// header. The hidden window, the message loop and the OpenClipboard/SetClipboardData
// calls live in win32_clipboard.cpp.
//
// Thread affinity: all three interface methods are safe from any thread and
// none of them blocks the calling thread's UI loop: each marshals to the owner
// thread and waits with a bounded timeout.
//
// Ownership: the object owns its owner thread and its hidden window. Destroying
// it stops the thread and unregisters nothing that belongs to another process.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "platform/api/clipboard.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace voicetyper::platform {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// How long a clipboard operation waits for the owner thread to answer. A
/// clipboard call that has not answered within this is a broken owner thread,
/// not a slow clipboard, and it is reported instead of waited on forever.
inline constexpr std::chrono::milliseconds kClipboardRequestTimeout{3000};

/// How long the constructor waits for the hidden owner window to exist. Without
/// it, "constructed" and "usable" would be two different moments and every caller
/// would have to poll is_owner_ready() before its first write.
inline constexpr std::chrono::milliseconds kClipboardOwnerStartupTimeout{2000};

/// Retry and timeout policy of the Win32 clipboard backend.
struct Win32ClipboardOptions {
    /// OpenClipboard attempts. The frozen value is kClipboardMaxAttempts (5),
    /// because another process may hold the clipboard open. Set it to 1 when a
    /// portable RetryingClipboard decorator already applies the policy.
    int max_attempts = kClipboardMaxAttempts;
    /// Gap between attempts; the frozen value is kClipboardRetryDelayMs.
    std::chrono::milliseconds retry_delay{kClipboardRetryDelayMs};
    /// Upper bound on the wait for the owner thread.
    std::chrono::milliseconds request_timeout{kClipboardRequestTimeout};
};

class Win32Clipboard final : public Clipboard {
public:
    /// Starts the owner thread and creates the hidden clipboard window.
    /// Construction never throws: if the window cannot be created (a session
    /// with no window station, for example) is_owner_ready() returns false and
    /// every operation reports ErrorCode::unavailable.
    explicit Win32Clipboard(Win32ClipboardOptions options = {});
    ~Win32Clipboard() override;

    Win32Clipboard(const Win32Clipboard&) = delete;
    Win32Clipboard& operator=(const Win32Clipboard&) = delete;
    Win32Clipboard(Win32Clipboard&&) = delete;
    Win32Clipboard& operator=(Win32Clipboard&&) = delete;

    /// EmptyClipboard + SetClipboardData(CF_UNICODETEXT) as UTF-16.
    ///
    /// Failure: unavailable (no owner window, or the OS refused),
    /// permission_denied (the clipboard stayed locked across every attempt, or
    /// policy denied access), cancelled.
    Status set_text(std::string_view text, const CancellationToken& cancellation) override;

    /// The current clipboard text, or a success with nullopt when the clipboard
    /// holds no CF_UNICODETEXT data. One OpenClipboard attempt: a read is a
    /// probe, and retrying it would stall the caller's own retry policy.
    /// Failure: unavailable, permission_denied.
    [[nodiscard]] Result<std::optional<std::string>> get_text() const override;

    /// Cheap probe. False means "no text observed", which includes "the
    /// clipboard could not be opened": the interface returns a plain bool and has
    /// no error channel, so this can never be mistaken for a successful write.
    [[nodiscard]] bool has_text() const override;

    /// True when the hidden owner window exists and its message loop is
    /// running. A CTest run with no desktop session prints this as its skip
    /// reason instead of failing.
    [[nodiscard]] bool is_owner_ready() const noexcept;

    /// The Win32 error of the last failed OpenClipboard attempt, for the
    /// "clipboard busy after N tries" diagnostic. 0 when the last attempt worked.
    [[nodiscard]] unsigned long last_open_error() const noexcept;

    /// Attempts the last set_text() needed. Zero before the first call.
    [[nodiscard]] int last_attempts() const noexcept;

    /// The number of seconds a default retry policy can block for, i.e.
    /// (max_attempts - 1) * retry_delay. Callers that also run their own retry
    /// policy use it to avoid stacking two long waits.
    [[nodiscard]] std::chrono::milliseconds worst_case_retry_wait() const noexcept;

    /// Runs every queued clipboard job on the calling thread. The hidden window
    /// procedure calls this when it sees the private wake-up message, so it is
    /// public only so the window procedure can reach it; there is no reason for
    /// application code to call it.
    void drain_jobs();

private:
    /// One marshalled clipboard operation. Shared with the owner thread so a
    /// timed-out caller can abandon the request without the owner thread writing
    /// into freed memory.
    struct Call {
        std::mutex mutex;
        std::condition_variable done_signal;
        Status status = Status::success();
        std::optional<std::string> text;
        bool has_text = false;
        bool done = false;
    };

    void owner_main();
    void stop();
    /// Publishes "the owner window is up" or "it could not be created" to the
    /// waiting constructor.
    void publish_owner_state(bool ready);
    /// Runs `work` on the owner thread and waits for the result. `call` is shared
    /// with the owner thread, so abandoning it on timeout is safe: the owner
    /// keeps writing into memory the caller no longer owns. Returns false when
    /// the owner never accepted or never answered, which the callers turn into
    /// unavailable.
    bool run_on_owner(std::function<void()> work, const std::shared_ptr<Call>& call) const;
    void post_job(std::function<void()> work) const;
    /// OpenClipboard against the hidden window, retrying Win32ClipboardOptions::max_attempts
    /// times and honouring `cancellation` between attempts. Must be called on the
    /// owner thread. Returns ERROR_SUCCESS on success, otherwise the last Win32
    /// error.
    unsigned long open_clipboard_for_write(const CancellationToken& cancellation) const;

    Win32ClipboardOptions options_;

    mutable std::mutex mutex_;
    mutable std::deque<std::function<void()>> jobs_;
    /// Hand-off between the constructor and the owner thread: the window either
    /// exists or it definitively does not, by the time construction returns.
    std::mutex ready_mutex_;
    std::condition_variable ready_signal_;
    std::atomic_bool owner_failed_{false};
    std::atomic_bool stop_requested_{false};
    /// The hidden owner window, as an opaque handle. Null until the owner thread
    /// created it and again after it destroyed it.
    std::atomic<void*> owner_window_{nullptr};
    std::atomic_bool owner_ready_{false};
    // Mutable: the const probes (get_text/has_text) and the const retry helper
    // record the last observed error for the diagnostic.
    mutable std::atomic<unsigned long> last_open_error_{0};
    mutable std::atomic_int last_attempts_{0};
    std::thread owner_;
};

} // namespace voicetyper::platform
