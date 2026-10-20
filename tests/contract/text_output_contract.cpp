#include "domain/text_output.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

using namespace voicetyper;
using namespace voicetyper::domain;

/// Virtual clock: time only moves when a sleep is asked for, so the 80 ms and
/// 120 ms numbers are asserted exactly and the test still runs instantly.
class FakeClock final : public platform::Clock {
public:
    std::chrono::steady_clock::time_point now() const override { return now_value; }
    std::chrono::system_clock::time_point wall_now() const override { return {}; }
    std::chrono::steady_clock::duration elapsed_since(std::chrono::steady_clock::time_point start) const override
    {
        return now_value - start;
    }
    platform::Status sleep_for(std::chrono::milliseconds duration, const CancellationToken& token) override
    {
        sleeps.push_back(duration.count());
        if (token.is_cancellation_requested() || cancel_on_sleep) {
            return platform::Status::failure(ErrorCode::cancelled, "cancelled");
        }
        now_value += duration;
        return platform::Status::success();
    }
    platform::Status sleep_until(std::chrono::steady_clock::time_point deadline, const CancellationToken& token) override
    {
        return sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now_value), token);
    }

    std::chrono::steady_clock::time_point now_value{};
    std::vector<long long> sleeps;
    bool cancel_on_sleep = false;
};

class FakeClipboard final : public platform::Clipboard {
public:
    platform::Status set_text(std::string_view text, const CancellationToken&) override
    {
        ++writes;
        if (fail) {
            return platform::Status::failure(ErrorCode::permission_denied, "clipboard locked");
        }
        last = std::string(text);
        return platform::Status::success();
    }
    Result<std::optional<std::string>> get_text() const override { return std::optional<std::string>(last); }
    bool has_text() const override { return !last.empty(); }

    bool fail = false;
    std::string last;
    int writes = 0;
};

class FakePaste final : public platform::PasteSimulator {
public:
    platform::Status paste() override
    {
        ++calls;
        if (fail_code == ErrorCode::ok) {
            return platform::Status::success();
        }
        return platform::Status::failure(fail_code, "injection refused");
    }
    bool is_injection_supported() const noexcept override { return supported; }

    bool supported = true;
    ErrorCode fail_code = ErrorCode::ok;
    int calls = 0;
};

/// The direct-delivery pair (VT-DOM-613). The defaults describe the state every
/// pre-existing check in this file assumes: no probe backend, so the delivery is
/// the clipboard-then-paste path and `probe.calls` stays 0.
class FakeProbe final : public platform::TextTargetProbe {
public:
    platform::TextTargetInfo probe() override
    {
        ++calls;
        return info;
    }
    bool is_supported() const noexcept override { return supported; }

    bool supported = true;
    platform::TextTargetInfo info{};
    int calls = 0;
};

class FakeDirect final : public platform::DirectTextInjector {
public:
    platform::InsertResult insert(std::string_view text, const CancellationToken&) override
    {
        ++calls;
        last = std::string(text);
        platform::InsertResult result;
        result.typed = typed;
        result.via = via;
        result.status = fail_code == ErrorCode::ok
            ? platform::Status::success()
            : platform::Status::failure(fail_code, "typing refused");
        return result;
    }
    bool is_supported() const noexcept override { return supported; }

    bool supported = false;
    ErrorCode fail_code = ErrorCode::ok;
    std::size_t typed = 0;
    platform::InsertVia via = platform::InsertVia::none;
    std::string last;
    int calls = 0;
};

/// VT-DOM-618: the reader the paste check uses. `supported = false` is the default
/// on purpose - those are the platforms (and the older expectations) where the check
/// cannot run and the paste keeps its historical verdict.
class FakeFocusedField final : public platform::FocusedTextField {
public:
    bool read_caret_tail(std::size_t, std::string* utf8) override
    {
        ++tail_reads;
        if (!tail_readable) {
            return false;
        }
        *utf8 = tail;
        return true;
    }
    bool read_value(std::string* utf8) override
    {
        ++value_reads;
        if (!value_readable) {
            return false;
        }
        *utf8 = value;
        return true;
    }
    bool selection_present() override { return selection; }
    bool is_supported() const noexcept override { return supported; }
    bool has_text_input_in_foreground_window() override
    {
        ++window_queries;
        return window_has_text_input;
    }

