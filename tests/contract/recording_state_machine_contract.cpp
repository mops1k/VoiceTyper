// Contract test for the portable recording state machine.
//
// The .NET reference is VoiceTyper.Tests/Services/RecordingStateMachineTests.cs;
// the scenarios below are the same observable behavior, expressed over abstract
// ports so that no microphone, model or platform backend is involved. Every
// scenario is deterministic: the machine is driven through a manual worker, and
// the two thread-based cases are synchronized with latches instead of sleeps.

#include "domain/recording_state_machine.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace voicetyper::domain;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

std::vector<float> ramp(std::size_t count, float first = 0.1f)
{
    std::vector<float> samples;
    samples.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        samples.push_back(first + static_cast<float>(i) * 0.001f);
    }
    return samples;
}

/// Deterministic executor: nothing runs until the test asks for it.
class ManualWorker final : public RecordingWorker {
public:
    Status post(Task task) override
    {
        if (!task) {
            return Status::failure(ErrorCode::invalid_argument, "empty task");
        }
        queue_.push_back(std::move(task));
        return Status::success();
    }

    void join() override
    {
        while (!queue_.empty()) {
            run_next();
        }
    }

    void run_next()
    {
        if (queue_.empty()) {
            return;
        }
        Task task = std::move(queue_.front());
        queue_.erase(queue_.begin());
        task();
    }

    [[nodiscard]] std::size_t pending() const { return queue_.size(); }

private:
    std::vector<Task> queue_;
};

class FakeRecorder final : public RecordingPort {
public:
    Status start(const CancellationToken& cancellation) override
    {
        starts += 1;
        last_start_token_cancellable = cancellation.can_be_cancelled();
        if (start_status.is_error()) {
            return start_status;
        }
        captured = samples;
        return Status::success();
    }

    Result<SampleBuffer> stop() override
    {
        stops += 1;
        call_order.push_back("stop");
        if (stop_status.is_error()) {
            return Result<SampleBuffer>::failure(stop_status.code(), stop_status.message());
        }
        SampleBuffer buffer(captured.size() + 16U);
        if (!captured.empty()) {
            (void)buffer.append(captured);
        }
        return buffer;
    }

    Status cancel() override
    {
        cancels += 1;
        call_order.push_back("cancel");
        return Status::success();
    }

    Result<SampleBuffer> drain() override
    {
        drains += 1;
        call_order.push_back("drain");
        if (drain_chunk.empty()) {
            return Result<SampleBuffer>::failure(ErrorCode::invalid_state, "no drain chunk configured");
        }
        SampleBuffer buffer(drain_chunk.size() + 16U);
        (void)buffer.append(drain_chunk);
        return buffer;
    }

    // configuration
    Status start_status;
    Status stop_status;
    std::vector<float> samples = ramp(4800);
    std::vector<float> drain_chunk;
    // observation
    int starts = 0;
    int stops = 0;
    int cancels = 0;
    int drains = 0;
    bool last_start_token_cancellable = false;
    std::vector<float> captured;
    std::vector<std::string> call_order;
};

class FakeTranscriber final : public TranscriptionPort {
public:
    Result<std::string> transcribe(
        const SampleBuffer& audio,
        const SessionOptions& options,
        const CancellationToken& cancellation) override
    {
        calls += 1;
        captured_samples = audio.samples();
        captured_options = options;
        order.push_back("transcribe");
        if (block) {
            // Cooperative: waits for the token instead of sleeping blindly.
            std::unique_lock<std::mutex> lock(gate_mutex);
            released.wait(lock, [this, &cancellation] {
                return gate_open.load(std::memory_order_acquire)
                       || cancellation.is_cancellation_requested();
            });
            gate_open.store(false, std::memory_order_release);
        }
        if (cancellation.is_cancellation_requested()) {
            return Result<std::string>::failure(ErrorCode::cancelled, "cancelled in transcribe");
        }
        if (result.is_error()) {
            return result.error();
        }
        return result.value();
    }

