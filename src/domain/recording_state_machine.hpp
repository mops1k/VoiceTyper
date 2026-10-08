#pragma once

// Deterministic recording state machine: Idle -> Recording -> Processing -> Idle.
//
// Evidence: docs/migration/cpp/feature-parity.md (recording modes, final-only
// dictation), docs/migration/cpp/compatibility-contracts.md, and the .NET
// reference VoiceTyper.Core/Services/RecordingStateMachine.cs with its tests
// VoiceTyper.Tests/Services/RecordingStateMachineTests.cs.
//
// Frozen behavior implemented here (standard C++20, no Qt, no OS headers):
//   * Three observable states and one cycle:
//     idle -> recording -> processing -> idle. "processing" is entered only when
//     the capture produced audio, so an empty capture returns to idle directly.
//   * Three modes on one machine (domain::RecordingMode): push_to_talk (release
//     stops), toggle (second press stops), vad (a background auto-stop loop
//     stops; the loop is a callback seam and may be absent).
//   * Final-only dictation: exactly one transcription of the whole captured
//     buffer and at most one text delivery per session. There is no streaming
//     preview hook in this interface, by product decision (the .NET experiment
//     produced boundary artifacts and no latency win).
//   * stop-before-process: the capture port is fully stopped before the
//     transcriber is called, and the buffer is *moved* into the processing step,
//     so no audio tail can be lost between capture and recognition.
//   * Cancellation is idempotent, cooperative and epoch-scoped: a step that was
//     already in flight when cancel() arrived may not deliver text, may not
//     resurrect a state and cannot affect a later session.
//   * No port call, no state publication and no handler invocation ever happens
//     while the internal lock is held. Handlers run after the lock is released,
//     in the order the events were queued.
//   * dispose()/reset_session() never destroy the cached SpeechSegmenter: the
//     composition root owns it and reuses it between sessions so a session never
//     pays to load the VAD model. The machine only asks it to reset().
//
// Port/adapter split: the ports below live in the portable domain layer and are
// implemented later by adapters over src/platform/api/{audio_capture,
// transcriber}.hpp and the clipboard/paste contract. This file names no platform
// type and makes no capture/ASR claim: it is pure control flow over abstract
// ports, exercised by tests/contract/recording_state_machine_contract.cpp.

#include "domain/audio_wav.hpp"
#include "domain/text_output.hpp"
#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/settings.hpp"
#include "domain/speech_segments.hpp"
#include "domain/vad.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace voicetyper::domain {

/// Observable state of a dictation session. Parity with the .NET
/// `RecordingState` enum; the values appear only in diagnostics.
enum class RecordingState : std::uint8_t {
    idle = 0,
    recording = 1,
    processing = 2,
};

[[nodiscard]] constexpr std::string_view recording_state_name(RecordingState state) noexcept
{
    switch (state) {
    case RecordingState::idle: return "idle";
    case RecordingState::recording: return "recording";
    case RecordingState::processing: return "processing";
    }
    return "unknown";
}

/// The .NET machine always asks for 3 greedy candidates for the final result.
inline constexpr int kFinalBestOf = 3;

/// The persisted setting starts at the machine's own value, so the first dictation
/// after an update behaves exactly as before. settings.hpp cannot include this
/// header (it includes settings.hpp), so the two defaults are repeated and pinned
/// here instead of drifting apart silently.
static_assert(kFinalBestOf == kDefaultBestOf,
    "AppSettings::best_of must default to the recording machine's candidate count");

/// Per-session recognition/output parameters, read on every session so a settings
/// change applies without recreating the machine (the .NET optionsProvider).
struct SessionOptions {
    RecognitionLanguage language = RecognitionLanguage::ru;
    std::string prompt;
    double temperature = 0.0;
    bool condition_on_previous_text = false;
    /// Passed through to the text output port (clipboard first, paste optional).
    bool auto_paste = true;
    int best_of = kFinalBestOf;
    /// Background noise suppression, ported from the .NET build. Off by default, exactly
    /// like the setting it comes from.
    bool noise_suppression = false;
    /// What a detector already found for THIS audio, when a caller ran one (the
    /// trimming decorator does). Empty means "the engine detects for itself".
    SpeechMap speech_map;
};

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

