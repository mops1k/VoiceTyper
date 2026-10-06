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
    TextOutputService service(clipboard, paste, clock, executor);
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
    TextOutputService service(clipboard, paste, clock, executor);
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
    TextOutputService service(clipboard, paste, clock, executor);
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
    TextOutputService service(clipboard, paste, clock, executor);

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
    TextOutputService service(blocking, paste, clock, executor);

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

} // namespace

int main()
{
    check_blank_unicode();
    check_clipboard_retry();
    check_delivery_paths();
    check_paste_refused_is_success();
    check_suspension_and_executor();
    check_cancellation_rules();
    check_single_writer();

    if (failures != 0) {
        std::cerr << "text-output-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "text-output-contract: OK\n";
    return 0;
}
