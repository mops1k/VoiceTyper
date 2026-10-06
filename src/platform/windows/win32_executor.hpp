#pragma once

// Win32 implementation of the frozen platform::Executor contract.
//
// Evidence and contract:
//   * src/platform/api/executor.hpp - post/post_delayed/invoke/shutdown,
//     on_this_thread(), pending(), TaskGeneration epochs and "invoke() converts
//     an escaped exception into ErrorCode::internal".
//   * src/domain/text_output.hpp - the paste path is marshalled through
//     Executor::invoke with an on_this_thread() inline fast path, "because the
//     recording worker is not the UI thread and a blocking invoke from the UI
//     thread would deadlock".
//   * src/asr/engine_host.* - the same single-thread/epoch discipline the engine
//     host already uses for its own background work.
//
// The self-deadlock trap, stated once: invoke() from the executor's own thread
// cannot post-and-wait, because the only thread that would run the task is the
// one blocked in the wait. Win32Executor therefore checks on_this_thread() first
// and runs the task inline. That is a correctness rule, not an optimisation, and
// the contract test asserts both halves: on_this_thread() is true inside a task
// run through invoke() and false on the calling thread.
//
// Generation semantics are the ones ManualExecutor already freezes, so the two
// executors cannot drift:
//   * post(task) tags the task with the executor's current generation;
//   * post_delayed(generation, task, delay) carries the caller's generation;
//   * a task whose generation differs from the executor's current one when it
//     becomes due is dropped silently (stale session/executor epoch);
//   * shutdown() clears the queue and advances the generation, so everything
//     already queued becomes stale and nothing new from the old epoch can run.
//
// Thread affinity: post/post_delayed/shutdown/pending() are safe from any
// thread. invoke() is safe from any thread, including its own (inline path).
//
// Ownership: the executor owns its single worker thread; the destructor drains
// nothing, drops whatever is still queued and joins. Callers keep ownership of
// the task callables, which must not throw (an escaping exception becomes
// ErrorCode::internal instead of crossing the boundary).

#include "domain/error.hpp"
#include "platform/api/executor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace voicetyper::platform {

class Win32Executor final : public Executor {
public:
    /// Starts the worker thread. Construction failure is reported by
    /// is_running(); every method then refuses with ErrorCode::unavailable
    /// instead of posting into nothing.
    Win32Executor();
    ~Win32Executor() override;

    Win32Executor(const Win32Executor&) = delete;
    Win32Executor& operator=(const Win32Executor&) = delete;
    Win32Executor(Win32Executor&&) = delete;
    Win32Executor& operator=(Win32Executor&&) = delete;

    void post(Task task) override;
    void post(TaskGeneration task_generation, Task task) override;
    void post_delayed(TaskGeneration task_generation, Task task, std::chrono::milliseconds delay) override;

    /// Runs `task` on the worker thread and waits at most `timeout` for it.
    /// On the worker's own thread the task runs inline, which is what keeps a
    /// UI-thread blocking invoke from deadlocking the process.
    ///
    /// Returns the task's own status: ErrorCode::invalid_argument for an empty
    /// task, ErrorCode::internal when it throws, ErrorCode::timeout when the
    /// task was not picked up within `timeout` (in which case the task is
    /// abandoned, never run late), ErrorCode::unavailable when the worker
    /// thread is not running.
    domain::Status invoke(Task task, std::chrono::milliseconds timeout) override;

    /// Drops everything queued, advances the generation and waits up to
    /// `deadline` for an already-running task to finish. Returns
    /// ErrorCode::timeout when a task is still running at the deadline; the
    /// worker thread itself is *not* stopped, because the same executor object
    /// is reused for the next recording session.
    domain::Status shutdown(std::chrono::milliseconds deadline) override;

    /// True only on the worker thread.
    [[nodiscard]] bool on_this_thread() const noexcept override;

    /// Tasks and invoke requests waiting to run.
    [[nodiscard]] std::size_t pending() const noexcept override;

    /// The current epoch. Exposed so a caller that keeps a long-lived
    /// generation value (the recording state machine does) can observe that
    /// shutdown() invalidated it.
    ///
    /// Takes mutex_. Code that already holds mutex_ must read the generation
    /// member directly: std::mutex is not recursive, and calling this from a
    /// locked scope self-deadlocks the caller.
    [[nodiscard]] TaskGeneration generation() const noexcept;

    /// True when the worker thread was created and has not been stopped.
    [[nodiscard]] bool is_running() const noexcept;

    /// Blocks until the queue is empty, or `timeout` expires. Test/diagnostic
    /// helper: invoke() already covers the "run this and wait" case, this covers
    /// "let the posted backlog drain".
    bool wait_for_idle(std::chrono::milliseconds timeout);

private:
    /// A blocking invoke. Shared with the caller so the timeout path can mark
    /// the request abandoned without the worker ever writing into freed memory.
    struct InvokeState {
        std::mutex mutex;
        std::condition_variable done_signal;
        Task task;
        domain::Status result = domain::Status::success();
        bool done = false;
    };

    struct Scheduled {
        TaskGeneration generation = 0;
        std::chrono::steady_clock::time_point due{};
        Task task;
        /// Non-null for a blocking invoke() request; null for a plain post.
        std::shared_ptr<InvokeState> invoke;
    };

    void worker_main();
    void stop();
    /// Runs one task with the contract's exception conversion. `state` is null
    /// for a plain posted task.
    static domain::Status run_task(Task& task, InvokeState* state);
    bool run_scheduled(Scheduled& item);

    mutable std::mutex mutex_;
    /// Woken by post()/post_delayed()/invoke()/stop(). Always waited on with a
    /// predicate, so a notification that arrives before the worker starts
    /// waiting cannot be lost.
    std::condition_variable work_signal_;
    /// Separate from work_signal_ on purpose. One variable shared between "there
    /// is work" and "the queue drained" lets a notify_one() from a post() wake
    /// wait_for_idle() instead of the worker, after which the worker sleeps with
    /// no deadline and the executor stops running anything. Two variables make
    /// both waits unambiguous.
    std::condition_variable idle_signal_;
    std::vector<Scheduled> queue_;
    TaskGeneration generation_ = 1;
    bool stopping_ = false;
    /// True while the worker is executing a task; shutdown() waits for it.
    bool busy_ = false;

    std::atomic<std::thread::id> worker_id_{};
    std::thread worker_;
    std::atomic_bool running_{false};
};

} // namespace voicetyper::platform