/// Capture port: device I/O only, matching the division of responsibility in
/// src/platform/api/audio_capture.hpp. It hands back normalized 16 kHz mono float
/// samples; it never transcribes and the machine never resamples.
class RecordingPort {
public:
    virtual ~RecordingPort() = default;

    RecordingPort(const RecordingPort&) = delete;
    RecordingPort& operator=(const RecordingPort&) = delete;
    RecordingPort(RecordingPort&&) = delete;
    RecordingPort& operator=(RecordingPort&&) = delete;

    /// Opens the device and begins a session. Failure codes: not_found,
    /// permission_denied, unavailable, device_disconnected, invalid_state.
    virtual Status start(const CancellationToken& cancellation) = 0;

    /// Stops the session and returns everything captured since it started. An ok
    /// result with an empty buffer means "nothing recorded" and must never reach
    /// the transcriber. Failure codes: device_disconnected, io_failure,
    /// invalid_state.
    [[nodiscard]] virtual Result<SampleBuffer> stop() = 0;

    /// Stops the session and discards the audio. Idempotent by contract.
    virtual Status cancel() = 0;

    /// Returns the audio captured since the previous drain without stopping.
    /// This is the VAD-mode primitive; an empty result is not an error.
    [[nodiscard]] virtual Result<SampleBuffer> drain() = 0;

protected:
    RecordingPort() = default;
};

/// Transcription port: one full final transcript per call.
///
/// An empty string is a *successful* result, and a whitespace-only transcript
/// means "nothing to deliver" (the machine then does not call the output port).
/// There is deliberately no partial/streaming entry point.
class TranscriptionPort {
public:
    virtual ~TranscriptionPort() = default;

    TranscriptionPort(const TranscriptionPort&) = delete;
    TranscriptionPort& operator=(const TranscriptionPort&) = delete;
    TranscriptionPort(TranscriptionPort&&) = delete;
    TranscriptionPort& operator=(TranscriptionPort&&) = delete;

    /// Transcribes the whole session audio. Blocking, called from a worker
    /// thread and never from the thread that pressed the hotkey. It must observe
    /// `cancellation` and return ErrorCode::cancelled when it fires. Failure
    /// codes: engine_unavailable, model_not_ready, cancelled, io_failure.
    [[nodiscard]] virtual Result<std::string> transcribe(
        const SampleBuffer& audio,
        const SessionOptions& options,
        const CancellationToken& cancellation) = 0;

protected:
    TranscriptionPort() = default;
};

/// Text delivery port (clipboard plus optional auto-paste).
// The TextOutputPort interface now lives in domain/text_output.hpp next to its
// implementation, so the machine and the service share one interface instead of
// two declarations that can drift.

/// Background execution port. The machine owns no thread of its own, so tests
/// inject a deterministic manual executor while production injects a real one.
class RecordingWorker {
public:
    using Task = std::function<void()>;

    virtual ~RecordingWorker() = default;

    RecordingWorker(const RecordingWorker&) = delete;
    RecordingWorker& operator=(const RecordingWorker&) = delete;
    RecordingWorker(RecordingWorker&&) = delete;
    RecordingWorker& operator=(RecordingWorker&&) = delete;

    /// Queues `task` for execution off the calling thread; it must not run the
    /// task inline, because the caller may still hold a lock or be a UI thread.
    /// An empty task is rejected with invalid_argument and never run.
    virtual Status post(Task task) = 0;

    /// Blocks until every task posted so far has finished. Idempotent, and a
    /// later post() starts a new batch. Must not deadlock when a task calls it.
    virtual void join() = 0;

protected:
    RecordingWorker() = default;
};

/// Default production worker: one long-lived FIFO thread driven by a mutex and a
/// condition_variable. Only the machine's own tasks are posted here (at most one
/// processing step and one VAD loop), so a single thread is enough and it makes
/// the stop-before-process ordering observable. A task that throws is contained:
/// no exception crosses the port boundary.
class ThreadRecordingWorker final : public RecordingWorker {
public:
    ThreadRecordingWorker();
    ~ThreadRecordingWorker() override;