    bool supported = false;
    bool window_has_text_input = true;
    int window_queries = 0;
    bool tail_readable = false;
    bool value_readable = false;
    bool selection = false;
    std::string tail;
    std::string value;
    int tail_reads = 0;
    int value_reads = 0;
};

/// Executor that runs inline (on_this_thread() == true) or marshals, so both
/// affinity paths are covered deterministically.
class RecordingExecutor final : public platform::Executor {
public:
    void post(platform::Task) override {}
    void post(platform::TaskGeneration, platform::Task) override {}
    void post_delayed(platform::TaskGeneration, platform::Task, std::chrono::milliseconds) override {}
    platform::Status invoke(platform::Task task, std::chrono::milliseconds) override
    {
        ++invokes;
        if (fail) {
            return platform::Status::failure(ErrorCode::timeout, "ui thread did not answer");
        }
        task();
        return platform::Status::success();
    }
    platform::Status shutdown(std::chrono::milliseconds) override { return platform::Status::success(); }
    bool on_this_thread() const noexcept override { return inline_mode; }
    std::size_t pending() const noexcept override { return 0; }

    bool inline_mode = true;
    bool fail = false;
    int invokes = 0;
};

void check_blank_unicode()
{
    check(is_transcript_blank("") && is_transcript_blank("  \t\r\n"), "ASCII whitespace is blank");
    check(is_transcript_blank("\xC2\xA0"), "NBSP is blank");
    check(is_transcript_blank("\xE3\x80\x80"), "the ideographic space is blank");
    check(is_transcript_blank("\xE2\x80\xA8"), "U+2028 is blank");
    check(!is_transcript_blank("\xC2\xA0x"), "a non-space code point is not blank");
    check(!is_transcript_blank(" привет "), "cyrillic text is not blank");
}

void check_clipboard_retry()
{
    FakeClipboard clipboard;
    FakeClock clock;
    RetryingClipboard retrying(clipboard, clock);

    check(retrying.set_text("hello", {}).is_ok() && retrying.last_attempts() == 1,
        "a free clipboard needs one attempt");
    check(clock.sleeps.empty(), "a free clipboard does not sleep");

    clipboard.fail = true;
    clock.sleeps.clear();
    check(retrying.set_text("hello", {}).is_error(), "a locked clipboard eventually fails");
    check(retrying.last_attempts() == platform::kClipboardMaxAttempts, "all five attempts are used");
    check(clock.sleeps.size() == platform::kClipboardMaxAttempts - 1, "there is no sleep after the final attempt");
    check(clock.sleeps.front() == platform::kClipboardRetryDelayMs, "the retry gap is 120 ms");
}

