#include "domain/recording_state_machine.hpp"

#include <exception>
#include <utility>

namespace voicetyper::domain {

namespace {

/// Runs a presentation callback without letting it take down recording: no
/// exception may cross a module boundary.
template <typename Handler, typename Arg>
void invoke_handler(const Handler& handler, Arg&& arg) noexcept
{
    if (!handler) {
        return;
    }
    try {
        handler(std::forward<Arg>(arg));
    } catch (...) {
        // A presentation callback cannot break the state machine.
    }
}

} // namespace

bool is_blank_text(std::string_view text) noexcept
{
    for (const char ch : text) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\v' && ch != '\f') {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// ThreadRecordingWorker
// ---------------------------------------------------------------------------

ThreadRecordingWorker::ThreadRecordingWorker()
{
    thread_ = std::make_unique<std::thread>(&ThreadRecordingWorker::run, this);
}

ThreadRecordingWorker::~ThreadRecordingWorker()
{
    shutdown();
}

Status ThreadRecordingWorker::post(Task task)
{
    if (!task) {
        return Status::failure(ErrorCode::invalid_argument, "empty recording worker task");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return Status::failure(ErrorCode::invalid_state, "recording worker is shut down");
        }
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
    return Status::success();
}

void ThreadRecordingWorker::join()
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return stopping_ || (queue_.empty() && !busy_); });
}

void ThreadRecordingWorker::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_ != nullptr && thread_->joinable()) {
        thread_->join();
    }
}

void ThreadRecordingWorker::run()
{
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                // Queued work is abandoned; a shut-down worker never claims to
                // have run a task it dropped.
                queue_.clear();
                busy_ = false;
                cv_.notify_all();
                return;
            }
            task = std::move(queue_.front());
            queue_.erase(queue_.begin());
            busy_ = true;
        }

        try {
            task();
        } catch (...) {
            // Contained: no exception crosses the worker boundary.
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
        }
        cv_.notify_all();
    }
}

// ---------------------------------------------------------------------------
// RecordingStateMachine: handlers and event queueing
// ---------------------------------------------------------------------------

void RecordingStateMachine::set_state_changed_handler(StateChangedHandler handler)
{
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    state_changed_ = std::move(handler);
}

void RecordingStateMachine::set_text_ready_handler(TextReadyHandler handler)
{
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    text_ready_ = std::move(handler);
}

void RecordingStateMachine::set_failed_handler(FailedHandler handler)
{
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    failed_ = std::move(handler);
}

void RecordingStateMachine::set_live_settings_provider(std::function<LiveSettings()> provider)
{
    std::lock_guard<std::mutex> lock(mutex_);
    live_settings_provider_ = std::move(provider);
}

RecordingMode RecordingStateMachine::mode_locked() const
{
    // The provider runs while the lock is held, which is why every caller inside
    // a locked scope uses this. A provider that calls back into the machine would
    // deadlock, so the composition's provider only reads the settings snapshot.
    if (live_settings_provider_) {
        return live_settings_provider_().mode;
    }
    return options_.mode;
}

RecordingMode RecordingStateMachine::mode() const noexcept
{
    const auto provider = [this] {
        std::lock_guard<std::mutex> lock(mutex_);
        return live_settings_provider_;
    }();
    if (provider) {
        return provider().mode;
    }
    return options_.mode;
}

double RecordingStateMachine::live_silence_threshold_seconds_locked() const
{
    if (live_settings_provider_) {
        const auto seconds = live_settings_provider_().vad_silence_threshold_seconds;
        if (seconds > 0.0) {
            return seconds;
        }
    }
    return options_.vad_silence_threshold_seconds;
}

double RecordingStateMachine::live_silence_threshold_seconds() const
{
    const auto provider = [this] {
        std::lock_guard<std::mutex> lock(mutex_);
        return live_settings_provider_;
    }();
    if (provider) {
        const auto seconds = provider().vad_silence_threshold_seconds;
        if (seconds > 0.0) {
            return seconds;
        }
    }
    return options_.vad_silence_threshold_seconds;
}

void RecordingStateMachine::set_options_provider(std::function<SessionOptions()> provider)
{
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    options_provider_ = std::move(provider);
}

void RecordingStateMachine::set_vad_hook(VadAutoStopHook hook)
{
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    vad_hook_ = std::move(hook);
}