    Status post(Task task) override;
    void join() override;

    /// Stops accepting tasks and joins the thread. Idempotent; called by the
    /// destructor. Queued tasks are dropped, not run.
    void shutdown();

private:
    void run();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Task> queue_;
    bool busy_ = false;
    bool stopping_ = false;
    std::unique_ptr<std::thread> thread_;
};

/// VAD auto-stop seam: returns true to stop the session. With no hook and no
/// cached segmenter the background loop is a no-op seam, and the session then
/// ends only through press/release/cancel.
using VadAutoStopHook = std::function<bool(const std::vector<float>& samples)>;

struct RecordingStateMachineOptions {
    RecordingMode mode = RecordingMode::push_to_talk;
    /// VAD loop poll interval. Zero polls as fast as the ports allow and is only
    /// meant for deterministic tests.
    std::chrono::milliseconds vad_poll_interval{250};
    /// Used by the built-in auto-stop detector when no VadAutoStopHook is set
    /// and a cached segmenter is available.
    double vad_silence_threshold_seconds = 1.2;
};

// ---------------------------------------------------------------------------
// Machine
// ---------------------------------------------------------------------------

class RecordingStateMachine final {
public:
    using StateChangedHandler = std::function<void(RecordingState)>;
    using TextReadyHandler = std::function<void(std::string)>;
    using FailedHandler = std::function<void(Error)>;

    /// Ownership: the machine borrows every port and the worker, so they must
    /// outlive it. `segmenter` is optional, non-owning and cached by the
    /// composition root; the machine resets it between sessions and never
    /// destroys it.
    RecordingStateMachine(
        RecordingPort& recorder,
        TranscriptionPort& transcription,
        TextOutputPort& output,
        RecordingWorker& worker,
        RecordingStateMachineOptions options = {},
        SpeechSegmenter* segmenter = nullptr);
    ~RecordingStateMachine();

    RecordingStateMachine(const RecordingStateMachine&) = delete;
    RecordingStateMachine& operator=(const RecordingStateMachine&) = delete;
    RecordingStateMachine(RecordingStateMachine&&) = delete;
    RecordingStateMachine& operator=(RecordingStateMachine&&) = delete;

    /// Handlers are snapshotted per event, so they may be replaced at runtime.
    /// They run on the thread that published the event, outside the internal
    /// lock; a handler must marshal to the UI thread itself. A handler that
    /// throws cannot take down recording.
    void set_state_changed_handler(StateChangedHandler handler);
    void set_text_ready_handler(TextReadyHandler handler);
    void set_failed_handler(FailedHandler handler);

    /// Read on every session, so a settings change applies immediately.
    void set_options_provider(std::function<SessionOptions()> provider);

    /// Mode and VAD threshold are read when a session STARTS, not fixed at
    /// construction, exactly like the .NET machine, which asks the settings view
    /// model for the current values. Without this a user who switches to VAD in
    /// the settings would keep getting push-to-talk until the process restarted.
    /// With no provider set, the construction options are used unchanged.
    struct LiveSettings {
        RecordingMode mode = RecordingMode::push_to_talk;
        double vad_silence_threshold_seconds = 1.2;
    };
    void set_live_settings_provider(std::function<LiveSettings()> provider);

    /// Optional replacement for the built-in auto-stop detector.
    void set_vad_hook(VadAutoStopHook hook);

    [[nodiscard]] RecordingState state() const noexcept;
    /// The mode the NEXT session will use.
    [[nodiscard]] RecordingMode mode() const noexcept;
    /// The VAD threshold the next session will use, in seconds.
    [[nodiscard]] double live_silence_threshold_seconds() const;
    [[nodiscard]] bool is_recording() const noexcept;
    [[nodiscard]] bool is_processing() const noexcept;
    /// True while the current session owns the capture port.
    [[nodiscard]] bool has_active_session() const noexcept;
    [[nodiscard]] bool is_disposed() const noexcept;
    /// Reports the borrowed segmenter so a test can prove that dispose() and
    /// reset_session() kept the very same cached instance alive.
    [[nodiscard]] SpeechSegmenter* cached_segmenter() const noexcept;

