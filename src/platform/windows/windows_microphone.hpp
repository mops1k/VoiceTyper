#pragma once

// Windows microphone enumeration: IMMDeviceEnumerator over DataFlow.Capture.
//
// Evidence and contract:
//   * src/platform/api/microphone.hpp (frozen contract: active capture
//     endpoints only, opaque persisted id, empty list is a success).
//   * .NET reference VoiceTyper.Core/Services/MicrophoneService.cs:
//     MMDeviceEnumerator + EnumerateAudioEndPoints(DataFlow.Capture,
//     DeviceState.Active) mapped to (Id, FriendlyName), with *every* exception
//     swallowed into an empty list. That swallow is the documented defect this
//     port does not reproduce: the same enumeration runs here, but a failure is
//     reported as an ErrorCode (permission_denied / unavailable) and an empty
//     list is reserved for "the machine genuinely has no microphone".
//   * .NET reference VoiceTyper.Core/Audio/RawWasapiCapture.cs ResolveDevice:
//     a selected device id is honoured when it is neither empty nor "0", and
//     the default endpoint is otherwise taken from a role. "0" is kept as an
//     alias for "default" so a settings value written by the old build keeps
//     working.
//
// Platform boundary: this header is standard C++20 with no <windows.h>, so the
// contract test compiles on any host and the IMM* types stay in the .cpp.
//
// Thread affinity: safe from any thread. Every call initializes and releases
// COM on the calling thread, so two threads may enumerate concurrently. The
// implementation deliberately does not cache: a cached device list is what
// makes "the selected device disappeared" undetectable, and the settings page
// must re-enumerate after a hot-plug.

#include "domain/error.hpp"
#include "platform/api/microphone.hpp"

#include <string>
#include <vector>

namespace voicetyper::platform {

using domain::Error;
using domain::ErrorCode;
using domain::Result;
using domain::Status;
// MicrophoneDevice is declared by platform/api/microphone.hpp in this same
// namespace, so it needs no using-declaration here.

/// The frozen platform::MicrophoneService implementation for Windows.
///
/// Ownership: stateless apart from the last diagnostic, so one instance may be
/// shared. `list_devices()` cannot report a failure in its signature (that is
/// the frozen contract), so the Error of the most recent call is available from
/// last_error() and diagnostics() for logging and for the settings page.
class WindowsMicrophone final : public MicrophoneService {
public:
    WindowsMicrophone() = default;

    ~WindowsMicrophone() override = default;

    WindowsMicrophone(const WindowsMicrophone&) = delete;
    WindowsMicrophone& operator=(const WindowsMicrophone&) = delete;
    WindowsMicrophone(WindowsMicrophone&&) = delete;
    WindowsMicrophone& operator=(WindowsMicrophone&&) = delete;

    /// Lists active capture endpoints as (id, friendly name, default flag,
    /// probed mix format).
    ///
    /// Diagnostics, in the order the checks run:
    ///   * COM refused / MMDeviceEnumerator not creatable -> unavailable;
    ///   * EnumAudioEndpoints refused                  -> unavailable;
    ///   * every endpoint name blocked by privacy settings -> permission_denied
    ///     (the list is still returned with the ids as names, so a caller can
    ///     show something useful; only a caller that asks for the error code
    ///     sees permission_denied);
    ///   * nothing active                                -> an empty list, which
    ///     is a success and not an error.
    [[nodiscard]] std::vector<MicrophoneDevice> list_devices() override;

    /// The id of the system default capture endpoint, or "" when unknown. The
    /// console role is asked for first and the communications role second,
    /// which is the same order the capture backend uses.
    [[nodiscard]] std::string default_device_id() override;

    /// True when `device_id` still resolves to an active capture endpoint. An
    /// empty id means "system default" and is true when a default exists.
    [[nodiscard]] bool is_device_available(const std::string& device_id) override;

    /// The Error of the most recent call. `is_ok()` after a successful one.
    [[nodiscard]] const Error& last_error() const noexcept { return last_error_; }

    /// One line naming the last failure and its HRESULT-ish cause, e.g.
    /// "microphone enumeration: permission_denied (endpoint name blocked)".
    /// Empty after a successful call.
    [[nodiscard]] const std::string& diagnostics() const noexcept { return diagnostics_; }

private:
    mutable Error last_error_;
    mutable std::string diagnostics_;
};

/// The endpoint id meaning "system default", as written by the .NET settings.
constexpr std::string_view kDefaultMicrophoneDeviceId = "0";

/// True when `device_id` selects the system default endpoint rather than a
/// specific device: empty, or the legacy "0" alias.
[[nodiscard]] bool is_default_microphone_id(std::string_view device_id);

} // namespace voicetyper::platform
