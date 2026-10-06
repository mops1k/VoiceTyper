#pragma once

// Portable executor contract. No Qt, OS, or thread-pool implementation lives
// here: QtUiExecutor/ThreadExecutor are platform adapters in later phases.
// Tasks are plain callables and must not throw; invoke() converts an escaped
// exception into ErrorCode::internal so no exception crosses the boundary.

#include "domain/error.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace voicetyper::platform {

using Task = std::function<void()>;
using TaskGeneration = std::uint64_t;

class Executor {
public:
    virtual ~Executor() = default;

    virtual void post(Task task) = 0;
    virtual void post(TaskGeneration generation, Task task) = 0;
    virtual void post_delayed(TaskGeneration generation, Task task, std::chrono::milliseconds delay) = 0;
    virtual domain::Status invoke(Task task, std::chrono::milliseconds timeout) = 0;
    virtual domain::Status shutdown(std::chrono::milliseconds deadline) = 0;
    [[nodiscard]] virtual bool on_this_thread() const noexcept = 0;
    [[nodiscard]] virtual std::size_t pending() const noexcept = 0;
};

class ManualExecutor final : public Executor {
public:
    void post(Task task) override
    {
        post(generation(), std::move(task));
    }

    void post(TaskGeneration task_generation, Task task) override
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(Scheduled{task_generation, std::chrono::steady_clock::now(), std::move(task)});
    }

    void post_delayed(TaskGeneration task_generation, Task task, std::chrono::milliseconds delay) override
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(Scheduled{
            task_generation,
            std::chrono::steady_clock::now() + delay,
            std::move(task),
        });
    }

    domain::Status invoke(Task task, std::chrono::milliseconds /*timeout*/) override
    {
        return run_one(std::move(task));
    }

    domain::Status shutdown(std::chrono::milliseconds /*deadline*/) override
    {
        std::lock_guard lock(mutex_);
        queue_.clear();
        ++generation_;
        return domain::Status::success();
    }

    [[nodiscard]] bool on_this_thread() const noexcept override
    {
        return true;
    }

    [[nodiscard]] std::size_t pending() const noexcept override
    {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] TaskGeneration generation() const noexcept
    {
        std::lock_guard lock(mutex_);
        return generation_;
    }

    void advance_generation() noexcept
    {
        std::lock_guard lock(mutex_);
        ++generation_;
    }

    /// Executes every task that is due. Returns the first task error, if any.
    domain::Status drain()
    {
        while (true) {
            Scheduled next;
            {
                std::lock_guard lock(mutex_);
                if (queue_.empty()) {
                    return domain::Status::success();
                }
                const auto now = std::chrono::steady_clock::now();
                const auto it = std::find_if(queue_.begin(), queue_.end(), [now](const Scheduled& item) {
                    return item.due <= now;
                });
                if (it == queue_.end()) {
                    return domain::Status::success();
                }
                next = std::move(*it);
                queue_.erase(it);
            }

            if (next.generation != generation()) {
                continue; // stale session/executor epoch: drop silently
            }
            const auto result = run_one(std::move(next.task));
            if (!result.is_ok()) {
                return result;
            }
        }
    }

private:
    struct Scheduled {
        TaskGeneration generation = 0;
        std::chrono::steady_clock::time_point due{};
        Task task;
    };

    static domain::Status run_one(Task task)
    {
        if (!task) {
            return domain::Status::failure(domain::ErrorCode::invalid_argument, "empty executor task");
        }
        try {
            task();
            return domain::Status::success();
        } catch (...) {
            return domain::Status::failure(domain::ErrorCode::internal, "executor task threw");
        }
    }

    mutable std::mutex mutex_;
    std::deque<Scheduled> queue_;
    TaskGeneration generation_ = 1;
};

class InlineExecutor final : public Executor {
public:
    void post(Task task) override
    {
        post(generation(), std::move(task));
    }

    void post(TaskGeneration task_generation, Task task) override
    {
        if (task_generation == generation()) {
            static_cast<void>(run_one(std::move(task)));
        }
    }

    void post_delayed(TaskGeneration task_generation, Task task, std::chrono::milliseconds /*delay*/) override
    {
        if (task_generation == generation()) {
            static_cast<void>(run_one(std::move(task)));
        }
    }

    domain::Status invoke(Task task, std::chrono::milliseconds /*timeout*/) override
    {
        return run_one(std::move(task));
    }

    domain::Status shutdown(std::chrono::milliseconds /*deadline*/) override
    {
        ++generation_;
        return domain::Status::success();
    }

    [[nodiscard]] bool on_this_thread() const noexcept override
    {
        return true;
    }

    [[nodiscard]] std::size_t pending() const noexcept override
    {
        return 0;
    }

    [[nodiscard]] TaskGeneration generation() const noexcept
    {
        return generation_;
    }

    void advance_generation() noexcept
    {
        ++generation_;
    }

private:
    static domain::Status run_one(Task task)
    {
        if (!task) {
            return domain::Status::failure(domain::ErrorCode::invalid_argument, "empty executor task");
        }
        try {
            task();
            return domain::Status::success();
        } catch (...) {
            return domain::Status::failure(domain::ErrorCode::internal, "executor task threw");
        }
    }

    TaskGeneration generation_ = 1;
};

} // namespace voicetyper::platform