void RecordingStateMachine::queue_state_change_locked(RecordingState next)
{
    if (state_ == next) {
        return;
    }
    state_ = next;
    StateChangedHandler handler;
    {
        std::lock_guard<std::mutex> guard(handlers_mutex_);
        handler = state_changed_;
    }
    pending_callbacks_.push_back([handler, next] { invoke_handler(handler, next); });
}

void RecordingStateMachine::queue_text_ready_locked(std::string text)
{
    TextReadyHandler handler;
    {
        std::lock_guard<std::mutex> guard(handlers_mutex_);
        handler = text_ready_;
    }
    pending_callbacks_.push_back([handler, text = std::move(text)] { invoke_handler(handler, text); });
}

void RecordingStateMachine::queue_failure_locked(Error error)
{
    FailedHandler handler;
    {
        std::lock_guard<std::mutex> guard(handlers_mutex_);
        handler = failed_;
    }
    pending_callbacks_.emplace_back([handler, error] { invoke_handler(handler, error); });
}

void RecordingStateMachine::queue_segmenter_reset_locked()
{
    segmenter_reset_pending_ = segmenter_ != nullptr;
}

std::vector<RecordingStateMachine::Callback> RecordingStateMachine::take_callbacks_locked()
{
    std::vector<Callback> callbacks;
    callbacks.swap(pending_callbacks_);
    return callbacks;
}

void RecordingStateMachine::invoke_all(std::vector<Callback>& callbacks)
{
    for (Callback& callback : callbacks) {
        if (!callback) {
            continue;
        }
        try {
            callback();
        } catch (...) {
            // Handlers run without the internal lock, so a throwing handler
            // cannot leave the machine locked.
        }
    }
}

// ---------------------------------------------------------------------------
// RecordingStateMachine: observation
// ---------------------------------------------------------------------------

RecordingState RecordingStateMachine::state() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool RecordingStateMachine::is_recording() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == RecordingState::recording;
}

bool RecordingStateMachine::is_processing() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == RecordingState::processing;
}

bool RecordingStateMachine::has_active_session() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return session_active_;
}

bool RecordingStateMachine::is_disposed() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return disposed_;
}

SpeechSegmenter* RecordingStateMachine::cached_segmenter() const noexcept
{
    return segmenter_;
}

// ---------------------------------------------------------------------------
// RecordingStateMachine: sessions
// ---------------------------------------------------------------------------

RecordingStateMachine::RecordingStateMachine(
    RecordingPort& recorder,
    TranscriptionPort& transcription,
    TextOutputPort& output,
    RecordingWorker& worker,
    RecordingStateMachineOptions options,
    SpeechSegmenter* segmenter)
    : recorder_(recorder)
    , transcription_(transcription)
    , output_(output)
    , worker_(worker)
    , options_(options)
    , segmenter_(segmenter)
{
}

RecordingStateMachine::~RecordingStateMachine()
{
    dispose();
}

std::uint64_t RecordingStateMachine::begin_session_locked()
{
    ++session_epoch_;
    session_cancel_ = CancellationSource();
    stop_requested_ = false;
    session_active_ = true;
    return session_epoch_;
}

bool RecordingStateMachine::token_cancelled_locked(std::uint64_t epoch) const noexcept
{
    return epoch == session_epoch_ && session_cancel_.is_cancellation_requested();
}

Status RecordingStateMachine::press_record()
{
    enum class Action { none, start, stop };

    Action action = Action::none;
    std::uint64_t epoch = 0;
    std::vector<Callback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (disposed_) {
            return Status::failure(ErrorCode::invalid_state, "recording state machine is disposed");
        }
        if (state_ == RecordingState::idle && !stop_requested_) {
            action = Action::start;
            epoch = begin_session_locked();
        } else if (state_ == RecordingState::recording && mode_locked() == RecordingMode::toggle
                   && !stop_requested_) {
            action = Action::stop;
            epoch = session_epoch_;
        }
        callbacks = take_callbacks_locked();
    }
    invoke_all(callbacks);

    if (action == Action::start) {
        return start_session(epoch);
    }
    if (action == Action::stop) {
        return finish_capture(epoch);
    }
    return Status::success();
}

