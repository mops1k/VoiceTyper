#pragma once

// Portable error/result contract for the VoiceTyper C++ migration.
//
// Contract rules (Phase A of plan p_312b2ec83985):
//   * No exception ever crosses a module or platform interface boundary.
//     Every fallible operation reports an ErrorCode plus a human-readable message.
//   * Only standard C++20 headers are used: this header must stay compilable in
//     a Qt-free, OS-free build (see cmake/PortableHeaders.cmake).
//   * A `Status` carries success/failure only; `Result<T>` additionally carries a
//     value. `Result<void>` is the "callable that can fail" spelling.
//
// Intent-parity note: error codes describe *observable user-visible conditions*
// (model not ready, device gone, clipboard busy, engine unavailable). They do not
// reproduce .NET exception types as behavior, and no code here decides a policy
// that the migration plan still leaves open.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace voicetyper::domain {

/// Coarse, stable classification of a failure. Values are wire-visible only
/// inside diagnostics; they are not part of the settings or model-file formats.
enum class ErrorCode : std::uint8_t {
    /// No failure. Never stored in a failing Status/Error.
    ok = 0,
    /// The operation was aborted by a CancellationToken.
    cancelled,
    /// A caller supplied an argument outside the documented contract.
    invalid_argument,
    /// A required resource (file, device, engine, model) does not exist.
    not_found,
    /// A resource that must be unique already exists (e.g. single instance).
    already_exists,
    /// The resource exists but is not usable yet (model/engine still loading).
    not_ready,
    /// The requested capability is not implemented on this platform.
    unsupported,
    /// Access was denied by the OS (UAC integrity mismatch, ACL, privacy mode).
    permission_denied,
    /// A dependency (native library, service, display) is not present.
    unavailable,
    /// A capture/input device disappeared while it was in use.
    device_disconnected,
    /// The operation exceeded its documented deadline.
    timeout,
    /// Underlying filesystem/network/OS I/O failure.
    io_failure,
    /// Data did not match the expected format (truncated WAV, bad JSON, ...).
    corrupt_data,
    /// A documented numeric bound was violated.
    out_of_range,
    /// A bounded resource (thread, memory, file handle) is exhausted.
    resource_exhausted,
    /// The requested transcription engine cannot be used on this machine.
    engine_unavailable,
    /// The engine exists but its model file is absent or not yet loaded.
    model_not_ready,
    /// Reserved: a call tried to violate an exclusivity/ordering contract.
    invalid_state,
    /// A defect in VoiceTyper itself. Always a bug, never a user condition.
    internal,
};

/// Stable diagnostic name of an error code, used by logs and the diagnostics
/// CLI. Never empty for a valid code.
[[nodiscard]] constexpr std::string_view error_code_name(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::ok: return "ok";
    case ErrorCode::cancelled: return "cancelled";
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::not_found: return "not_found";
    case ErrorCode::already_exists: return "already_exists";
    case ErrorCode::not_ready: return "not_ready";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::permission_denied: return "permission_denied";
    case ErrorCode::unavailable: return "unavailable";
    case ErrorCode::device_disconnected: return "device_disconnected";
    case ErrorCode::timeout: return "timeout";
    case ErrorCode::io_failure: return "io_failure";
    case ErrorCode::corrupt_data: return "corrupt_data";
    case ErrorCode::out_of_range: return "out_of_range";
    case ErrorCode::resource_exhausted: return "resource_exhausted";
    case ErrorCode::engine_unavailable: return "engine_unavailable";
    case ErrorCode::model_not_ready: return "model_not_ready";
    case ErrorCode::invalid_state: return "invalid_state";
    case ErrorCode::internal: return "internal";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool is_error(ErrorCode code) noexcept
{
    return code != ErrorCode::ok;
}

/// A failure value. Default-constructed means "no error" so that optional
/// storage of an Error never invents a failure.
class Error {
public:
    Error() noexcept = default;

    explicit Error(ErrorCode code)
        : code_(code)
    {
    }

    Error(ErrorCode code, std::string message)
        : code_(code)
        , message_(std::move(message))
    {
    }

    Error(ErrorCode code, std::string message, std::string detail)
        : code_(code)
        , message_(std::move(message))
        , detail_(std::move(detail))
    {
    }

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

    /// True when this value denotes no failure.
    [[nodiscard]] bool is_ok() const noexcept { return !is_error(code_); }

    /// Single-line rendering for logs and the diagnostics CLI:
    /// `<code_name>: <message>` plus ` (<detail>)` when a detail is present.
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const Error& lhs, const Error& rhs) noexcept
    {
        return lhs.code_ == rhs.code_ && lhs.message_ == rhs.message_ && lhs.detail_ == rhs.detail_;
    }

private:
    ErrorCode code_ = ErrorCode::ok;
    std::string message_;
    std::string detail_;
};

inline std::string Error::to_string() const
{
    std::string text(error_code_name(code_));
    if (!message_.empty()) {
        text += ": ";
        text += message_;
    }
    if (!detail_.empty()) {
        text += " (";
        text += detail_;
        text += ')';
    }
    return text;
}

/// Success-or-failure without a value.
///
/// Lifetime/ownership: value type, trivially copyable state, safe to store in
/// other value types. Not thread-safe to mutate, but copies are independent.
class Status {
public:
    /// Constructs a successful status.
    Status() noexcept = default;