    /// Lets a blocked transcribe return, for the "cancel during processing" case.
    void release()
    {
        {
            std::lock_guard<std::mutex> lock(gate_mutex);
            gate_open.store(true, std::memory_order_release);
        }
        released.notify_all();
    }

    bool block = false;
    Result<std::string> result = std::string("hello world");
    int calls = 0;
    std::vector<float> captured_samples;
    SessionOptions captured_options;
    std::vector<std::string> order;
    std::mutex gate_mutex;
    std::condition_variable released;
    std::atomic<bool> gate_open{false};
};

class FakeOutput final : public TextOutputPort {
public:
    Result<bool> output(
        std::string_view text,
        bool auto_paste,
        const CancellationToken& cancellation) override
    {
        calls += 1;
        texts.emplace_back(text);
        last_auto_paste = auto_paste;
        last_token_cancellable = cancellation.can_be_cancelled();
        if (status.is_error()) {
            return Result<bool>::failure(status.code(), status.message());
        }
        return deliver;
    }

    Status status;
    bool deliver = true;
    int calls = 0;
    bool last_auto_paste = false;
    bool last_token_cancellable = false;
    std::vector<std::string> texts;
};

class FakeSegmenter final : public SpeechSegmenter {
public:
    std::vector<SpeechSegment> detect_speech_no_reset(const std::vector<float>& samples) override
    {
        detect_calls += 1;
        fed_samples += samples.size();
        if (speech_reported) {
            return {};
        }
        speech_reported = true;
        return {SpeechSegment{0.0, 0.25}};
    }

    void reset() override
    {
        resets += 1;
        speech_reported = false;
    }

    int resets = 0;
    int detect_calls = 0;
    std::size_t fed_samples = 0;
    bool speech_reported = false;
};

struct Recorder {
    std::vector<RecordingState> states;
    std::vector<std::string> texts;
    std::vector<ErrorCode> failures;
    std::vector<std::string> failure_messages;
};

/// Wires the handlers and records the observable event stream.
void observe(RecordingStateMachine& machine, Recorder& recorder)
{
    machine.set_state_changed_handler([&recorder](RecordingState state) {
        recorder.states.push_back(state);
    });
    machine.set_text_ready_handler([&recorder](std::string text) {
        recorder.texts.push_back(std::move(text));
    });
    machine.set_failed_handler([&recorder](Error error) {
        recorder.failures.push_back(error.code());
        recorder.failure_messages.push_back(error.message());
    });
}

