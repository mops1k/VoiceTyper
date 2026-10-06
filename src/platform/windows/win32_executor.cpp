#include "platform/windows/win32_executor.hpp"

#include <algorithm>
#include <utility>

namespace voicetyper::platform {

Win32Executor::Win32Executor()
{
    try {
        worker_ = std::thread([this] { worker_main(); });
    } catch (...) {
        // A thread that cannot be created is not a VoiceTyper defect the user
        // can act on; the executor reports unavailable on every call instead of
        // pretending the task is queued.
        running_.store(false, std::memory_order_release);
        return;
    }
    running_.store(true, std::memory_order_release);
}

Win32Executor::~Win32Executor()
{
    stop();
}

bool Win32Executor::on_this_thread() const noexcept
{
    return std::this_thread::get_id() == worker_id_.load(std::memory_order_acquire);
}

bool Win32Executor::is_running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

TaskGeneration Win32Executor::generation() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

std::size_t Win32Executor::pending() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void Win32Executor::post(Task task)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !running_.load(std::memory_order_acquire)) {
        return;
    }
    queue_.push_back(Scheduled{
        generation_,
        std::chrono::steady_clock::now(),
        std::move(task),
        nullptr,
    });
    work_signal_.notify_one();
}

void Win32Executor::post(TaskGeneration task_generation, Task task)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !running_.load(std::memory_order_acquire)) {
        return;
    }
    queue_.push_back(Scheduled{
        task_generation,
        std::chrono::steady_clock::now(),
        std::move(task),
        nullptr,
    });
    work_signal_.notify_one();
}

void Win32Executor::post_delayed(TaskGeneration task_generation, Task task, std::chrono::milliseconds delay)
{
    if (delay < std::chrono::milliseconds::zero()) {
        delay = std::chrono::milliseconds::zero();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !running_.load(std::memory_order_acquire)) {
        return;
    }
    queue_.push_back(Scheduled{
        task_generation,
        std::chrono::steady_clock::now() + delay,
        std::move(task),
        nullptr,
    });
    work_signal_.notify_one();
}

domain::Status Win32Executor::run_task(Task& task, InvokeState* state)
{
    domain::Status result = domain::Status::success();
    if (!task) {
        result = domain::Status::failure(domain::ErrorCode::invalid_argument, "empty executor task");
    } else {
        try {
            task();
        } catch (...) {
            // The frozen contract: an exception must never cross the executor
            // boundary. It becomes internal, and the worker thread survives.
            result = domain::Status::failure(domain::ErrorCode::internal, "executor task threw");
        }
    }
    if (state != nullptr) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->result = result;
        state->done = true;
        state->done_signal.notify_all();
    }
    return result;
}

bool Win32Executor::run_scheduled(Scheduled& item)
{
    if (item.invoke) {
        // A blocking invoke: report the result to the waiting caller. If the
        // caller already timed out it set done itself, and the request is
        // abandoned rather than run late.
        {
            std::lock_guard<std::mutex> lock(item.invoke->mutex);
            if (item.invoke->done) {
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = true;
            idle_signal_.notify_all();
        }
        // The task lives in the shared invoke state, not in the queued copy, so
        // the caller owns it and the timeout path can abandon it safely.
        run_task(item.invoke->task, item.invoke.get());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            idle_signal_.notify_all();
        }
        return true;
    }
    run_task(item.task, nullptr);
    return true;
}

