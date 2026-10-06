#pragma once

// Cooperative cancellation contract.
//
// Why this exists instead of std::stop_token: the migration targets a fixed
// toolchain set (MSVC/MinGW-w64 + Arch GCC) and needs one shared header usable
// from pure domain code, the platform layer and the Qt UI without dragging in
// <stop_token> availability questions. The semantics are deliberately identical
// to std::stop_source/std::stop_token.
//
// Rules:
//   * Cancellation is cooperative: every long-running contract in this codebase
//     takes a `const CancellationToken&` and polls it through `check_cancelled`.
//   * Cancellation is sticky: once requested it stays requested until reset().
//   * A default-constructed token can never be cancelled (the "no token" case).
//   * No exceptions cross the boundary; a cancelled operation returns
//     ErrorCode::cancelled.
//   * Thread safety: request_cancelled()/reset() are safe from any thread;
//     is_cancellation_requested() is safe to call concurrently from any thread.

#include "domain/error.hpp"

#include <atomic>
#include <memory>
#include <utility>

namespace voicetyper::domain {

namespace detail {

/// Shared flag between a source and every token derived from it.
/// Copies of a token share one state, so cancelling the source is observed by
/// all of them (this is the observable .NET CancellationTokenSource contract).
class CancellationState {
public:
    [[nodiscard]] bool is_requested() const noexcept { return flag_.load(std::memory_order_acquire); }

    void request() noexcept { flag_.store(true, std::memory_order_release); }

    void reset() noexcept { flag_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> flag_{false};
};

} // namespace detail

/// Read-only view of a cancellation request. Cheap to copy (one shared_ptr).
class CancellationToken {
public:
    /// The "not cancellable" token. Copying and comparing it are trivial.
    CancellationToken() noexcept = default;

    [[nodiscard]] bool is_cancellation_requested() const noexcept
    {
        return state_ != nullptr && state_->is_requested();
    }

    /// False only for a default-constructed token. Implementations use this to
    /// skip registering a poll where no source exists.
    [[nodiscard]] bool can_be_cancelled() const noexcept { return state_ != nullptr; }

    friend bool operator==(const CancellationToken& lhs, const CancellationToken& rhs) noexcept
    {
        return lhs.state_ == rhs.state_;
    }

private:
    friend class CancellationSource;

    explicit CancellationToken(std::shared_ptr<detail::CancellationState> state) noexcept
        : state_(std::move(state))
    {
    }

    std::shared_ptr<detail::CancellationState> state_;
};

/// Owns a cancellation flag and hands out tokens that observe it.
///
/// Lifetime/ownership: the source owns the shared state. Tokens keep the state
/// alive, so destroying the source never leaves a dangling token; a token then
/// simply reports "not requested" forever after.
class CancellationSource {
public:
    /// Creates an independent source.
    CancellationSource()
        : state_(std::make_shared<detail::CancellationState>())
    {
    }

    /// Creates a source that shares `token`'s state. Used to forward a parent
    /// scope's cancellation into a child operation.
    explicit CancellationSource(const CancellationToken& token)
        : state_(token.state_)
    {
        if (state_ == nullptr) {
            state_ = std::make_shared<detail::CancellationState>();
        }
    }

    [[nodiscard]] CancellationToken token() const noexcept
    {
        return CancellationToken(state_);
    }

    void request_cancellation() noexcept
    {
        if (state_ != nullptr) {
            state_->request();
        }
    }

    /// Clears the flag so the source can be reused for the next operation.
    /// Callers must guarantee no operation is still observing the token; the
    /// reset is a race by construction and is the caller's responsibility.
    void reset() noexcept
    {
        if (state_ != nullptr) {
            state_->reset();
        }
    }

    [[nodiscard]] bool is_cancellation_requested() const noexcept
    {
        return state_ != nullptr && state_->is_requested();
    }

private:
    std::shared_ptr<detail::CancellationState> state_;
};

/// Converts a cancellation request into a Status, so polling fits the same
/// `Result`-based control flow as every other failure.
[[nodiscard]] inline Status check_cancelled(const CancellationToken& token) noexcept
{
    if (token.is_cancellation_requested()) {
        return Status::failure(ErrorCode::cancelled, "operation cancelled");
    }
    return Status::success();
}

} // namespace voicetyper::domain