[[nodiscard]] bool contains(const std::vector<RecordingState>& states, RecordingState value)
{
    for (const RecordingState state : states) {
        if (state == value) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::size_t count_state(const std::vector<RecordingState>& states, RecordingState value)
{
    std::size_t total = 0;
    for (const RecordingState state : states) {
        if (state == value) {
            ++total;
        }
    }
    return total;
}

RecordingStateMachineOptions options_for(RecordingMode mode)
{
    RecordingStateMachineOptions options;
    options.mode = mode;
    options.vad_poll_interval = std::chrono::milliseconds(0);
    return options;
}

// ---------------------------------------------------------------------------

void check_push_to_talk()
{
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    FakeOutput output;
    ManualWorker worker;
    RecordingStateMachine machine(
        recorder, transcriber, output, worker, options_for(RecordingMode::push_to_talk));
    Recorder events;
    observe(machine, events);
    machine.set_options_provider([] {
        SessionOptions options;
        options.prompt = "terms";
        options.auto_paste = false;
        return options;
    });

    check(machine.press_record().is_ok(), "push-to-talk press starts the session");
    check(machine.state() == RecordingState::recording, "press enters recording");
    check(machine.has_active_session(), "the session owns the capture port");
    check(recorder.last_start_token_cancellable, "the capture port gets a session token");
    check(machine.press_record().is_ok() && machine.state() == RecordingState::recording,
          "a second press is ignored in push-to-talk mode");

    check(machine.release_record().is_ok(), "release stops the session");
    check(machine.state() == RecordingState::processing, "processing starts after the stop");
    check(worker.pending() == 1, "processing is queued on the worker, not run inline");
    check(transcriber.calls == 0, "transcription does not run before the worker executes it");

    check(machine.wait_idle(std::chrono::seconds(1)), "the session returns to idle");
    check(machine.state() == RecordingState::idle, "final state is idle");
    check(recorder.stops == 1, "the capture port was stopped exactly once");
    check(transcriber.calls == 1 && output.calls == 1, "one transcription and one delivery");
    check(events.texts.size() == 1 && events.texts.front() == "hello world",
          "the full final text is delivered once");
    check(!recorder.call_order.empty() && recorder.call_order.front() == "stop",
          "stop-before-process: the capture stops first");
    check(transcriber.order.size() == 1 && transcriber.order.front() == "transcribe",
          "transcription happens after the stop");
    check(!output.last_auto_paste && transcriber.captured_options.prompt == "terms",
          "the options provider is read per session");
    check(output.last_token_cancellable, "the output port gets the live session token");
    check(transcriber.captured_options.best_of == kFinalBestOf,
          "the final result asks for 3 greedy candidates");
    check(events.failures.empty(), "a successful session reports no failure");
    check(count_state(events.states, RecordingState::idle) == 1, "idle is published once");
    check(contains(events.states, RecordingState::recording), "recording is published");
    check(contains(events.states, RecordingState::processing), "processing is published");
}

void check_toggle_mode()
{
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    FakeOutput output;
    ManualWorker worker;
    RecordingStateMachine machine(recorder, transcriber, output, worker, options_for(RecordingMode::toggle));
    Recorder events;
    observe(machine, events);

    machine.press_record();
    check(machine.state() == RecordingState::recording, "toggle press starts recording");
    check(machine.release_record().is_ok() && machine.state() == RecordingState::recording,
          "release does not stop in toggle mode");
    machine.press_record();
    check(machine.state() == RecordingState::processing, "the second toggle press stops");
    check(machine.wait_idle(std::chrono::seconds(1)), "toggle session returns to idle");
    check(recorder.stops == 1 && transcriber.calls == 1, "toggle processes exactly once");

    // A release with no session, and a press from processing, are no-ops.
    check(machine.release_record().is_ok(), "release outside a session is a no-op");
    check(events.failures.empty(), "no-op transitions do not report failures");
}

void check_vad_mode_hook()
{
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    FakeOutput output;
    ManualWorker worker;
    RecordingStateMachine machine(recorder, transcriber, output, worker, options_for(RecordingMode::vad));
    Recorder events;
    observe(machine, events);

    int hook_chunks = 0;
    machine.set_vad_hook([&hook_chunks](const std::vector<float>& samples) {
        ++hook_chunks;
        return !samples.empty(); // deterministic first-chunk auto-stop
    });
    recorder.drain_chunk = ramp(4000, 0.1f);

    machine.press_record();
    check(machine.state() == RecordingState::recording, "vad mode starts on press");
    check(worker.pending() == 1, "vad mode posts a background auto-stop loop");

    // The loop polls until the hook says stop; drain returns the same chunk, so
    // the third poll stops the session.
    worker.run_next();
    check(machine.state() == RecordingState::processing, "the vad hook stops the session");
    check(hook_chunks == 1, "the hook stops the VAD session on its first drained chunk");
    check(machine.wait_idle(std::chrono::seconds(1)), "vad session returns to idle");
    check(transcriber.calls == 1 && events.texts.size() == 1, "the vad session is transcribed once");
    check(recorder.stops == 1, "the vad session stops the capture port once");
}

void check_vad_mode_segmenter_and_reset()
{
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    FakeOutput output;
    ManualWorker worker;
    FakeSegmenter segmenter;
    RecordingStateMachine machine(
        recorder, transcriber, output, worker, options_for(RecordingMode::vad), &segmenter);
    Recorder events;
    observe(machine, events);
    machine.set_vad_hook([](const std::vector<float>& samples) { return !samples.empty(); });
    recorder.drain_chunk = ramp(4000, 0.05f);

    machine.press_record();
    check(machine.state() == RecordingState::recording, "VAD session starts with a cached segmenter");
    worker.run_next();
    check(machine.state() == RecordingState::processing, "VAD hook stops the session");
    check(machine.wait_idle(std::chrono::seconds(1)), "VAD session returns to idle");
    check(segmenter.resets >= 1, "the cached segmenter is reset after the session");
    check(machine.cached_segmenter() == &segmenter, "the same cached segmenter survives the session");

    const int resets_after_first = segmenter.resets;
    machine.press_record();
    worker.run_next();
    check(machine.wait_idle(std::chrono::seconds(1)), "the second VAD session returns to idle");
    check(segmenter.resets > resets_after_first, "the second session resets the same instance");
    check(machine.cached_segmenter() == &segmenter, "the segmenter instance is reused");
}

void check_vad_mode_without_seam()
{
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    FakeOutput output;
    ManualWorker worker;
    RecordingStateMachine machine(recorder, transcriber, output, worker, options_for(RecordingMode::vad));
    Recorder events;
    observe(machine, events);
    recorder.drain_chunk = ramp(4000, 0.05f);

    machine.press_record();
    check(machine.state() == RecordingState::recording, "vad mode without a seam still records");
    // The loop task is queued but never executed by this test: with no hook and
    // no segmenter it is a no-op, so the session is ended by the hotkey.
    check(worker.pending() == 1, "the no-op loop seam is posted but not executed here");
    machine.cancel();
    check(machine.state() == RecordingState::idle, "a no-op seam session is ended by cancel");
    check(transcriber.calls == 0, "a cancelled capture is never transcribed");
}

void check_empty_capture()
{
    FakeRecorder recorder;
    recorder.samples.clear();
    FakeTranscriber transcriber;
    FakeOutput output;
    ManualWorker worker;
    RecordingStateMachine machine(
        recorder, transcriber, output, worker, options_for(RecordingMode::push_to_talk));
    Recorder events;
    observe(machine, events);

    machine.press_record();
    machine.release_record();
    check(machine.state() == RecordingState::idle, "an empty capture returns directly to idle");
    check(worker.pending() == 0, "an empty capture never enters processing");
    check(transcriber.calls == 0, "an empty capture is not transcribed");
    check(output.calls == 0, "an empty capture writes nothing");
    check(count_state(events.states, RecordingState::processing) == 0, "processing is never published");
    check(!machine.has_active_session(), "the session is released after an empty capture");
}

void check_no_lost_buffer_and_no_preview()
{
    FakeRecorder recorder;
    recorder.samples = ramp(40000, -0.25f); // long session with a tail sample
    FakeTranscriber transcriber;
    FakeOutput output;
    ManualWorker worker;
    RecordingStateMachine machine(
        recorder, transcriber, output, worker, options_for(RecordingMode::toggle));
    Recorder events;
    observe(machine, events);

    machine.press_record();
    machine.press_record();
    check(machine.wait_idle(std::chrono::seconds(1)), "session completes");
    check(transcriber.captured_samples == recorder.samples,
          "the recogniser receives the whole captured buffer, tail included");
    check(transcriber.captured_samples.size() == 40000, "no sample is truncated");
    check(output.calls == 1 && output.texts.size() == 1,
          "the final text is delivered exactly once, with no streaming preview");
    check(events.texts.size() == 1, "TextReady fires once for the full text");
}

void check_blank_and_declined_output()
{
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    transcriber.result = std::string("   \n\t ");
    FakeOutput output;
    ManualWorker worker;
    RecordingStateMachine machine(
        recorder, transcriber, output, worker, options_for(RecordingMode::toggle));
    Recorder events;
    observe(machine, events);
    machine.press_record();
    machine.press_record();
    check(machine.wait_idle(std::chrono::seconds(1)), "blank transcript session completes");
    check(output.calls == 0, "a whitespace-only transcript is never delivered");
    check(events.texts.empty(), "a blank transcript reports no text");

    // Deliver == false is a successful no-delivery: no TextReady, no failure.
    FakeRecorder second_recorder;
    FakeTranscriber second_transcriber;
    FakeOutput second_output;
    second_output.deliver = false;
    ManualWorker second_worker;
    RecordingStateMachine second(
        second_recorder, second_transcriber, second_output, second_worker, options_for(RecordingMode::toggle));
    Recorder second_events;
    observe(second, second_events);
    second.press_record();
    second.press_record();
    check(second.wait_idle(std::chrono::seconds(1)), "declined session completes");
    check(second_output.calls == 1, "the output port was called");
    check(second_events.texts.empty() && second_events.failures.empty(),
          "a declined delivery reports neither text nor failure");
}

void check_error_paths()
{
    // Microphone failure: no session, no stop, no transcription.
    FakeRecorder recorder;
    recorder.start_status = Status::failure(ErrorCode::permission_denied, "mic blocked");
    FakeTranscriber transcriber;
    FakeOutput output;
    ManualWorker worker;
    RecordingStateMachine machine(
        recorder, transcriber, output, worker, options_for(RecordingMode::toggle));
    Recorder events;
    observe(machine, events);

    check(machine.press_record().code() == ErrorCode::permission_denied, "the start failure is returned");
    check(machine.state() == RecordingState::idle, "a start failure leaves the machine idle");
    check(events.failures.size() == 1 && events.failures.front() == ErrorCode::permission_denied,
          "the start failure is reported once");
    check(!machine.has_active_session(), "a failed start owns no session");

    // Transcription failure: reported, machine returns to idle, nothing delivered.
    FakeRecorder transcribing_recorder;
    FakeTranscriber failing;
    failing.result = Result<std::string>::failure(ErrorCode::model_not_ready, "model missing");
    FakeOutput failing_output;
    ManualWorker failing_worker;
    RecordingStateMachine failing_machine(
        transcribing_recorder, failing, failing_output, failing_worker, options_for(RecordingMode::toggle));
    Recorder failing_events;
    observe(failing_machine, failing_events);
    failing_machine.press_record();
    failing_machine.press_record();
    check(failing_machine.wait_idle(std::chrono::seconds(1)), "a failed transcription returns to idle");
    check(failing_events.failures.size() == 1
              && failing_events.failures.front() == ErrorCode::model_not_ready,
          "the transcription failure is reported");
    check(failing_output.calls == 0, "a failed transcription delivers nothing");
    check(failing_machine.state() == RecordingState::idle, "a failed transcription ends in idle");

    // Output failure: reported, machine returns to idle.
    FakeRecorder output_recorder;
    FakeTranscriber output_transcriber;
    FakeOutput failing_output_port;
    failing_output_port.status = Status::failure(ErrorCode::permission_denied, "no clipboard");
    ManualWorker output_worker;
    RecordingStateMachine output_machine(
        output_recorder, output_transcriber, failing_output_port, output_worker, options_for(RecordingMode::toggle));
    Recorder output_events;
    observe(output_machine, output_events);
    output_machine.press_record();
    output_machine.press_record();
    check(output_machine.wait_idle(std::chrono::seconds(1)), "a failed delivery returns to idle");
    check(output_events.failures.size() == 1, "the output failure is reported");
    check(output_events.texts.empty(), "a failed delivery reports no text");

    // Stop failure: reported, nothing is transcribed.
    FakeRecorder stop_recorder;
    stop_recorder.stop_status = Status::failure(ErrorCode::device_disconnected, "device lost");
    FakeTranscriber stop_transcriber;
    FakeOutput stop_output;
    ManualWorker stop_worker;
    RecordingStateMachine stop_machine(
        stop_recorder, stop_transcriber, stop_output, stop_worker, options_for(RecordingMode::push_to_talk));
    Recorder stop_events;
    observe(stop_machine, stop_events);
    stop_machine.press_record();
    stop_machine.release_record();
    check(stop_machine.state() == RecordingState::idle, "a stop failure ends in idle");
    check(stop_transcriber.calls == 0, "a stop failure never transcribes");
    check(stop_events.failures.size() == 1, "the stop failure is reported");
}

void check_cancel_during_processing()
{
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    transcriber.block = true;
    FakeOutput output;
    ManualWorker worker;
    RecordingStateMachine machine(
        recorder, transcriber, output, worker, options_for(RecordingMode::push_to_talk));
    Recorder events;
    observe(machine, events);

    machine.press_record();
    machine.release_record();
    check(machine.state() == RecordingState::processing, "the session is processing");
    check(machine.has_active_session(), "processing still owns the session");

    machine.cancel();
    check(machine.state() == RecordingState::idle, "cancel returns the machine to idle immediately");
    check(recorder.cancels == 1, "cancel reaches the capture port");
    check(count_state(events.states, RecordingState::idle) == 1, "cancel publishes one idle transition");

    // Idempotence: a second cancel with no session touches no port and no event.
    machine.cancel();
    check(recorder.cancels == 1, "a repeated cancel does not re-cancel the port");
    check(count_state(events.states, RecordingState::idle) == 1, "a repeated cancel publishes no event");

    // The in-flight step finishes now and must not resurrect anything.
    worker.run_next();
    check(machine.state() == RecordingState::idle, "the cancelled step does not change the state");
    check(output.calls == 0, "a cancelled transcription never reaches the output port");
    check(events.texts.empty(), "a cancelled transcription delivers no text");
    check(events.failures.empty(), "cancellation is not reported as a failure");

    // A new session after the cancel is unaffected by the stale one.
    transcriber.block = false;
    check(machine.press_record().is_ok(), "a new session can start after a cancel");
    check(machine.release_record().is_ok(), "the new session can be stopped");
    check(machine.wait_idle(std::chrono::seconds(1)), "the new session completes");
    check(transcriber.calls == 1 && events.texts.size() == 1,
          "the new session transcribes and delivers exactly once");
    transcriber.release();
}

void check_cancel_while_recording()
{
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    FakeOutput output;
    ManualWorker worker;
    RecordingStateMachine machine(
        recorder, transcriber, output, worker, options_for(RecordingMode::push_to_talk));
    Recorder events;
    observe(machine, events);

    machine.cancel();
    check(recorder.cancels == 0, "cancel with no session calls no port");
    check(events.states.empty(), "cancel with no session publishes nothing");

    machine.press_record();
    check(machine.state() == RecordingState::recording, "the session records");
    machine.cancel();
    check(machine.state() == RecordingState::idle, "cancel ends the recording session");
    check(recorder.cancels == 1, "cancel reaches the capture port once");
    check(recorder.stops == 0, "a cancelled recording is dropped without a stop");
    check(transcriber.calls == 0, "a cancelled recording is never transcribed");
    check(machine.release_record().is_ok(), "a release after cancel is a no-op");
    check(machine.state() == RecordingState::idle, "a release after cancel changes nothing");
}

void check_dispose_keeps_segmenter()
{
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    FakeOutput output;
    ManualWorker worker;
    FakeSegmenter segmenter;
    RecordingStateMachine machine(
        recorder, transcriber, output, worker, options_for(RecordingMode::push_to_talk), &segmenter);
    Recorder events;
    observe(machine, events);

    machine.press_record();
    check(machine.state() == RecordingState::recording, "a session is recording before dispose");
    machine.dispose();
    check(machine.is_disposed(), "the machine is disposed");
    check(machine.state() == RecordingState::idle, "dispose returns the machine to idle");
    check(recorder.cancels == 1, "dispose cancels the capture port");
    check(segmenter.resets == 0, "dispose does not reset the cached segmenter");
    check(machine.cached_segmenter() == &segmenter, "dispose keeps the cached segmenter instance");

    machine.dispose();
    check(recorder.cancels == 1, "dispose is idempotent");

    check(machine.press_record().code() == ErrorCode::invalid_state, "a disposed machine refuses to start");
    check(recorder.starts == 1, "a disposed machine calls no port");
    check(machine.release_record().is_ok(), "a disposed machine ignores release");
    check(transcriber.calls == 0, "a disposed machine transcribes nothing");
}

void check_reset_session_keeps_segmenter()
{
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    FakeOutput output;
    ManualWorker worker;
    FakeSegmenter segmenter;
    RecordingStateMachine machine(
        recorder, transcriber, output, worker, options_for(RecordingMode::toggle), &segmenter);
    Recorder events;
    observe(machine, events);

    machine.press_record();
    machine.reset_session();
    check(machine.state() == RecordingState::idle, "reset_session ends the session");
    check(recorder.cancels == 1, "reset_session cancels the capture port");
    check(segmenter.resets == 1, "reset_session resets the cached segmenter state");
    check(machine.cached_segmenter() == &segmenter, "reset_session keeps the same segmenter instance");
    check(transcriber.calls == 0, "reset_session transcribes nothing");

    // The segmenter is still usable for the next session.
    machine.press_record();
    machine.press_record();
    check(machine.wait_idle(std::chrono::seconds(1)), "the next session completes");
    check(transcriber.calls == 1, "the next session transcribes once");
    check(machine.cached_segmenter() == &segmenter, "the segmenter survived every session");
}

void check_thread_worker_end_to_end()
{
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    FakeOutput output;
    ThreadRecordingWorker worker;
    RecordingStateMachine machine(
        recorder, transcriber, output, worker, options_for(RecordingMode::push_to_talk));
    Recorder events;
    observe(machine, events);

    machine.press_record();
    check(machine.state() == RecordingState::recording, "the real worker starts the session");
    machine.release_record();
    check(machine.wait_idle(std::chrono::seconds(5)), "the real worker finishes the session");
    check(machine.state() == RecordingState::idle, "the real worker returns the machine to idle");
    check(events.texts.size() == 1 && events.texts.front() == "hello world",
          "the real worker delivers the full final text");
    check(transcriber.captured_samples == recorder.samples, "the real worker loses no audio");
}

void check_thread_worker_cancel_race()
{
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    transcriber.block = true;
    FakeOutput output;
    ThreadRecordingWorker worker;
    RecordingStateMachine machine(
        recorder, transcriber, output, worker, options_for(RecordingMode::push_to_talk));
    Recorder events;
    observe(machine, events);

    machine.press_record();
    machine.release_record();
    // cancel() must not wait for the blocked worker: it only requests
    // cancellation and returns to idle.
    const auto before = std::chrono::steady_clock::now();
    machine.cancel();
    const auto elapsed = std::chrono::steady_clock::now() - before;
    check(elapsed < std::chrono::seconds(1), "cancel does not block on the worker");
    check(machine.state() == RecordingState::idle, "cancel is immediate even while processing");
    transcriber.release();
    check(machine.wait_idle(std::chrono::seconds(5)), "the cancelled step retires");
    check(output.calls == 0, "the cancelled step delivers nothing");
    check(events.texts.empty(), "the cancelled step reports no text");
}

} // namespace

int main()
{
    check_push_to_talk();
    check_toggle_mode();
    check_vad_mode_hook();
    check_vad_mode_segmenter_and_reset();
    check_vad_mode_without_seam();
    check_empty_capture();
    check_no_lost_buffer_and_no_preview();
    check_blank_and_declined_output();
    check_error_paths();
    check_cancel_during_processing();
    check_cancel_while_recording();
    check_dispose_keeps_segmenter();
    check_reset_session_keeps_segmenter();
    check_thread_worker_end_to_end();
    check_thread_worker_cancel_race();

    if (failures != 0) {
        std::cerr << "recording-state-machine-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "recording-state-machine-contract: OK\n";
    return 0;
}