Status RecordingStateMachine::start_session(std::uint64_t epoch)
{
    CancellationToken token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (disposed_ || epoch != session_epoch_) {
            return Status::failure(ErrorCode::cancelled, "session superseded before start");
        }
        token = session_cancel_.token();
    }

    // The capture port is called with no lock held.
    const Status started = recorder_.start(token);

    std::vector<Callback> callbacks;
    bool drop_capture = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started.is_error()) {
            session_active_ = false;
            queue_failure_locked(started.error());
        } else if (disposed_ || epoch != session_epoch_ || token.is_cancellation_requested()) {
            // Cancelled or disposed while the device was opening: whatever the
            // backend may have started must not survive, and nothing is
            // transcribed.
            session_active_ = false;
            drop_capture = true;
            queue_failure_locked(Error(
                ErrorCode::cancelled,
                "recording session cancelled while starting",
                "capture dropped without transcription"));
        } else {
            queue_state_change_locked(RecordingState::recording);
        }
        callbacks = take_callbacks_locked();
        cv_.notify_all();
    }
    invoke_all(callbacks);

    if (drop_capture) {
        run_port_cancel();
        return Status::failure(ErrorCode::cancelled, "recording session cancelled while starting");
    }
    if (started.is_error()) {
        return started;
    }

    if (mode() == RecordingMode::vad) {
        // The auto-stop loop is an optional seam: with neither hook nor cached
        // segmenter it polls and never stops anything, and the session then ends
        // through press/release/cancel.
        const Status posted = worker_.post([this, epoch] { run_vad_loop(epoch); });
        if (posted.is_error()) {
            std::vector<Callback> failure_callbacks;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_failure_locked(posted.error());
                failure_callbacks = take_callbacks_locked();
            }
            invoke_all(failure_callbacks);
        }
    }
    return Status::success();
}

Status RecordingStateMachine::release_record()
{
    std::uint64_t epoch = 0;
    bool stop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop = !disposed_ && state_ == RecordingState::recording
               && mode_locked() == RecordingMode::push_to_talk && !stop_requested_;
        if (stop) {
            epoch = session_epoch_;
        }
    }
    if (!stop) {
        return Status::success();
    }
    return finish_capture(epoch);
}

Status RecordingStateMachine::finish_capture(std::uint64_t epoch)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (disposed_ || state_ != RecordingState::recording || epoch != session_epoch_
            || stop_requested_) {
            return Status::failure(ErrorCode::invalid_state, "no recording session to stop");
        }
        // Claim the stop so a concurrent press/release cannot stop twice.
        stop_requested_ = true;
    }

    // stop-before-process: capture is fully stopped before any processing runs,
    // and the buffer is moved into the processing step, so the audio tail can
    // neither be lost nor truncated between capture and recognition.
    Result<SampleBuffer> captured = recorder_.stop();

    std::vector<Callback> callbacks;
    bool reset_segmenter = false;
    bool process = false;
    SampleBuffer audio;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = false;
        const bool current = !disposed_ && epoch == session_epoch_;
        if (captured.is_error()) {
            if (current) {
                session_active_ = false;
                queue_segmenter_reset_locked();
                queue_failure_locked(captured.error());
                queue_state_change_locked(RecordingState::idle);
            }
        } else if (current && token_cancelled_locked(epoch)) {
            // Cancelled while the device was stopping: the audio is dropped.
            session_active_ = false;
            queue_segmenter_reset_locked();
            queue_state_change_locked(RecordingState::idle);
        } else if (current && captured.value().empty()) {
            // Empty capture: there is nothing to recognise, so back to idle
            // without entering processing.
            session_active_ = false;
            queue_segmenter_reset_locked();
            queue_state_change_locked(RecordingState::idle);
        } else if (current) {
            audio = std::move(captured).value();
            processing_in_flight_ = true;
            queue_state_change_locked(RecordingState::processing);
            process = true;
        }
        reset_segmenter = segmenter_reset_pending_;
        segmenter_reset_pending_ = false;
        callbacks = take_callbacks_locked();
        cv_.notify_all();
    }
    invoke_all(callbacks);
    if (reset_segmenter && segmenter_ != nullptr) {
        segmenter_->reset();
    }
    if (!process) {
        return captured.is_error() ? captured.status() : Status::success();
    }

    const Status posted = worker_.post([this, epoch, buffer = std::move(audio)]() mutable {
        run_processing(epoch, std::move(buffer));
    });
    if (posted.is_error()) {
        std::vector<Callback> failure_callbacks;
        bool reset_after_failure = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (epoch == session_epoch_ && !disposed_) {
                processing_in_flight_ = false;
                session_active_ = false;
                queue_segmenter_reset_locked();
                queue_failure_locked(posted.error());
                queue_state_change_locked(RecordingState::idle);
            }
            reset_after_failure = segmenter_reset_pending_;
            segmenter_reset_pending_ = false;
            failure_callbacks = take_callbacks_locked();
            cv_.notify_all();
        }
        invoke_all(failure_callbacks);
        if (reset_after_failure && segmenter_ != nullptr) {
            segmenter_->reset();
        }
        return posted;
    }
    return Status::success();
}

