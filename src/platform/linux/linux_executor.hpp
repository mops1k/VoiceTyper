#pragma once

// Linux implementation of the frozen platform::Executor contract.
//
// Evidence and contract: src/platform/api/executor.hpp (post/post_delayed/
// invoke/shutdown, on_this_thread(), pending(), TaskGeneration epochs and "an
// escaped exception becomes ErrorCode::internal"), and
// src/platform/windows/win32_executor.hpp, whose semantics this backend
// reproduces exactly so the two executors cannot drift:
//
//   * post(task) tags the task with the executor's current generation;
//   * post_delayed(generation, task, delay) carries the caller's generation;
//   * a task whose generation differs from the executor's current one when it
//     becomes due is dropped silently (stale session/executor epoch);
//   * shutdown() clears the queue and advances the generation, so everything
//     already queued becomes stale; the worker thread keeps running and the
//     same executor object serves the next session;
//   * invoke() runs the task inline when it is called from the executor's own
//     thread. That is a correctness rule, not an optimisation: posting and
//     waiting there would block the only thread able to run the task.
//
// The implementation is standard C++ (std::thread, std::mutex,
// std::condition_variable); it lives in the Linux backend because that is the
// platform that uses it, exactly as the Windows backend keeps its own copy.
//
// Thread affinity: post/post_delayed/shutdown/pending()/generation() are safe
// from any thread. invoke() is safe from any thread, including its own.
//
// Ownership: the executor owns its worker thread; the destructor drops whatever
// is still queued and joins. Tasks must not throw.

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

class LinuxExecutor final : public Executor {
public:
    /// Starts the worker thread. A thread that cannot be created is reported by
    /// is_running(); every call then refuses with ErrorCode::unavailable instead
    /// of posting into nothing.
    LinuxExecutor();
    ~LinuxExecutor() override;

    LinuxExecutor(const LinuxExecutor&) = delete;
    LinuxExecutor& operator=(const LinuxExecutor&) = delete;
    LinuxExecutor(LinuxExecutor&&) = delete;
    LinuxExecutor& operator=(LinuxExecutor&&) = delete;

    void post(Task task) override;
    void post(TaskGeneration task_generation, Task task) override;
    void post_delayed(TaskGeneration task_generation, Task task, std::chrono::milliseconds delay) override;

    /// Runs `task` on the worker thread and waits at most `timeout` for it.
    /// Returns the task's own status; ErrorCode::timeout means the task was not
    /// picked up in time and is abandoned (never run late).
    domain::Status invoke(Task task, std::chrono::milliseconds timeout) override;

    /// Drops everything queued, advances the generation and waits up to
    /// `deadline` for an already-running task to finish.
    domain::Status shutdown(std::chrono::milliseconds deadline) override;

    /// True only on the worker thread.
    [[nodiscard]] bool on_this_thread() const noexcept override;

    /// Tasks and invoke requests waiting to run.
    [[nodiscard]] std::size_t pending() const noexcept override;

    /// The current epoch, so a caller that keeps a long-lived generation value
    /// can observe that shutdown() invalidated it.
    [[nodiscard]] TaskGeneration generation() const noexcept;

    /// True when the worker thread was created and has not been stopped.
    [[nodiscard]] bool is_running() const noexcept;

    /// Blocks until the queue is empty, or `timeout` expires.
    bool wait_for_idle(std::chrono::milliseconds timeout);

private:
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
    static domain::Status run_task(Task& task, InvokeState* state);
    bool run_scheduled(Scheduled& item);

    mutable std::mutex mutex_;
    std::condition_variable work_signal_;
    /// Separate from work_signal_: one variable shared between "there is work"
    /// and "the queue drained" lets a post() wake wait_for_idle() instead of the
    /// worker, after which the worker sleeps with no deadline.
    std::condition_variable idle_signal_;
    std::vector<Scheduled> queue_;
    TaskGeneration generation_ = 1;
    bool stopping_ = false;
    bool busy_ = false;

    std::atomic<std::thread::id> worker_id_{};
    std::thread worker_;
    std::atomic_bool running_{false};
};

} // namespace voicetyper::platform