    [[nodiscard]] static Status success() noexcept { return Status{}; }

    [[nodiscard]] static Status failure(Error error) { return Status(std::move(error)); }

    [[nodiscard]] static Status failure(ErrorCode code, std::string message = {})
    {
        return Status(Error(code, std::move(message)));
    }

    [[nodiscard]] bool is_ok() const noexcept { return !error_.has_value(); }
    [[nodiscard]] bool is_error() const noexcept { return error_.has_value(); }

    /// Explicit so that a Status is never silently dropped as a bool.
    [[nodiscard]] explicit operator bool() const noexcept { return is_ok(); }

    [[nodiscard]] ErrorCode code() const noexcept
    {
        return error_.has_value() ? error_->code() : ErrorCode::ok;
    }

    /// Precondition: `is_error()`.
    [[nodiscard]] const Error& error() const noexcept { return *error_; }

    /// Precondition: `is_error()`.
    [[nodiscard]] const std::string& message() const noexcept { return error_->message(); }

    /// Returns a copy of this status with `detail` appended, keeping code and
    /// message. Lets an inner layer add context without losing the cause.
    [[nodiscard]] Status with_context(std::string detail) const;

    friend bool operator==(const Status& lhs, const Status& rhs) noexcept
    {
        if (lhs.error_.has_value() != rhs.error_.has_value()) {
            return false;
        }
        return !lhs.error_.has_value() || *lhs.error_ == *rhs.error_;
    }

private:
    explicit Status(Error error)
        : error_(std::move(error))
    {
    }

    std::optional<Error> error_;
};

inline Status Status::with_context(std::string detail) const
{
    if (!error_.has_value()) {
        return *this;
    }
    return Status(Error(error_->code(), error_->message(), std::move(detail)));
}

/// Either a `T` or an `Error`, never both and never neither.
///
/// Ownership: the stored `T` is owned by value. Moving out of a failed Result is
/// a contract violation; `value()` on a failed Result is undefined-by-contract and
/// is marked accordingly.
template <typename T>
class Result {
    static_assert(!std::is_void_v<T>, "Result<void> is specialized; use Status for void operations");

public:
    using value_type = T;

    /// Implicit on purpose: `return value;` at a call site stays readable, and
    /// `Result<Error>` is never constructed accidentally in the same expression.
    Result(T value) // NOLINT(google-explicit-constructor): call-site ergonomics
        : data_(std::in_place_index<0>, std::move(value))
    {
    }

    Result(Error error) // NOLINT(google-explicit-constructor): `return Error{...};`
        : data_(std::in_place_index<1>, std::move(error))
    {
    }

    [[nodiscard]] static Result failure(ErrorCode code, std::string message = {})
    {
        return Result(Error(code, std::move(message)));
    }

    [[nodiscard]] bool is_ok() const noexcept { return data_.index() == 0; }
    [[nodiscard]] bool is_error() const noexcept { return data_.index() == 1; }
    [[nodiscard]] explicit operator bool() const noexcept { return is_ok(); }

    /// Precondition: `is_ok()`.
    [[nodiscard]] const T& value() const& { return std::get<0>(data_); }
    [[nodiscard]] T& value() & { return std::get<0>(data_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(data_)); }

    /// Precondition: `is_error()`.
    [[nodiscard]] const Error& error() const& { return std::get<1>(data_); }

    [[nodiscard]] ErrorCode code() const noexcept
    {
        return is_error() ? error().code() : ErrorCode::ok;
    }

    /// Success-only view, useful for `if (auto v = result.optional())` call sites.
    [[nodiscard]] std::optional<T> value_as_optional() const&
    {
        return is_ok() ? std::optional<T>(std::get<0>(data_)) : std::nullopt;
    }

    [[nodiscard]] T value_or(T fallback) const&
    {
        return is_ok() ? std::get<0>(data_) : std::move(fallback);
    }

    /// Success/failure projection, for chaining calls that return void.
    [[nodiscard]] Status status() const
    {
        return is_ok() ? Status::success() : Status::failure(error());
    }

private:
    std::variant<T, Error> data_;
};

/// Specialization for operations that succeed or fail without producing a value.
template <>
class Result<void> {
public:
    using value_type = void;

    /// Implicit success, so `return {};` reads as "done".
    Result() noexcept = default; // NOLINT(google-explicit-constructor): success spelling

    [[nodiscard]] static Result success() noexcept { return Result{}; }

    [[nodiscard]] static Result failure(ErrorCode code, std::string message = {})
    {
        Result result;
        result.error_ = Error(code, std::move(message));
        return result;
    }

    [[nodiscard]] static Result failure(Error error)
    {
        Result result;
        result.error_ = std::move(error);
        return result;
    }

    [[nodiscard]] bool is_ok() const noexcept { return !error_.has_value(); }
    [[nodiscard]] bool is_error() const noexcept { return error_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return is_ok(); }

    [[nodiscard]] ErrorCode code() const noexcept
    {
        return error_.has_value() ? error_->code() : ErrorCode::ok;
    }

    /// Precondition: `is_error()`.
    [[nodiscard]] const Error& error() const& { return *error_; }
    [[nodiscard]] const std::string& message() const& { return error_->message(); }

    [[nodiscard]] Status status() const
    {
        return error_.has_value() ? Status::failure(*error_) : Status::success();
    }

private:
    std::optional<Error> error_;
};

} // namespace voicetyper::domain