void RecordingStateMachine::run_processing(std::uint64_t epoch, SampleBuffer audio)
{
    CancellationToken token;
    SessionOptions options;
    bool superseded = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        superseded = disposed_ || epoch != session_epoch_;
        if (!superseded) {
            token = session_cancel_.token();
        } else {
            // Cancelled before this step started: no port is touched, and the
            // session reset that was queued by the cancelling thread is not
            // duplicated here.
            segmenter_reset_pending_ = false;
        }
    }
    if (superseded) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(handlers_mutex_);
        if (options_provider_) {
            options = options_provider_();
        }
    }

    Error error;
    bool report = false;
    std::string ready;
    if (token.is_cancellation_requested()) {
        error = Error(ErrorCode::cancelled, "transcription cancelled");
    } else {
        const Result<std::string> transcribed = transcription_.transcribe(audio, options, token);
        if (transcribed.is_error()) {
            error = transcribed.error();
            report = error.code() != ErrorCode::cancelled;
        } else if (token.is_cancellation_requested()) {
            // Cancel landed between transcription and delivery: the late result
            // is dropped and nothing is written.
            error = Error(ErrorCode::cancelled, "transcription cancelled before output");
        } else if (!is_blank_text(transcribed.value())) {
            const Result<bool> delivered = output_.output(transcribed.value(), options.auto_paste, token);
            if (delivered.is_error()) {
                error = delivered.error();
                report = error.code() != ErrorCode::cancelled;
            } else if (delivered.value()) {
                ready = std::move(transcribed).value();
            }
            // `false` is a successful no-delivery (paste declined): no TextReady.
        }
        // A blank transcript is a successful no-op: the output port is not called.
    }

    finish_processing(epoch, error, report, std::move(ready));
}

void RecordingStateMachine::finish_processing(
    std::uint64_t epoch,
    Error error,
    bool report,
    std::string text)
{
    std::vector<Callback> callbacks;
    bool reset_segmenter = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (epoch == session_epoch_ && !disposed_) {
            processing_in_flight_ = false;
            session_active_ = false;
            queue_segmenter_reset_locked();
            if (report) {
                queue_failure_locked(error);
            }
            if (!text.empty()) {
                queue_text_ready_locked(std::move(text));
            }
            queue_state_change_locked(RecordingState::idle);
        } else {
            // Cancelled or disposed while the step ran: a stale step may not
            // resurrect a state or deliver text.
            processing_in_flight_ = false;
        }
        reset_segmenter = segmenter_reset_pending_;
        segmenter_reset_pending_ = false;
        callbacks = take_callbacks_locked();
        cv_.notify_all();
    }
    invoke_all(callbacks);
    if (reset_segmenter && segmenter_ != nullptr) {
        segmenter_->reset();
    }
}

// ---------------------------------------------------------------------------
// RecordingStateMachine: VAD auto-stop loop
// ---------------------------------------------------------------------------

