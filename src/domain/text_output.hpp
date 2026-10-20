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
//   * Direct delivery (VT-DOM-613..617): when `directInsert` is on, the focused
//     field is probed first and an `editable` verdict posts the transcript into
//     it — then the clipboard is not touched at all and the outcome is `direct`.
//     Every other verdict, and any refusal by the injector, falls back to exactly
//     the clipboard-then-paste behaviour above, so the clipboard remains
//     authoritative for every path except a successful direct delivery.
//   * A direct attempt that posted characters and did not confirm them MUST NOT
//     be followed by the paste chord (VT-DOM-619): the field may have received
//     the text and only exposed it late, and a fallback paste then inserted the
//     transcript twice (measured 2026-10-20). Such a delivery writes the
//     clipboard, ends as `clipboard_only` with reason `direct_unconfirmed` and
//     sends no `Ctrl+V`.
//   * The paste is confirmed, not assumed (VT-DOM-618): a synthetic `Ctrl+V` is
//     accepted by the input stream whether or not the target windows reads it -
//     measured 2026-10-19, a Windows Terminal window accepted the chord and showed
//     nothing while the old code reported `pasted`. When the platform can read the
//     focused field back, the text has to be seen there; otherwise the outcome is
//     `clipboard_only` with reason `paste_unconfirmed` and the user is told the
//     transcript is on the clipboard. A platform without a reader keeps the old
//     behaviour: an unverifiable delivery is not turned into a verdict.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "platform/api/clock.hpp"
#include "platform/api/clipboard.hpp"
#include "platform/api/executor.hpp"
#include "platform/api/paste.hpp"
#include "platform/api/text_target.hpp"

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

/// VT-DOM-620: true when a transcript delivered at this caret must start with one
/// space, so the dictation continues the sentence instead of gluing itself to the
/// punctuation in front of the caret. `caret_tail_utf8` is the text immediately
/// before the caret (UTF-8, the last code point is what counts); `transcript` is the
/// recognized text as the engine returned it. Pure on purpose: the rule is pinned by
/// the contract test without a live desktop.
///
/// A non-empty selection is NOT this function's business: its caller must ask
/// `FocusedTextField::selection_present()` first, because a delivery replaces the
/// selection and has no punctuation to continue.
[[nodiscard]] bool needs_leading_space(
    std::string_view caret_tail_utf8, std::string_view transcript) noexcept;

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
    /// What the direct-delivery attempt did, so the log can tell "the field was
    /// not recognised" from "the field refused the typing" (VT-DOM-616). One of
    /// `not_attempted`, `refused`, `partial`, `unconfirmed`,
    /// `skipped_no_target`, `skipped_read_only`, `skipped_unknown`,
    /// `probe_timeout`. A successful direct delivery reports no diagnostics at
    /// all (VT-DOM-614), which is why `inserted` is not a value here.
    std::string_view direct = "not_attempted";
};

struct OutputReport {
    platform::TextOutputOutcome outcome = platform::TextOutputOutcome::skipped_empty;
    std::optional<PasteDiagnostics> paste;
    bool delivered = false;
    /// Wall time of the whole delivery, from the clipboard write to the paste
    /// attempt, in milliseconds. Diagnostics for the log: it is what separates
    /// "recognition was slow" from "the paste into the focused window was slow".
    long long duration_ms = 0;

    /// Where the delivery spent its time, in milliseconds. `duration_ms` alone could
    /// not answer the 2026-10-19 question "why is a direct delivery 449 ms while the
    /// clipboard path is 87 ms": the probe, the typing/paste and the field check are
    /// three different costs with three different fixes. Zero means "not run".
    long long probe_ms = 0;
    long long insert_ms = 0;
    long long paste_ms = 0;
    long long confirm_ms = 0;

    /// VT-DOM-620: true when one leading space was added to the delivered text because
    /// the caret followed a sentence terminator with no space behind it. Diagnostics
    /// only: the report never carries the text, and the transcript itself is unchanged.
    bool leading_space = false;
};

class TextOutputService final : public TextOutputPort {
public:
    TextOutputService(
        platform::Clipboard& clipboard,
        platform::PasteSimulator& paste,
        platform::TextTargetProbe& probe,
        platform::DirectTextInjector& direct,
        platform::FocusedTextField& field,
        platform::Clock& clock,
        platform::Executor& ui_executor);

    [[nodiscard]] Result<bool> output(
        std::string_view text, bool auto_paste, const CancellationToken& cancellation) override;

    /// Suppresses injection while the settings window is capturing a hotkey; the
    /// text still reaches the clipboard.
    void set_paste_suspended(bool suspended) noexcept { suspended_.store(suspended); }
    [[nodiscard]] bool paste_suspended() const noexcept { return suspended_.load(); }

    /// The `directInsert` setting (VT-DOM-613): when false the probe is not
    /// consulted at all and delivery is the clipboard-then-paste path, exactly as
    /// it was before direct delivery existed.
    void set_direct_insert(bool enabled) noexcept { direct_insert_.store(enabled); }
    [[nodiscard]] bool direct_insert() const noexcept { return direct_insert_.load(); }

    void set_report_sink(std::function<void(const OutputReport&)> sink);

private:
    /// What the post-paste check could establish (VT-DOM-618). Three states, because
    /// "I could not read the field" is not "the paste failed": a plugin editor that a
    /// UI Automation reader cannot see would otherwise raise an alarm on every working
    /// paste (measured 2026-10-19 in a JetBrains plugin).
    enum class PasteConfirmation : std::uint8_t {
        /// The transcript was seen in the field.
        confirmed,
        /// The field was readable and stayed empty: the chord demonstrably did nothing.
        failed,
        /// The field could not be read, or it holds other text: nothing can be proven.
        unverifiable,
    };

    /// VT-DOM-618: looks at the field after the paste chord and answers "is the
    /// transcript there?".
    [[nodiscard]] PasteConfirmation confirm_paste(
        std::string_view text, const CancellationToken& cancellation);

    platform::Clipboard& clipboard_;
    platform::PasteSimulator& paste_;
    platform::TextTargetProbe& probe_;
    platform::DirectTextInjector& direct_;
    platform::FocusedTextField& field_;
    platform::Clock& clock_;
    platform::Executor& ui_executor_;
    std::atomic_bool suspended_{false};
    std::atomic_bool direct_insert_{true};
    std::atomic_flag in_flight_ = ATOMIC_FLAG_INIT;
    std::mutex report_mutex_;
    std::function<void(const OutputReport&)> report_sink_;
};

/// How long the typing of one transcript may occupy the input-owning thread
/// before the delivery gives up on it. A long transcript is typed at roughly
/// 2 ms per character, so the budget has to clear a few thousand characters.
inline constexpr std::chrono::milliseconds kDirectInsertExecutorTimeout{10000};

/// Extra time on top of the probe's own two bounded queries before the delivery
/// stops waiting for a verdict and takes the clipboard path.
inline constexpr std::chrono::milliseconds kDirectProbeSlack{500};

/// How much of the field the post-paste check reads. Smaller than the typer's own
/// check on purpose: reading a legacy edit costs about 10 ms per character
/// (measured 2026-10-16), and one paste only has to show its tail.
inline constexpr std::size_t kPasteConfirmTailUnits{16};

/// How long the post-paste check waits for the target to process the chord. The
/// target reads its queue on its own thread, so the text appears a moment after
/// the chord, never instantly.
inline constexpr int kPasteConfirmAttempts{6};
inline constexpr std::chrono::milliseconds kPasteConfirmInterval{30};

} // namespace voicetyper::domain