void check_delivery_paths()
{
    FakeClipboard clipboard;
    FakePaste paste;
    FakeClock clock;
    RecordingExecutor executor;
    FakeProbe probe;
    FakeDirect direct;
    FakeFocusedField field;
    TextOutputService service(clipboard, paste, probe, direct, field, clock, executor);
    std::vector<OutputReport> reports;
    service.set_report_sink([&](const OutputReport& report) { reports.push_back(report); });

    const auto blank = service.output("  \xC2\xA0 ", true, {});
    check(blank.is_ok() && !blank.value(), "a blank transcript is a successful no-op");
    check(clipboard.writes == 0, "a blank transcript never touches the clipboard");
    check(reports.back().outcome == platform::TextOutputOutcome::skipped_empty, "skipped_empty is reported");

    // whisper describes non-speech input instead of returning nothing, and a
    // microphone that only heard the room produced "[звук сетки] [звук сетки]" on
    // the target machine. Typing that into the user's document is a defect, so an
    // annotation-only transcript is treated exactly like a blank one.
    const auto annotation = service.output("[\xD0\xB7\xD0\xB2\xD1\x83\xD0\xBA \xD1\x81\xD0\xB5\xD1\x82\xD0\xBA\xD0\xB8] [\xD0\xB7\xD0\xB2\xD1\x83\xD0\xBA \xD1\x81\xD0\xB5\xD1\x82\xD0\xBA\xD0\xB8]", true, {});
    check(annotation.is_ok() && !annotation.value(), "an annotation-only transcript is not delivered");
    check(clipboard.writes == 0, "an annotation-only transcript never touches the clipboard");
    check(reports.back().outcome == platform::TextOutputOutcome::skipped_empty,
        "an annotation-only transcript reports skipped_empty");
    const auto asterisks = service.output("*\xD0\xB7\xD0\xB2\xD1\x83\xD0\xBA*", true, {});
    check(asterisks.is_ok() && !asterisks.value(), "an asterisk annotation is not delivered");
    const auto parenthesised = service.output("(music)", true, {});
    check(parenthesised.is_ok() && !parenthesised.value(), "a parenthesised annotation is not delivered");

    // Real text that merely contains an annotation stays real text.
    const auto mixed = service.output("[\xD0\xBC\xD1\x83\xD0\xB7\xD1\x8B\xD0\xBA\xD0\xB0] \xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", false, {});
    check(mixed.is_ok() && mixed.value(), "a transcript with real words is still delivered");
    check(clipboard.writes == 1, "the mixed transcript reached the clipboard");

    const auto clipboard_only = service.output("hello", false, {});
    check(clipboard_only.is_ok() && clipboard_only.value(), "auto-paste off still delivers the text");
    check(paste.calls == 0 && clock.sleeps.empty(), "no paste and no delay when auto-paste is off");
    check(reports.back().outcome == platform::TextOutputOutcome::clipboard_only, "clipboard_only is reported");

    clock.sleeps.clear();
    const auto pasted = service.output("hello", true, {});
    check(pasted.is_ok() && pasted.value() && paste.calls == 1, "a successful paste delivers");
    check(clock.sleeps.size() == 1 && clock.sleeps.front() == 80, "the paste happens 80 ms after the write");
    check(reports.back().outcome == platform::TextOutputOutcome::pasted, "pasted is reported");
    check(executor.invokes == 0, "on the owning thread the paste runs inline");
}

void check_paste_refused_is_success()
{
    FakeClipboard clipboard;
    FakePaste paste;
    FakeClock clock;
    RecordingExecutor executor;
    FakeProbe probe;
    FakeDirect direct;
    FakeFocusedField field;
    TextOutputService service(clipboard, paste, probe, direct, field, clock, executor);
    std::vector<OutputReport> reports;
    service.set_report_sink([&](const OutputReport& report) { reports.push_back(report); });

    paste.fail_code = ErrorCode::permission_denied; // the UAC case
    const auto result = service.output("hello", true, {});
    check(result.is_ok() && result.value(), "a refused paste still counts as delivered");
    check(clipboard.last == "hello", "the transcript stays on the clipboard");
    check(reports.back().outcome == platform::TextOutputOutcome::clipboard_only, "the state is clipboard_only");
    check(reports.back().paste.has_value() && reports.back().paste->reason == "integrity_level",
        "the UAC case is reported as an integrity mismatch");

    paste.fail_code = ErrorCode::ok;
    paste.supported = false;
    reports.clear();
    const auto unsupported = service.output("hello", true, {});
    check(unsupported.is_ok(), "a missing injection backend is not an error");
    check(reports.back().paste->reason == "no_injection_backend", "the reason names the missing backend");
}

void check_suspension_and_executor()
{
    FakeClipboard clipboard;
    FakePaste paste;
    FakeClock clock;
    RecordingExecutor executor;
    FakeProbe probe;
    FakeDirect direct;
    FakeFocusedField field;
    TextOutputService service(clipboard, paste, probe, direct, field, clock, executor);
    std::vector<OutputReport> reports;
    service.set_report_sink([&](const OutputReport& report) { reports.push_back(report); });

    service.set_paste_suspended(true);
    check(service.paste_suspended(), "suspension is observable");
    const auto suspended = service.output("hello", true, {});
    check(suspended.is_ok() && suspended.value() && paste.calls == 0, "a suspended session does not inject");
    check(reports.back().paste->reason == "suspended_for_hotkey_capture", "the reason names hotkey capture");
    service.set_paste_suspended(false);

    executor.inline_mode = false;
    executor.fail = true;
    reports.clear();
    const auto failed = service.output("hello", true, {});
    check(failed.is_ok() && failed.value(), "a UI thread that does not answer still keeps the text");
    check(executor.invokes == 1, "the paste was marshalled through the executor");
    check(reports.back().paste->reason == "executor_unavailable", "the reason names the executor");
    executor.fail = false;
    reports.clear();
    const auto marshalled = service.output("hello", true, {});
    check(marshalled.is_ok() && paste.calls == 1, "a marshalled paste still works");
}