void RecordingStateMachine::run_vad_loop(std::uint64_t epoch)
{
    VadAutoStopHook hook;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        hook = vad_hook_;
    }
    // The detector is built per session over the *cached* segmenter: it borrows
    // the instance and never destroys it, so the VAD model is loaded once for
    // the whole application lifetime.
    std::unique_ptr<SilenceAutoStopDetector> detector;
    if (!hook && segmenter_ != nullptr) {
        detector = std::make_unique<SilenceAutoStopDetector>(
            *segmenter_, live_silence_threshold_seconds());
    }

    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const bool finished = cv_.wait_for(
                lock,
                options_.vad_poll_interval,
                [this, epoch] {
                    return disposed_ || epoch != session_epoch_
                           || state_ != RecordingState::recording;
                });
            if (finished) {
                return; // session stopped, cancelled or disposed
            }
        }

        const Result<SampleBuffer> chunk = recorder_.drain();
        if (chunk.is_error()) {
            std::vector<Callback> callbacks;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!disposed_ && epoch == session_epoch_) {
                    queue_failure_locked(chunk.error());
                }
                callbacks = take_callbacks_locked();
            }
            invoke_all(callbacks);
            continue;
        }
        if (chunk.value().empty()) {
            continue; // an empty drain is not an error
        }

        bool stop_session = false;
        if (hook) {
            stop_session = hook(chunk.value().samples());
        } else if (detector != nullptr) {
            stop_session = detector->process(chunk.value().samples()).stop;
        }
        if (!stop_session) {
            continue;
        }
        static_cast<void>(finish_capture(epoch));
        return; // exactly one auto-stop per session
    }
}

// ---------------------------------------------------------------------------
// RecordingStateMachine: cancellation and teardown
// ---------------------------------------------------------------------------

bool RecordingStateMachine::cancel_active_session_locked(std::vector<Callback>& callbacks)
{
    if (!session_active_ && state_ == RecordingState::idle) {
        return false; // nothing to cancel: no port call and no event
    }
    session_cancel_.request_cancellation();
    stop_requested_ = false;
    processing_in_flight_ = false;
    session_active_ = false;
    queue_segmenter_reset_locked();
    queue_state_change_locked(RecordingState::idle);
    callbacks = take_callbacks_locked();
    cv_.notify_all();
    return true;
}

void RecordingStateMachine::run_port_cancel() const noexcept
{
    try {
        static_cast<void>(recorder_.cancel());
    } catch (...) {
        // Contained: a failing port cannot break cancellation.
    }
}

void RecordingStateMachine::cancel()
{
    std::vector<Callback> callbacks;
    bool cancelled = false;
    bool reset_segmenter = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled = cancel_active_session_locked(callbacks);
        reset_segmenter = segmenter_reset_pending_;
        segmenter_reset_pending_ = false;
    }
    if (cancelled) {
        run_port_cancel();
    }
    if (reset_segmenter && segmenter_ != nullptr) {
        segmenter_->reset();
    }
    invoke_all(callbacks);
}

void RecordingStateMachine::reset_session()
{
    std::vector<Callback> callbacks;
    bool cancelled = false;
    bool reset_segmenter = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled = cancel_active_session_locked(callbacks);
        // The cached segmenter keeps its identity; only its per-session VAD
        // state is cleared, so the next session never reloads the model.
        queue_segmenter_reset_locked();
        reset_segmenter = segmenter_reset_pending_;
        segmenter_reset_pending_ = false;
    }
    if (cancelled) {
        run_port_cancel();
    }
    if (reset_segmenter && segmenter_ != nullptr) {
        segmenter_->reset();
    }
    invoke_all(callbacks);
}

void RecordingStateMachine::dispose()
{
    std::vector<Callback> callbacks;
    bool cancelled = false;
    bool reset_segmenter = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (disposed_) {
            return; // idempotent
        }
        disposed_ = true;
        cancelled = cancel_active_session_locked(callbacks);
        // Deliberately no segmenter reset: disposal leaves the cached segmenter
        // exactly as it is, because the composition root owns and reuses it.
        // Discard any pending per-session reset as well.
        reset_segmenter = false;
        segmenter_reset_pending_ = false;
        cv_.notify_all();
    }
    if (cancelled) {
        run_port_cancel();
    }
    if (reset_segmenter && segmenter_ != nullptr) {
        segmenter_->reset();
    }
    invoke_all(callbacks);
}

bool RecordingStateMachine::wait_idle(std::chrono::milliseconds timeout)
{
    // With a manual worker this runs the queued tasks deterministically; with
    // the thread worker it waits for them to finish.
    worker_.join();

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    while (processing_in_flight_) {
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            return false;
        }
    }
    return true;
}

} // namespace voicetyper::domain