    /// Hotkey press. From idle it starts a session in every mode; in toggle mode
    /// a press while recording stops and processes. Any other press is ignored
    /// and returns success.
    Status press_record();

    /// Hotkey release. Stops and processes only in push_to_talk mode, matching
    /// the .NET machine; every other mode/release combination is a no-op.
    Status release_record();

    /// Cancels the session in flight. Idempotent: with no active session it
    /// touches no port and publishes no event. A processing step that is already
    /// running may not deliver text or restore a state afterwards.
    void cancel();

    /// Cancels the session and resets the cached segmenter's VAD state between
    /// sessions. The segmenter instance itself is never destroyed.
    void reset_session();

    /// Idempotent teardown: cancels, returns to idle and stops the VAD loop. The
    /// worker is not shut down (it belongs to the caller) and the cached
    /// segmenter is neither reset nor destroyed.
    void dispose();

    /// Waits until no processing step is in flight. Deterministic with a manual
    /// worker (it runs the queued tasks) and safe with the thread worker.
    /// Returns false on timeout.
    bool wait_idle(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    /// Unlocked readers. The public mode() and live_silence_threshold_seconds()
    /// take the lock themselves; call sites that already hold mutex_ MUST use
    /// these, because std::mutex is not recursive and re-locking it inside a
    /// locked scope self-deadlocks the whole machine.
    [[nodiscard]] RecordingMode mode_locked() const;
    [[nodiscard]] double live_silence_threshold_seconds_locked() const;

private:
    using Callback = std::function<void()>;

    // Every *_locked helper requires mutex_ to be held and never calls a port.
    void queue_state_change_locked(RecordingState next);
    void queue_text_ready_locked(std::string text);
    void queue_failure_locked(Error error);
    void queue_segmenter_reset_locked();
    [[nodiscard]] std::vector<Callback> take_callbacks_locked();
    static void invoke_all(std::vector<Callback>& callbacks);

    // Cancels the active session. Returns true when a session was cancelled.
    bool cancel_active_session_locked(std::vector<Callback>& callbacks);
    void run_port_cancel() const noexcept;

    // Opens the capture port for `epoch` and, in VAD mode, starts the auto-stop
    // loop. Returns the start status; a failure never leaves a live session.
    Status start_session(std::uint64_t epoch);

    // Stops the capture for `epoch` and hands the buffer to the worker.
    // stop-before-process lives here.
    Status finish_capture(std::uint64_t epoch);

    // True when `epoch` still owns the session and its token is cancelled.
    [[nodiscard]] bool token_cancelled_locked(std::uint64_t epoch) const noexcept;

    // Runs on a worker thread; no lock is held while ports are called.
    void run_processing(std::uint64_t epoch, SampleBuffer audio);
    void run_vad_loop(std::uint64_t epoch);
    void finish_processing(std::uint64_t epoch, Error error, bool report, std::string text);

    [[nodiscard]] std::uint64_t begin_session_locked();

    RecordingPort& recorder_;
    TranscriptionPort& transcription_;
    TextOutputPort& output_;
    RecordingWorker& worker_;
    RecordingStateMachineOptions options_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    RecordingState state_ = RecordingState::idle;
    bool disposed_ = false;
    bool stop_requested_ = false;
    bool session_active_ = false;
    bool processing_in_flight_ = false;
    bool segmenter_reset_pending_ = false;
    std::uint64_t session_epoch_ = 0;
    CancellationSource session_cancel_;
    std::vector<Callback> pending_callbacks_;

    mutable std::mutex handlers_mutex_;
    SpeechSegmenter* const segmenter_ = nullptr;
    VadAutoStopHook vad_hook_;
    std::function<SessionOptions()> options_provider_;
    std::function<LiveSettings()> live_settings_provider_;
    StateChangedHandler state_changed_;
    TextReadyHandler text_ready_;
    FailedHandler failed_;
};

/// True when `text` is empty or contains only ASCII whitespace. The .NET check
/// is string.IsNullOrWhiteSpace; the machine applies the same rule so a
/// whitespace-only transcript never reaches the clipboard.
[[nodiscard]] bool is_blank_text(std::string_view text) noexcept;

} // namespace voicetyper::domain