void check_cancellation_rules()
{
    FakeClipboard clipboard;
    FakePaste paste;
    FakeClock clock;
    RecordingExecutor executor;
    FakeProbe probe;
    FakeDirect direct;
    FakeFocusedField field;
    TextOutputService service(clipboard, paste, probe, direct, field, clock, executor);

    CancellationSource pre;
    pre.request_cancellation();
    const auto cancelled = service.output("hello", true, pre.token());
    check(cancelled.is_error() && cancelled.code() == ErrorCode::cancelled,
        "a pre-cancelled token writes nothing");
    check(clipboard.writes == 0, "a pre-cancelled delivery never touches the clipboard");

    clock.cancel_on_sleep = true;
    const auto during = service.output("hello", true, {});
    check(during.is_error() && during.code() == ErrorCode::cancelled, "cancelling during the delay fails the call");
    check(clipboard.last == "hello", "but the clipboard keeps the text");
    check(paste.calls == 0, "and no paste is attempted");
}

void check_direct_delivery()
{
    FakeClipboard clipboard;
    FakePaste paste;
    FakeClock clock;
    RecordingExecutor executor;
    FakeProbe probe;
    FakeDirect direct;
    FakeFocusedField field;
    TextOutputService service(clipboard, paste, probe, direct, field, clock, executor);
    std::vector<OutputReport> reports;
    service.set_report_sink([&](const OutputReport& report) { reports.push_back(report); });

    // A recognised text field: the transcript is typed and the clipboard is never
    // touched — the whole point of the direct path (VT-DOM-613/614).
    direct.supported = true;
    direct.typed = 5;
    probe.info.kind = platform::TextTargetKind::editable;
    probe.info.reason = "control_type_edit";
    const auto typed = service.output("hello", true, {});
    check(typed.is_ok() && typed.value(), "a typed delivery counts as delivered");
    check(direct.calls == 1 && direct.last == "hello", "the transcript was typed into the field");
    check(clipboard.writes == 0, "a typed delivery never writes the clipboard");
    check(paste.calls == 0, "a typed delivery never sends Ctrl+V");
    check(reports.back().outcome == platform::TextOutputOutcome::direct, "the outcome is direct");
    check(reports.back().delivered, "direct is a delivered state");
    check(!reports.back().paste.has_value(), "a typed delivery carries no paste diagnostics");

    // A partial typing (VT-DOM-615): the clipboard gets the whole transcript and
    // the paste chord stays unsent, because pasting would duplicate the prefix.
    direct.fail_code = ErrorCode::cancelled;
    direct.typed = 3;
    clipboard.writes = 0;
    paste.calls = 0;
    reports.clear();
    const auto partial = service.output("hello", true, {});
    check(partial.is_ok() && partial.value(), "a partial typing still delivers");
    check(clipboard.last == "hello", "the clipboard holds the whole transcript after a partial typing");
    check(paste.calls == 0, "a partial typing does not paste over its own prefix");
    check(reports.back().outcome == platform::TextOutputOutcome::clipboard_only,
        "a partial typing is reported as clipboard_only");
    check(reports.back().paste.has_value() && reports.back().paste->reason == "direct_partial",
        "the reason names the partial typing");
    check(reports.back().paste->direct == "partial", "the direct note names the partial typing");

    // An unconfirmed direct insertion (VT-DOM-619): the whole burst was posted, but the
    // field never confirmed it. The clipboard gets the transcript and NO Ctrl+V may
    // follow - the field may already hold the text and expose it late, and the fallback
    // paste is exactly what inserted it twice on 2026-10-20.
    direct.fail_code = ErrorCode::unavailable;
    direct.typed = 5;
    direct.via = platform::InsertVia::unconfirmed;
    clipboard.writes = 0;
    paste.calls = 0;
    reports.clear();
    const auto unconfirmed = service.output("hello", true, {});
    check(unconfirmed.is_ok() && unconfirmed.value(),
        "an unconfirmed typing still delivers through the clipboard");
    check(clipboard.writes == 1 && clipboard.last == "hello",
        "the clipboard holds the transcript of an unconfirmed typing");
    check(paste.calls == 0, "an unconfirmed typing never sends Ctrl+V over a delivery that may have landed");
    check(reports.back().outcome == platform::TextOutputOutcome::clipboard_only,
        "an unconfirmed typing is reported as clipboard_only");
    check(reports.back().delivered, "clipboard_only stays a delivered state");
    check(reports.back().paste.has_value() && reports.back().paste->reason == "direct_unconfirmed",
        "the reason names the unconfirmed typing");
    check(reports.back().paste->direct == "unconfirmed", "the direct note names the unconfirmed typing");

    // The executor lost the injector's verdict: the task may already be running, so the
    // paste chord must stay unsent for the same reason (VT-DOM-619).
    direct.via = platform::InsertVia::none;
    direct.fail_code = ErrorCode::ok;
    direct.typed = 5;
    executor.inline_mode = false;
    executor.fail = true;
    clipboard.writes = 0;
    paste.calls = 0;
    reports.clear();
    const auto executor_lost = service.output("hello", true, {});
    check(executor_lost.is_ok() && executor_lost.value(),
        "an executor that loses the verdict still delivers through the clipboard");
    check(clipboard.last == "hello" && clipboard.writes == 1,
        "the clipboard holds the transcript when the executor loses the verdict");
    check(paste.calls == 0, "no Ctrl+V follows an executor that lost the direct verdict");
    check(reports.back().paste.has_value() && reports.back().paste->reason == "direct_unconfirmed",
        "the lost verdict is reported as an unconfirmed direct delivery");
    check(reports.back().paste->direct == "unconfirmed",
        "the direct note names the lost verdict as unconfirmed");
    executor.fail = false;
    executor.inline_mode = true;

    // Nothing was typed: the ordinary clipboard-then-paste path runs, and the note
    // says the typer refused rather than the field never being recognised.
    direct.fail_code = ErrorCode::permission_denied;
    direct.typed = 0;
    paste.calls = 0;
    reports.clear();
    const auto refused = service.output("hello", true, {});
    check(refused.is_ok() && refused.value() && paste.calls == 1, "a refused typing falls back to the paste");
    check(reports.back().paste->direct == "refused", "the direct note names the refusal");

    // A target that is provably not a text field: nothing is typed, and the note
    // says why the direct attempt never happened.
    direct.fail_code = ErrorCode::ok;
    const int inserts_before = direct.calls;
    probe.info.kind = platform::TextTargetKind::none;
    reports.clear();
    const auto desktop = service.output("hello", true, {});
    check(desktop.is_ok() && desktop.value(), "a non-text target still delivers through the clipboard");
    check(direct.calls == inserts_before, "nothing is typed into a non-text target");
    check(reports.back().paste->direct == "skipped_no_target", "the direct note names the non-text target");

    // An unknown verdict falls back too, and says so: VT-PLT-506 keeps `unknown` a
    // different state from `none`.
    probe.info.kind = platform::TextTargetKind::unknown;
    probe.info.reason = "document_without_value_pattern";
    reports.clear();
    const auto unknown = service.output("hello", true, {});
    check(unknown.is_ok() && unknown.value(), "an unknown target still delivers");
    check(reports.back().paste->direct == "skipped_unknown", "the direct note names the unknown verdict");

    // The setting off (VT-DOM-617): the probe is not consulted at all.
    service.set_direct_insert(false);
    check(!service.direct_insert(), "the direct setting is observable");
    const int probes_before = probe.calls;
    reports.clear();
    const auto off = service.output("hello", true, {});
    check(off.is_ok() && probe.calls == probes_before, "direct delivery off never probes");
    check(reports.back().paste->direct == "not_attempted", "the direct note stays not_attempted");
    check(reports.back().paste->reason == "pasted", "the clipboard path is unchanged when the setting is off");

    // Auto-paste off means the user asked for the clipboard only: no probe either.
    service.set_direct_insert(true);
    const int probes_after = probe.calls;
    const auto clipboard_only = service.output("hello", false, {});
    check(clipboard_only.is_ok() && clipboard_only.value(), "auto-paste off still delivers");
    check(probe.calls == probes_after, "auto-paste off never probes");
}