void Win32Executor::worker_main()
{
    worker_id_.store(std::this_thread::get_id(), std::memory_order_release);

    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        // Every wait below carries a predicate. A bare wait would sleep with no
        // deadline, and a notification delivered before the worker reached the
        // wait would be lost - the executor would then stop running tasks
        // forever, which is a far worse failure than a slow queue.
        if (queue_.empty()) {
            if (stopping_) {
                break;
            }
            work_signal_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        auto next = std::min_element(queue_.begin(), queue_.end(), [](const Scheduled& lhs, const Scheduled& rhs) {
            return lhs.due < rhs.due;
        });
        if (next->due > now) {
            // Sleep only until the earliest due task, so a delayed post does not
            // add a second of latency to an immediate one.
            if (stopping_) {
                break;
            }
            work_signal_.wait_until(lock, next->due);
            continue;
        }

        Scheduled item = std::move(*next);
        queue_.erase(next);
        // The member field is read directly, not through generation(): the worker
        // already holds mutex_ and std::mutex is not recursive, so calling the
        // accessor here would self-deadlock on the very first task the executor
        // runs. That is not a theoretical hazard - it is what the first run of
        // this test actually hung on.
        const bool stale = item.generation != generation_;
        lock.unlock();

        if (!stale) {
            // A stale epoch is dropped silently, exactly like ManualExecutor.
            static_cast<void>(run_scheduled(item));
        }

        lock.lock();
        if (queue_.empty()) {
            idle_signal_.notify_all();
        }
    }
    lock.unlock();
    idle_signal_.notify_all();
    worker_id_.store(std::thread::id{}, std::memory_order_release);
}

domain::Status Win32Executor::invoke(Task task, std::chrono::milliseconds timeout)
{
    if (!task) {
        return domain::Status::failure(domain::ErrorCode::invalid_argument, "empty executor task");
    }
    if (!is_running()) {
        return domain::Status::failure(domain::ErrorCode::unavailable, "executor thread is not running");
    }

    // The Qt blocking-invoke trap. Posting and then waiting here would block
    // the only thread able to run the task, so the task runs inline instead.
    if (on_this_thread()) {
        return run_task(task, nullptr);
    }

    auto state = std::make_shared<InvokeState>();
    state->task = std::move(task);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return domain::Status::failure(domain::ErrorCode::unavailable, "executor is shutting down");
        }
        queue_.push_back(Scheduled{
            // The member field, not the generation() accessor: mutex_ is already
            // held here and std::mutex is not recursive, so calling the accessor
            // would self-deadlock the *caller* on the first blocking invoke and
            // leave the worker idle forever. Lock-taking accessors must never be
            // called from a scope that already owns the lock.
            generation_,
            std::chrono::steady_clock::now(),
            Task{},
            state,
        });
        work_signal_.notify_one();
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->done_signal.wait_for(lock, timeout, [&state] { return state->done; })) {
        return state->result;
    }
    // The worker has not started the task yet. Mark it abandoned so it is never
    // executed after the caller has given up.
    state->done = true;
    return domain::Status::failure(domain::ErrorCode::timeout, "executor invoke timed out");
}

domain::Status Win32Executor::shutdown(std::chrono::milliseconds deadline)
{
    std::unique_lock<std::mutex> lock(mutex_);
    // Everything queued belongs to the epoch that is ending.
    ++generation_;
    queue_.clear();

    idle_signal_.notify_all();
    if (busy_ && deadline > std::chrono::milliseconds::zero()) {
        if (!idle_signal_.wait_for(lock, deadline, [this] { return !busy_; })) {
            return domain::Status::failure(
                domain::ErrorCode::timeout, "executor shutdown deadline expired while a task was running");
        }
    }
    return domain::Status::success();
}

bool Win32Executor::wait_for_idle(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    const auto is_idle = [this] { return queue_.empty() && !busy_; };
    if (is_idle()) {
        return true;
    }
    return idle_signal_.wait_for(lock, timeout, is_idle);
}

void Win32Executor::stop()
{
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    work_signal_.notify_all();
    idle_signal_.notify_all();
    if (worker_.joinable()) {
        // Any in-flight task was already cancelled from the destructor path, so
        // joining cannot block on an unbounded task for longer than one call.
        worker_.join();
    }
    running_.store(false, std::memory_order_release);
}

} // namespace voicetyper::platform
