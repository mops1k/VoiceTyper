#include "domain/text_output.hpp"

#include <future>
#include <memory>
#include <thread>
#include <utility>

namespace voicetyper::domain {
namespace {

/// Decodes one UTF-8 code point, advancing `index`. A malformed byte is returned
/// as-is so a broken transcript cannot loop forever.
std::uint32_t next_code_point(std::string_view text, std::size_t& index)
{
    const auto first = static_cast<unsigned char>(text[index]);
    if (first < 0x80) {
        ++index;
        return first;
    }
    std::size_t extra = 0;
    std::uint32_t value = 0;
    if ((first & 0xE0U) == 0xC0U) {
        extra = 1;
        value = first & 0x1FU;
    } else if ((first & 0xF0U) == 0xE0U) {
        extra = 2;
        value = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
        extra = 3;
        value = first & 0x07U;
    } else {
        ++index;
        return first;
    }
    if (index + extra >= text.size()) {
        ++index;
        return first;
    }
    for (std::size_t i = 1; i <= extra; ++i) {
        const auto byte = static_cast<unsigned char>(text[index + i]);
        if ((byte & 0xC0U) != 0x80U) {
            ++index;
            return first;
        }
        value = (value << 6U) | (byte & 0x3FU);
    }
    index += extra + 1;
    return value;
}

bool is_unicode_space(std::uint32_t code_point) noexcept
{
    switch (code_point) {
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
    case 0x20:
    case 0x85:
    case 0xA0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
        return true;
    default:
        return code_point >= 0x2000 && code_point <= 0x200A;
    }
}

constexpr std::string_view kReasonIntegrity = "integrity_level";
constexpr std::string_view kReasonNoBackend = "no_injection_backend";
constexpr std::string_view kReasonWindowRejected = "window_rejected_input";
constexpr std::string_view kReasonSuspended = "suspended_for_hotkey_capture";
constexpr std::string_view kReasonExecutor = "executor_unavailable";
constexpr std::string_view kReasonClipboard = "clipboard_busy";
/// VT-DOM-615: part of the text was typed, so the clipboard gets the rest of the
/// story but the paste chord is deliberately not sent.
constexpr std::string_view kReasonDirectPartial = "direct_partial";
/// VT-DOM-618: the paste chord was accepted but the transcript never showed up in
/// the field the caller could read.
constexpr std::string_view kReasonPasteUnconfirmed = "paste_unconfirmed";

/// The last `max_code_points` characters of `text`, cut on a code-point boundary so
/// a multi-byte character is never split. The paste check compares this tail: it is
/// the part of the transcript a bounded read of the field can see.
std::string utf8_suffix(std::string_view text, std::size_t max_code_points)
{
    std::size_t index = text.size();
    std::size_t count = 0;
    while (index > 0 && count < max_code_points) {
        --index;
        while (index > 0 && (static_cast<unsigned char>(text[index]) & 0xC0U) == 0x80U) {
            --index;
        }
        ++count;
    }
    return std::string(text.substr(index));
}

/// Whitespace-insensitive form, so a console that wraps the pasted text with CRLF or
/// pads the line with spaces still matches: the check answers "is the transcript
/// there", not "is it laid out exactly as we sent it".
std::string without_whitespace(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char byte : text) {
        if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') {
            out.push_back(byte);
        }
    }
    return out;
}