class BlockingClipboard final : public platform::Clipboard {
public:
    platform::Status set_text(std::string_view, const CancellationToken&) override
    {
        entered.store(true);
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return platform::Status::success();
    }
    Result<std::optional<std::string>> get_text() const override { return std::optional<std::string>(); }
    bool has_text() const override { return false; }

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

void check_single_writer()
{
    // One service, one clipboard: the guard is per delivery pipeline, and a second
    // request while the first is inside set_text is rejected instead of racing.
    BlockingClipboard blocking;
    FakePaste paste;
    FakeClock clock;
    RecordingExecutor executor;
    FakeProbe probe;
    FakeDirect direct;
    FakeFocusedField field;
    TextOutputService service(blocking, paste, probe, direct, field, clock, executor);

    std::thread worker([&] { static_cast<void>(service.output("hello", false, {})); });
    while (!blocking.entered.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto second = service.output("hello", false, {});
    check(second.is_error() && second.code() == ErrorCode::invalid_state,
        "a second concurrent delivery is rejected instead of racing the clipboard");
    blocking.release = true;
    worker.join();
    check(service.output("hello", false, {}).is_ok(), "a later delivery works once the first finished");
}


void check_paste_confirmation()
{
    // A field that can be read but never shows the text: the chord was accepted,
    // the delivery did not happen, and the outcome must say so instead of "pasted"
    // (the Windows Terminal case of 2026-10-19).
    {
        FakeClipboard clipboard;
        FakePaste paste;
        FakeClock clock;
        RecordingExecutor executor;
        FakeProbe probe;
        FakeDirect direct;
        FakeFocusedField field;
        field.supported = true;
        field.tail_readable = true;
        field.tail = "some older text";
        TextOutputService service(clipboard, paste, probe, direct, field, clock, executor);
        std::vector<OutputReport> reports;
        service.set_report_sink([&](const OutputReport& report) { reports.push_back(report); });

        const auto result = service.output("hello world", true, {});
        check(result.is_ok() && result.value(), "an unobservable paste is still a delivered outcome");
        check(clipboard.last == "hello world", "the transcript stays on the clipboard");
        // A readable field that holds other text proves nothing: the reader may be
        // looking at another element (measured 2026-10-19 in a JetBrains plugin, where
        // the paste worked and an alarm was raised anyway). Unverifiable is not failed.
        check(reports.back().outcome == platform::TextOutputOutcome::pasted,
            "an unverifiable paste is reported as pasted, not as a failure");
        check(reports.back().paste.has_value() && reports.back().paste->reason == "pasted_unverified",
            "the log names the paste as unverified");
        check(field.tail_reads >= voicetyper::domain::kPasteConfirmAttempts,
            "the check keeps looking for the bounded number of attempts");
        // One 80 ms paste delay plus one gap between the confirmation attempts, and
        // no gap after the last attempt.
        check(clock.sleeps.size() == static_cast<std::size_t>(voicetyper::domain::kPasteConfirmAttempts),
            "the check waits between attempts and not after the last one");
        check(clock.sleeps.front() == 80, "the paste delay comes first");
    }

    // A field that can be read and stays empty: the chord demonstrably did nothing, and
    // that is the only case worth telling the user about (VT-DOM-618).
    {
        FakeClipboard clipboard;
        FakePaste paste;
        FakeClock clock;
        RecordingExecutor executor;
        FakeProbe probe;
        FakeDirect direct;
        FakeFocusedField field;
        field.supported = true;
        field.tail_readable = true;
        field.tail = "";
        TextOutputService service(clipboard, paste, probe, direct, field, clock, executor);
        std::vector<OutputReport> reports;
        service.set_report_sink([&](const OutputReport& report) { reports.push_back(report); });

        static_cast<void>(service.output("hello world", true, {}));
        check(reports.back().outcome == platform::TextOutputOutcome::clipboard_only,
            "an empty readable field proves the paste failed, so the outcome is clipboard_only");
        check(reports.back().paste.has_value() && reports.back().paste->reason == "paste_unconfirmed",
            "the proven failure is named paste_unconfirmed");
    }

    // A window with no text-capable element at all cannot have received the paste
    // (VT-PLT-518): dictating on the desktop must warn the user, not stay silent.
    {
        FakeClipboard clipboard;
        FakePaste paste;
        FakeClock clock;
        RecordingExecutor executor;
        FakeProbe probe;
        FakeDirect direct;
        FakeFocusedField field;
        field.supported = true;
        field.tail_readable = false;
        field.value_readable = false;
        field.window_has_text_input = false;
        TextOutputService service(clipboard, paste, probe, direct, field, clock, executor);
        std::vector<OutputReport> reports;
        service.set_report_sink([&](const OutputReport& report) { reports.push_back(report); });

        static_cast<void>(service.output("hello world", true, {}));
        check(field.window_queries == 1, "the window search runs once, and only when the paste is unreadable");
        check(reports.back().outcome == platform::TextOutputOutcome::clipboard_only,
            "a window without any text field proves the paste failed");
        check(reports.back().paste.has_value() && reports.back().paste->reason == "paste_unconfirmed",
            "the desktop case is named paste_unconfirmed");
    }

    // The same unreadable paste in a window that does hold a field must stay silent:
    // that is the JetBrains-plugin case, where the paste worked (2026-10-19).
    {
        FakeClipboard clipboard;
        FakePaste paste;
        FakeClock clock;
        RecordingExecutor executor;
        FakeProbe probe;
        FakeDirect direct;
        FakeFocusedField field;
        field.supported = true;
        field.tail_readable = false;
        field.value_readable = false;
        field.window_has_text_input = true;
        TextOutputService service(clipboard, paste, probe, direct, field, clock, executor);
        std::vector<OutputReport> reports;
        service.set_report_sink([&](const OutputReport& report) { reports.push_back(report); });

        static_cast<void>(service.output("hello world", true, {}));
        check(reports.back().outcome == platform::TextOutputOutcome::pasted,
            "an unreadable paste in a window that has a field stays pasted");
        check(reports.back().paste.has_value() && reports.back().paste->reason == "pasted_unverified",
            "and is logged as unverified");
    }

    // A console wraps the pasted text with CRLF and pads the line: whitespace must
    // not turn a real delivery into a false alarm.
    {
        FakeClipboard clipboard;
        FakePaste paste;
        FakeClock clock;
        RecordingExecutor executor;
        FakeProbe probe;
        FakeDirect direct;
        FakeFocusedField field;
        field.supported = true;
        field.tail_readable = true;
        field.tail = "C:\\Users> hello   \r\nworld\r\n";
        TextOutputService service(clipboard, paste, probe, direct, field, clock, executor);
        std::vector<OutputReport> reports;
        service.set_report_sink([&](const OutputReport& report) { reports.push_back(report); });

        const auto result = service.output("hello world", true, {});
        check(result.is_ok(), "a wrapped console line still delivers");
        check(reports.back().outcome == platform::TextOutputOutcome::pasted,
            "a wrapped console line is confirmed as pasted");
    }

    // A field without a text pattern but with a value: the check reads the value,
    // which is how a classic edit control confirms a paste.
    {
        FakeClipboard clipboard;
        FakePaste paste;
        FakeClock clock;
        RecordingExecutor executor;
        FakeProbe probe;
        FakeDirect direct;
        FakeFocusedField field;
        field.supported = true;
        field.tail_readable = false;
        field.value_readable = true;
        field.value = "before hello world";
        TextOutputService service(clipboard, paste, probe, direct, field, clock, executor);
        std::vector<OutputReport> reports;
        service.set_report_sink([&](const OutputReport& report) { reports.push_back(report); });

        static_cast<void>(service.output("hello world", true, {}));
        check(reports.back().outcome == platform::TextOutputOutcome::pasted,
            "a value pattern confirms the paste");
    }
}

/// VT-DOM-620: the one leading space that continues a sentence, and the selection that
/// must never get it. The predicate is portable; the service cases pin the delivery text
/// that reaches the injector, the clipboard and the paste.
void check_leading_space()
{
    // The rule itself.
    check(needs_leading_space(".", "Мир"), "a period right before the caret takes a space");
    check(needs_leading_space("!", "Мир"), "an exclamation mark right before the caret takes a space");
    check(needs_leading_space("?", "Мир"), "a question mark right before the caret takes a space");
    check(needs_leading_space("\xE2\x80\xA6", "Мир"), "an ellipsis right before the caret takes a space");
    check(needs_leading_space("\xD0\xB0.", "Мир"),
        "only the code point immediately before the caret counts");
    check(!needs_leading_space(". ", "Мир"), "a space after the period means the sentence is separated");
    check(!needs_leading_space(",", "Мир"), "a comma is not a sentence terminator");
    check(!needs_leading_space("word", "Мир"), "a letter before the caret is not punctuation");
    check(!needs_leading_space("", "Мир"), "an unreadable tail adds nothing");
    check(!needs_leading_space(".", " Мир"), "a transcript that already starts with a space gets no second one");
    check(!needs_leading_space(".", "\nМир"), "a transcript that starts with any whitespace needs no space");

    // Helper: one delivery with the given caret context and target verdict.
    struct Outcome {
        OutputReport report;
        std::string direct_text;
        std::string clipboard_text;
        int clipboard_writes = 0;
        int paste_calls = 0;
    };
    const auto run = [](std::string_view tail, bool tail_readable, bool selection,
                        platform::TextTargetKind kind, bool auto_paste) {
        FakeClipboard clipboard;
        FakePaste paste;
        FakeClock clock;
        RecordingExecutor executor;
        FakeProbe probe;
        FakeDirect direct;
        FakeFocusedField field;
        direct.supported = true;
        direct.typed = 5;
        probe.info.kind = kind;
        probe.info.reason = "control_type_edit";
        field.supported = true;
        field.tail_readable = tail_readable;
        field.tail = std::string(tail);
        field.selection = selection;
        TextOutputService service(clipboard, paste, probe, direct, field, clock, executor);
        Outcome outcome;
        service.set_report_sink([&](const OutputReport& report) { outcome.report = report; });
        static_cast<void>(service.output("hello", auto_paste, {}));
        outcome.direct_text = direct.last;
        outcome.clipboard_text = clipboard.last;
        outcome.clipboard_writes = clipboard.writes;
        outcome.paste_calls = paste.calls;
        return outcome;
    };

    // Appending after a period: the injected text carries the space and the clipboard
    // stays untouched (a direct delivery).
    const Outcome appended = run(".", true, false, platform::TextTargetKind::editable, true);
    check(appended.direct_text == " hello", "the injector receives the transcript with one leading space");
    check(appended.clipboard_writes == 0, "a spaced direct delivery still leaves the clipboard alone");
    check(appended.report.outcome == platform::TextOutputOutcome::direct, "the delivery stays direct");
    check(appended.report.leading_space, "the report says the space was added");

    // A selection is about to be replaced: the space would land before the replacement
    // and is wrong (the 2026-10-20 refinement).
    const Outcome selected = run(".", true, true, platform::TextTargetKind::editable, true);
    check(selected.direct_text == "hello", "a selection replacement gets no leading space");
    check(!selected.report.leading_space, "the report says no space was added for a selection");

    // The clipboard path gets the same spaced text, so a fallback does not diverge from
    // what a direct delivery would have inserted.
    const Outcome pasted = run(".", true, false, platform::TextTargetKind::none, true);
    check(pasted.clipboard_text == " hello", "the clipboard holds the spaced transcript on the paste path");
    check(pasted.paste_calls == 1, "the paste path still runs its chord");
    check(pasted.report.leading_space, "the paste path reports the leading space");

    // Best effort: no answer from the reader means no space, never a guess.
    const Outcome unreadable = run(".", false, false, platform::TextTargetKind::editable, true);
    check(unreadable.direct_text == "hello", "an unreadable caret tail adds no space");
    check(!unreadable.report.leading_space, "an unreadable caret tail reports no space");

    // Automatic pasting off: the clipboard is the whole delivery, and it is not the
    // field's continuation, so the transcript is copied verbatim.
    const Outcome copied = run(".", true, false, platform::TextTargetKind::editable, false);
    check(copied.clipboard_text == "hello", "a clipboard-only delivery is copied without a leading space");
    check(copied.paste_calls == 0, "a clipboard-only delivery sends no chord");
    check(!copied.report.leading_space, "a clipboard-only delivery reports no space");
}

} // namespace

int main()
{
    check_blank_unicode();
    check_clipboard_retry();
    check_delivery_paths();
    check_paste_refused_is_success();
    check_suspension_and_executor();
    check_cancellation_rules();
    check_direct_delivery();
    check_paste_confirmation();
    check_single_writer();
    check_leading_space();

    if (failures != 0) {
        std::cerr << "text-output-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "text-output-contract: OK\n";
    return 0;
}
