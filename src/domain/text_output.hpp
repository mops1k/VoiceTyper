#pragma once

// Portable clipboard/paste policy, assembled over the Phase A platform seams
// (platform::Clipboard, PasteSimulator, Clock, Executor). No OS or Qt include:
// the Windows backends implement those ports, and everything observable about
// the policy lives here where it can be contract-tested anywhere.
//
// Frozen behaviour (docs/migration/cpp/compatibility-contracts.md §3):
//   * The clipboard is authoritative. It is written before any sleep and before
//     any paste, and a failed write means no paste is ever attempted.
//   * Auto-paste waits kPasteDelay (80 ms) and then injects Ctrl+V at most
//     kClipboardMaxAttempts (5) times with a kClipboardRetryDelayMs (120 ms)
//     gap, because another process can hold the clipboard open.
//   * Empty/whitespace-only text is a deliberate no-op: the clipboard is not even
//     touched.
//   * clipboard_only (paste refused, UAC, no injection backend) is a *success*:
//     the user still has the text. Only a failed clipboard write is an error, so
//     a lost transcript is always visible.
//   * Cancellation never rolls back a written clipboard, and a paste that already
//     succeeded is not retroactively cancelled.
//   * Injection is marshalled through an Executor with an on_this_thread() inline
//     fast path, because the recording worker is not the UI thread and a blocking
//     invoke from the UI thread would deadlock.
//   * The blank check is Unicode-aware (IsNullOrWhiteSpace parity): an NBSP or
//     U+3000 only transcript is not pasted.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "platform/api/clock.hpp"
#include "platform/api/clipboard.hpp"
#include "platform/api/executor.hpp"
#include "platform/api/paste.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace voicetyper::domain {

/// The port the recording state machine already calls. It lives here now so the
/// state machine and the service share one interface instead of two.
class TextOutputPort {
public:
    virtual ~TextOutputPort() = default;
    [[nodiscard]] virtual Result<bool> output(
        std::string_view text, bool auto_paste, const CancellationToken& cancellation) = 0;

protected:
    TextOutputPort() = default;
};

/// char.IsNullOrWhiteSpace parity. The ASCII-only version is wrong for
/// recognized text: a transcript that is only an NBSP must not be pasted.
[[nodiscard]] bool is_transcript_blank(std::string_view text) noexcept;

/// Adds the 5 x 120 ms clipboard retry policy as a decorator, so both platforms
/// get the same behaviour instead of it living in one UI adapter.
class RetryingClipboard final : public platform::Clipboard {
public:
    RetryingClipboard(platform::Clipboard& inner, platform::Clock& clock);

    platform::Status set_text(std::string_view text, const CancellationToken& cancellation) override;
    [[nodiscard]] Result<std::optional<std::string>> get_text() const override { return inner_.get_text(); }
    [[nodiscard]] bool has_text() const override { return inner_.has_text(); }
    /// Attempts the last set_text() needed, for the "clipboard busy after N tries"
    /// diagnostic. Zero before the first call.
    [[nodiscard]] int last_attempts() const noexcept;

private:
    platform::Clipboard& inner_;
    platform::Clock& clock_;
    std::atomic_int last_attempts_{0};
};

struct PasteDiagnostics {
    std::string_view reason = "paste_failed";
    bool injection_supported = false;
    /// True when the clipboard still holds the transcript, i.e. the user can paste
    /// it manually. Always true for clipboard_only.
    bool clipboard_holds_text = true;
};

struct OutputReport {
    platform::TextOutputOutcome outcome = platform::TextOutputOutcome::skipped_empty;
    std::optional<PasteDiagnostics> paste;
    bool delivered = false;
};

class TextOutputService final : public TextOutputPort {
public:
    TextOutputService(
        platform::Clipboard& clipboard,
        platform::PasteSimulator& paste,
        platform::Clock& clock,
        platform::Executor& ui_executor);

    [[nodiscard]] Result<bool> output(
        std::string_view text, bool auto_paste, const CancellationToken& cancellation) override;

    /// Suppresses injection while the settings window is capturing a hotkey; the
    /// text still reaches the clipboard.
    void set_paste_suspended(bool suspended) noexcept { suspended_.store(suspended); }
    [[nodiscard]] bool paste_suspended() const noexcept { return suspended_.load(); }

    void set_report_sink(std::function<void(const OutputReport&)> sink);

private:
    platform::Clipboard& clipboard_;
    platform::PasteSimulator& paste_;
    platform::Clock& clock_;
    platform::Executor& ui_executor_;
    std::atomic_bool suspended_{false};
    std::atomic_flag in_flight_ = ATOMIC_FLAG_INIT;
    std::mutex report_mutex_;
    std::function<void(const OutputReport&)> report_sink_;
};

} // namespace voicetyper::domain
