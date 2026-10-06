#pragma once

// Engine lifecycle owner. It is deliberately separate from
// platform::EngineRegistry (which owns factories) and from the recording state
// machine (which owns a dictation session): this object owns exactly one loaded
// engine at a time, loads it on its own thread and swaps it without ever
// freeing a context that an in-flight job still holds.
//
// Rules encoded here, each with a contract test:
//   * transcribe() never lazily loads. A model that is not warm is an explicit
//     model_not_ready state, not a multi-second stall on the hotkey path.
//   * Engine selection is non-blocking: select() bumps an epoch and returns.
//   * A background load that finishes after a newer select() publishes nothing
//     and its engine is destroyed, so a stale load can never become active.
//   * The retired engine is released only after the recording state machine is
//     idle, which is why the host exposes wait_until_releasable().
//   * No engine is ever substituted. A missing/ABI-mismatched engine is
//     engine_unavailable, and readiness keeps the reason.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/recording_state_machine.hpp"
#include "domain/settings.hpp"
#include "platform/api/engine_registry.hpp"
#include "platform/api/transcriber.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace voicetyper::asr {

/// Single-threaded, LIFO-drop worker for model load and warm-up.
///
/// It is NOT the recording worker: a 1-3 s warm-up there would block the VAD
/// loop and the processing step of the next dictation.
class EngineExecutor {
public:
    using Task = std::function<void()>;

    EngineExecutor();
    ~EngineExecutor();

    EngineExecutor(const EngineExecutor&) = delete;
    EngineExecutor& operator=(const EngineExecutor&) = delete;

    /// Queues `task` under `epoch`. A task older than the newest posted epoch is
    /// dropped immediately, and older queued tasks are evicted, so the newest
    /// user choice always wins.
    void post(std::uint64_t epoch, Task task);
    /// Runs queued tasks on the calling thread until the queue is empty.
    void drain();
    /// Waits until the queue is empty and the worker is idle.
    bool wait_idle(std::chrono::milliseconds timeout);
    void shutdown();
    [[nodiscard]] std::size_t pending() const;
    [[nodiscard]] std::uint64_t newest_epoch() const noexcept;

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::pair<std::uint64_t, Task>> queue_;
    std::uint64_t newest_epoch_ = 0;
    bool running_ = false;
    std::thread thread_;
};

/// Readiness of the *current load*, which is a different axis from
/// platform::EngineAvailability (a property of the machine).
enum class EngineReadiness : std::uint8_t {
    /// No engine selected yet.
    idle = 0,
    /// The machine cannot run the selected engine (missing library, ABI, OS).
    unavailable = 1,
    /// No model path registered, or the model file is absent.
    model_missing = 2,
    /// A load is running in the background.
    loading = 3,
    /// The model is loaded; deep warm-up may still be running.
    ready = 4,
    /// Deep warm-up is running; transcription is allowed.
    warming = 5,
    /// Loading or warm-up failed; last_error explains it.
    failed = 6,
    /// The host is shutting down.
    shutting_down = 7,
};

[[nodiscard]] std::string_view engine_readiness_name(EngineReadiness readiness) noexcept;

struct EngineStateSnapshot {
    EngineReadiness readiness = EngineReadiness::idle;
    domain::TranscriptionEngine engine = domain::TranscriptionEngine::whisper;
    std::string model_path;
    std::optional<int> abi_version;
    platform::EngineAvailabilityReason reason = platform::EngineAvailabilityReason::available;
    std::string last_error;
};

/// Adapts the platform Transcriber to domain::TranscriptionPort and owns its
/// lifetime across a hotkey-driven dictation.
class EngineHost final : public domain::TranscriptionPort {
public:
    explicit EngineHost(platform::EngineRegistry& registry);
    ~EngineHost() override;

    EngineHost(const EngineHost&) = delete;
    EngineHost& operator=(const EngineHost&) = delete;

    /// Starts loading `engine` with `model_path` in the background and returns
    /// immediately. Never fails because of the engine itself: the outcome is
    /// published through state().
    void select(domain::TranscriptionEngine engine, std::filesystem::path model_path);

    [[nodiscard]] EngineStateSnapshot state() const;
    /// True when a dictation may start. Lock-free by design: the hotkey path must
    /// not block on a model load.
    [[nodiscard]] bool is_ready_for_recording() const noexcept;
    /// Blocks until a background load finished, or the timeout expires.
    bool wait_until_settled(std::chrono::milliseconds timeout);

    /// domain::TranscriptionPort. Returns model_not_ready without touching the
    /// engine when it is not warm, engine_unavailable when the machine cannot
    /// run the selected engine, and cancelled when the token fired.
    [[nodiscard]] domain::Result<std::string> transcribe(
        const domain::SampleBuffer& audio,
        const domain::SessionOptions& options,
        const domain::CancellationToken& cancellation) override;

    /// Stops accepting work, asks the background load to stop and joins it. The
    /// engine is released only when no transcription is in flight; with a
    /// deadline that expires while a non-interruptible engine is still running,
    /// the context is deliberately leaked instead of freed under an inference.
    domain::Status shutdown(std::chrono::milliseconds deadline);

    [[nodiscard]] std::uint64_t epoch() const noexcept;

private:
    void run_load(std::uint64_t epoch, domain::TranscriptionEngine engine, std::filesystem::path model_path);
    void publish(std::uint64_t epoch, EngineStateSnapshot snapshot, std::shared_ptr<platform::Transcriber> engine);
    void retire_locked(std::shared_ptr<platform::Transcriber> engine);

    platform::EngineRegistry& registry_;
    EngineExecutor executor_;

    mutable std::mutex mutex_;
    std::condition_variable settled_;
    EngineStateSnapshot state_;
    std::atomic<std::uint64_t> epoch_{1};
    std::atomic<std::uint32_t> in_flight_{0};
    std::shared_ptr<platform::Transcriber> active_;
    std::vector<std::shared_ptr<platform::Transcriber>> retired_;
    bool shutting_down_ = false;
};

} // namespace voicetyper::asr