bool ends_with(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size()
        && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

bool is_transcript_blank(std::string_view text) noexcept
{
    std::size_t index = 0;
    while (index < text.size()) {
        if (!is_unicode_space(next_code_point(text, index))) {
            return false;
        }
    }
    return true;
}

bool is_annotation_only(std::string_view text) noexcept
{
    // whisper.cpp describes non-speech input with annotations: `[звук сетки]`,
    // `*звук*`, `(music)`, `[BLANK_AUDIO]`. A transcript that is nothing but
    // annotations is not something the user said, so typing it into their
    // document is a defect, not a result - measured on the target machine, a
    // microphone that only heard the room produced `[звук сетки] [звук сетки]`.
    // Anything outside brackets keeps the transcript deliverable: a real sentence
    // that merely contains an annotation is real text.
    std::size_t index = 0;
    bool saw_annotation = false;
    while (index < text.size()) {
        const std::size_t probe = index;
        if (is_unicode_space(next_code_point(text, index))) {
            continue;
        }
        index = probe;
        const char opener = text[index];
        const char closer = opener == '[' ? ']' : (opener == '(' ? ')' : (opener == '*' ? '*' : '\0'));
        if (closer == '\0') {
            return false;
        }
        const std::size_t end = text.find(closer, index + 1);
        if (end == std::string_view::npos || end == index + 1) {
            return false;
        }
        saw_annotation = true;
        index = end + 1;
    }
    return saw_annotation;
}

// ---------------------------------------------------------------------------
// RetryingClipboard
// ---------------------------------------------------------------------------

RetryingClipboard::RetryingClipboard(platform::Clipboard& inner, platform::Clock& clock)
    : inner_(inner), clock_(clock)
{
}

int RetryingClipboard::last_attempts() const noexcept
{
    return last_attempts_.load();
}

platform::Status RetryingClipboard::set_text(std::string_view text, const CancellationToken& cancellation)
{
    platform::Status last = platform::Status::failure(
        ErrorCode::unavailable, "the clipboard was never written");
    for (int attempt = 0; attempt < platform::kClipboardMaxAttempts; ++attempt) {
        last_attempts_.store(attempt + 1);
        last = inner_.set_text(text, cancellation);
        if (last.is_ok()) {
            return last;
        }
        if (attempt + 1 < platform::kClipboardMaxAttempts) {
            const auto slept = clock_.sleep_for(
                std::chrono::milliseconds(platform::kClipboardRetryDelayMs), cancellation);
            if (slept.is_error()) {
                return slept;
            }
        }
    }
    return last;
}

// ---------------------------------------------------------------------------
// TextOutputService
// ---------------------------------------------------------------------------

namespace {

/// Probes the focused target with a hard deadline. The backend is supposed to
/// bound its own queries (kTextTargetProbeTimeout), but the delivery must not
/// depend on that: when no verdict arrives, the transcript still has to reach the
/// user, so a timeout is treated as `unknown` and the clipboard path runs. The
/// probe runs on a thread of its own because the caller may be the UI thread,
/// which a hung provider must not be able to freeze.
platform::TextTargetInfo probe_with_deadline(platform::TextTargetProbe& probe)
{
    auto promise = std::make_shared<std::promise<platform::TextTargetInfo>>();
    std::future<platform::TextTargetInfo> verdict = promise->get_future();
    std::thread worker([&probe, promise]() {
        platform::TextTargetInfo info;
        try {
            info = probe.probe();
        } catch (...) {
            // The port says probe() never throws; a backend that does must not
            // take the delivery down with it.
            info.kind = platform::TextTargetKind::unknown;
            info.reason = "probe_threw";
        }
        try {
            promise->set_value(info);
        } catch (...) {
            // The delivery already gave up on this verdict.
        }
    });
    // Detached on purpose: a provider that never answers must not leave a
    // joinable thread behind, and the shared promise keeps the result storage
    // alive until the worker writes into it.
    worker.detach();

    const auto budget = platform::kTextTargetProbeTimeout + platform::kTextTargetReprobeDelay
        + kDirectProbeSlack;
    if (verdict.wait_for(budget) == std::future_status::ready) {
        try {
            return verdict.get();
        } catch (...) {
            // Fall through to the timeout verdict below.
        }
    }
    platform::TextTargetInfo timed_out;
    timed_out.kind = platform::TextTargetKind::unknown;
    timed_out.reason = "probe_timeout";
    return timed_out;
}

} // namespace

TextOutputService::TextOutputService(
    platform::Clipboard& clipboard,
    platform::PasteSimulator& paste,
    platform::TextTargetProbe& probe,
    platform::DirectTextInjector& direct,
    platform::FocusedTextField& field,
    platform::Clock& clock,
    platform::Executor& ui_executor)
    : clipboard_(clipboard)
    , paste_(paste)
    , probe_(probe)
    , direct_(direct)
    , field_(field)
    , clock_(clock)
    , ui_executor_(ui_executor)
{
}

TextOutputService::PasteConfirmation TextOutputService::confirm_paste(
    std::string_view text, const CancellationToken& cancellation)
{
    if (!field_.is_supported()) {
        // No reader on this platform: the old behaviour stands. A verdict the
        // backend cannot make must not be invented (VT-PLT-516).
        return PasteConfirmation::unverifiable;
    }
    const std::string expected = without_whitespace(utf8_suffix(text, kPasteConfirmTailUnits));
    if (expected.empty()) {
        return PasteConfirmation::unverifiable;
    }
    int reads = 0;
    bool saw_content = false;
    for (int attempt = 0; attempt < kPasteConfirmAttempts; ++attempt) {
        if (cancellation.is_cancellation_requested()) {
            // The clipboard already holds the text; the check stops looking.
            return PasteConfirmation::unverifiable;
        }
        std::string tail;
        if (field_.read_caret_tail(kPasteConfirmTailUnits, &tail)) {
            ++reads;
            const std::string normalized = without_whitespace(tail);
            // A console wraps and pads; a document may hold the transcript before a
            // caret that also has older text behind it, so the check asks for the
            // tail, not for equality of the whole read.
            if (!normalized.empty() && ends_with(normalized, expected)) {
                return PasteConfirmation::confirmed;
            }
            if (!normalized.empty()) {
                saw_content = true;
            }
        } else {
            std::string value;
            if (field_.read_value(&value)) {
                ++reads;
                if (value.find(text) != std::string::npos) {
                    return PasteConfirmation::confirmed;
                }
                if (!without_whitespace(value).empty()) {
                    saw_content = true;
                }
            }
        }
        if (attempt + 1 < kPasteConfirmAttempts) {
            if (clock_.sleep_for(kPasteConfirmInterval, cancellation).is_error()) {
                return PasteConfirmation::unverifiable;
            }
        }
    }
    // VT-DOM-618: only a field that could be read more than once and stayed empty
    // proves that the chord did nothing. A field that could not be read, or that
    // holds other text, proves nothing - and a notification there would be a false
    // alarm on a working paste, which is exactly what the 2026-10-19 JetBrains
    // plugin run produced.
    if (reads >= 2 && !saw_content) {
        return PasteConfirmation::failed;
    }
    return PasteConfirmation::unverifiable;
}

void TextOutputService::set_report_sink(std::function<void(const OutputReport&)> sink)
{
    std::lock_guard<std::mutex> lock(report_mutex_);
    report_sink_ = std::move(sink);
}

Result<bool> TextOutputService::output(
    std::string_view text, bool auto_paste, const CancellationToken& cancellation)
{
    // One writer at a time: two sessions must not fight over the clipboard.
    if (in_flight_.test_and_set()) {
        return Result<bool>::failure(ErrorCode::invalid_state, "a text delivery is already in flight");
    }
    struct Release {
        std::atomic_flag& flag;
        ~Release() { flag.clear(); }
    } release{in_flight_};

    std::function<void(const OutputReport&)> sink;
    {
        std::lock_guard<std::mutex> lock(report_mutex_);
        sink = report_sink_;
    }
    const auto started = clock_.now();
    // Per-phase costs, filled as the delivery passes each stage (VT-DOM-618's
    // diagnostics): a single duration_ms cannot tell a slow probe from a slow check.
    long long probe_ms = 0;
    long long insert_ms = 0;
    long long paste_ms = 0;
    long long confirm_ms = 0;
    const auto publish = [&](OutputReport report) {
        report.delivered = platform::is_delivered(report.outcome);
        // The delivery cost is stamped here rather than at each call site, so the
        // log always carries it: recognition and pasting are otherwise one number.
        report.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock_.elapsed_since(started)).count();
        report.probe_ms = probe_ms;
        report.insert_ms = insert_ms;
        report.paste_ms = paste_ms;
        report.confirm_ms = confirm_ms;
        if (sink) {
            try {
                sink(report);
            } catch (...) {
                // A diagnostics sink cannot break delivery.
            }
        }
    };

    if (is_transcript_blank(text) || is_annotation_only(text)) {
        // Both cases mean "there is nothing the user dictated": whitespace, or
        // whisper's non-speech annotation. Neither may touch the clipboard.
        publish(OutputReport{platform::TextOutputOutcome::skipped_empty, std::nullopt, false});
        return false;
    }
    if (cancellation.is_cancellation_requested()) {
        publish(OutputReport{platform::TextOutputOutcome::skipped_empty, std::nullopt, false});
        return Result<bool>::failure(ErrorCode::cancelled, "cancelled before the clipboard write");
    }

    // Direct delivery first (VT-DOM-613). It runs *before* the clipboard write on
    // purpose: a transcript that lands in the focused field must not clobber the
    // user's clipboard, and that is only possible if nothing writes it first.
    std::string_view direct_note = "not_attempted";
    const bool injection_wanted = auto_paste && !suspended_.load();
    if (injection_wanted && direct_insert_.load() && direct_.is_supported()) {
        const auto probe_started = clock_.now();
        const platform::TextTargetInfo target = probe_with_deadline(probe_);
        probe_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock_.elapsed_since(probe_started)).count();
        if (target.accepts_typing()) {
            platform::InsertResult injected;
            bool ran = false;
            const auto insert_started = clock_.now();
            if (ui_executor_.on_this_thread()) {
                // Already on the input-owning thread: inline, like the paste.
                injected = direct_.insert(text, cancellation);
                ran = true;
            } else {
                const auto invoked = ui_executor_.invoke(
                    [&] { injected = direct_.insert(text, cancellation); }, kDirectInsertExecutorTimeout);
                if (invoked.is_error()) {
                    // The typing never ran on the input thread. The transcript
                    // still has to reach the user, so the clipboard path follows.
                    direct_note = "executor_unavailable";
                } else {
                    ran = true;
                }
            }
            insert_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                clock_.elapsed_since(insert_started)).count();
            if (ran && injected.status.is_ok()) {
                // VT-DOM-614: the text is in the field, the clipboard was never
                // touched, and the user can dictate without losing what they had
                // copied. This is the only outcome that skips the clipboard.
                publish(OutputReport{platform::TextOutputOutcome::direct, std::nullopt, false});
                return true;
            }
            if (ran && injected.is_partial()) {
                // VT-DOM-615: a prefix is in the field, so the whole transcript
                // goes to the clipboard and the paste chord stays unsent - a paste
                // on top of the typed prefix would duplicate it.
                direct_note = "partial";
                const auto written = clipboard_.set_text(text, cancellation);
                if (written.is_error()) {
                    publish(OutputReport{platform::TextOutputOutcome::clipboard_failed, std::nullopt, false});
                    return Result<bool>::failure(written.code(), written.message());
                }
                PasteDiagnostics partial;
                partial.injection_supported = paste_.is_injection_supported();
                partial.reason = kReasonDirectPartial;
                partial.direct = direct_note;
                publish(OutputReport{platform::TextOutputOutcome::clipboard_only, partial, true});
                return true;
            }
            if (ran) {
                // The typer refused before delivering anything: the clipboard path
                // below is the fallback, and the note says why the direct attempt
                // did not happen.
                direct_note = "refused";
            }
        } else {
            switch (target.kind) {
            case platform::TextTargetKind::none:
                direct_note = "skipped_no_target";
                break;
            case platform::TextTargetKind::read_only:
                direct_note = "skipped_read_only";
                break;
            case platform::TextTargetKind::unknown:
                direct_note = target.reason == "probe_timeout" ? "probe_timeout" : "skipped_unknown";
                break;
            case platform::TextTargetKind::editable:
                break; // handled above
            }
        }
    }

    // Clipboard first: nothing below may run before the text is safely stored.
    const auto written = clipboard_.set_text(text, cancellation);
    if (written.is_error()) {
        publish(OutputReport{platform::TextOutputOutcome::clipboard_failed, std::nullopt, false});
        return Result<bool>::failure(written.code(), written.message());
    }
    if (!auto_paste) {
        publish(OutputReport{platform::TextOutputOutcome::clipboard_only, std::nullopt, true});
        return true;
    }

    PasteDiagnostics diagnostics;
    diagnostics.injection_supported = paste_.is_injection_supported();
    diagnostics.direct = direct_note;
    if (suspended_.load()) {
        // Hotkey capture owns the keyboard right now: do not inject Ctrl+V.
        diagnostics.reason = kReasonSuspended;
        publish(OutputReport{platform::TextOutputOutcome::clipboard_only, diagnostics, true});
        return true;
    }
    if (!diagnostics.injection_supported) {
        diagnostics.reason = kReasonNoBackend;
        publish(OutputReport{platform::TextOutputOutcome::clipboard_only, diagnostics, true});
        return true;
    }

    const auto delay = clock_.sleep_for(platform::kPasteDelay, cancellation);
    if (delay.is_error()) {
        // The clipboard already holds the text; the paste is simply not done.
        diagnostics.reason = "cancelled_before_paste";
        publish(OutputReport{platform::TextOutputOutcome::clipboard_only, diagnostics, true});
        return Result<bool>::failure(delay.code(), delay.message());
    }

    platform::Status pasted = platform::Status::failure(ErrorCode::unavailable, "paste was not attempted");
    bool attempted = false;
    const auto paste_started = clock_.now();
    if (ui_executor_.on_this_thread()) {
        // Already on the input-owning thread: inline, otherwise a blocking invoke
        // from the UI thread would deadlock on itself.
        pasted = paste_.paste();
        attempted = true;
    } else {
        pasted = ui_executor_.invoke([&] { pasted = paste_.paste(); }, std::chrono::milliseconds(2000));
        if (pasted.is_error()) {
            diagnostics.reason = kReasonExecutor;
            publish(OutputReport{platform::TextOutputOutcome::clipboard_only, diagnostics, true});
            return true;
        }
        attempted = true;
    }
    static_cast<void>(attempted);
    paste_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock_.elapsed_since(paste_started)).count();

    if (pasted.is_ok()) {
        const auto confirm_started = clock_.now();
        PasteConfirmation confirmation = confirm_paste(text, cancellation);
        confirm_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock_.elapsed_since(confirm_started)).count();
        if (confirmation == PasteConfirmation::unverifiable && field_.is_supported()
            && !field_.has_text_input_in_foreground_window()) {
            // VT-PLT-518: nothing in the window can accept text at all, so the chord
            // went nowhere. That is a proven failure and worth telling the user about -
            // measured 2026-10-19, a dictation on the desktop was lost silently. A window
            // that holds a field but hides it from the reader stays silent: there the
            // paste can still have worked.
            confirmation = PasteConfirmation::failed;
        }
        if (confirmation == PasteConfirmation::failed) {
            // VT-DOM-618: the field was readable and stayed empty, so the chord really
            // did nothing. The clipboard holds the transcript, and only this case is
            // worth a user-visible notification.
            diagnostics.reason = kReasonPasteUnconfirmed;
            publish(OutputReport{platform::TextOutputOutcome::clipboard_only, diagnostics, true});
            return true;
        }
        // A paste that cannot be observed is reported as what it is: unverified, not
        // failed. The log keeps the distinction, the user is not alarmed. A platform
        // without a reader at all keeps the plain "pasted" of VT-DOM-602..612: nothing
        // changed there, and the Linux log should not grow a new reason.
        diagnostics.reason = (confirmation == PasteConfirmation::confirmed || !field_.is_supported())
            ? "pasted"
            : "pasted_unverified";
        publish(OutputReport{platform::TextOutputOutcome::pasted, diagnostics, true});
        return true;
    }
    diagnostics.reason = pasted.code() == ErrorCode::permission_denied
        ? kReasonIntegrity
        : kReasonWindowRejected;
    publish(OutputReport{platform::TextOutputOutcome::clipboard_only, diagnostics, true});
    return true;
}

} // namespace voicetyper::domain
