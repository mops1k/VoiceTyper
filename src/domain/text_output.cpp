#include "domain/text_output.hpp"

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

TextOutputService::TextOutputService(
    platform::Clipboard& clipboard,
    platform::PasteSimulator& paste,
    platform::Clock& clock,
    platform::Executor& ui_executor)
    : clipboard_(clipboard)
    , paste_(paste)
    , clock_(clock)
    , ui_executor_(ui_executor)
{
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
    const auto publish = [&](OutputReport report) {
        report.delivered = platform::is_delivered(report.outcome);
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

    if (pasted.is_ok()) {
        diagnostics.reason = "pasted";
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
