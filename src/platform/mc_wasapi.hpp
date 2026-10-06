#pragma once

// The mc_wasapi.dll capture path.
//
// Why this exists: the .NET build does not capture through its IAudioClient
// interop at all. VoiceTyper.Core/Audio/NativeWasapiCapture.cs calls a third-party
// native library, dist/mc_wasapi.dll, whose header comment says the interop breaks
// on the Intel Smart Sound array ("Initialize succeeds, GetService returns
// DEVICE_INVALIDATED") while the DLL captures fine, and its own fallback path
// (RawWasapiCapture.cs) documents that shared mode "always gives E_INVALIDARG" on
// that hardware. The C++ port originally reproduced only that fallback path, which
// is exactly why capture failed on every candidate while the .NET build opened the
// device (Alexander, 2026-10-06).
//
// The library exports exactly two functions (verified with objdump):
//   int  mc_start(mc_callback cb, int rate, int ch);   // Cdecl
//   void mc_stop();
// with `typedef void (*mc_callback)(const void* data, int bytes, int rate, int ch)`.
//
// The function pointers are injected rather than linked so the session logic is
// contract-tested without the DLL and without a microphone, and so a missing DLL is
// an ordinary failure instead of a load-time crash.

#include "domain/error.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace voicetyper::platform {

// The platform layer names the portable domain types directly (the same usings as
// platform/windows/wasapi_capture.hpp).
using domain::ErrorCode;
using domain::Status;

/// The two exported functions, as pointer types.
struct McWasapiApi {
    using Callback = void (*)(const void* data, int bytes, int rate, int channels);

    int (*start)(Callback callback, int rate, int channels) = nullptr;
    void (*stop)() = nullptr;

    [[nodiscard]] bool available() const noexcept { return start != nullptr && stop != nullptr; }
};

/// The library as it is found next to the executable, or an unavailable api on a
/// platform without it and when the file is missing.
[[nodiscard]] McWasapiApi load_mc_wasapi();

/// What a non-zero mc_start result means. The library returns its own codes; the
/// text is what reaches the overlay, so it has to be readable.
[[nodiscard]] std::string mc_wasapi_failure_detail(int code);

/// One capture session.
///
/// The library has no user-data parameter for its callback, so at most one session
/// can be live at a time - the same global contract as mc_start/mc_stop. The session
/// announces that in its own words: a second start() while one is running fails with
/// invalid_state instead of hijacking the callback.
class McWasapiSession {
public:
    /// data, bytes, rate, channels. The pointer is only valid inside the call.
    using Sink = std::function<void(const void* data, int bytes, int rate, int channels)>;

    McWasapiSession(McWasapiApi api, std::uint32_t rate, std::uint16_t channels);
    ~McWasapiSession();

    McWasapiSession(const McWasapiSession&) = delete;
    McWasapiSession& operator=(const McWasapiSession&) = delete;
    McWasapiSession(McWasapiSession&&) = delete;
    McWasapiSession& operator=(McWasapiSession&&) = delete;

    /// Opens the capture. Failure codes: unavailable (no library), invalid_state
    /// (another session is running), resource_exhausted (mc_start returned a code),
    /// internal (no sink).
    [[nodiscard]] Status start(Sink sink);

    /// Stops the capture. Idempotent: the library's stop is called exactly once per
    /// successful start, and never when the start failed.
    void stop();

    [[nodiscard]] bool running() const noexcept;
    /// The last non-zero mc_start result, or zero.
    [[nodiscard]] int last_code() const noexcept;

    /// Called by the library's own thread. Buffers of no bytes are ignored, and so is
    /// anything that arrives outside a running session.
    void deliver(const void* data, int bytes, int rate, int channels);

private:
    McWasapiApi api_;
    std::uint32_t rate_;
    std::uint16_t channels_;
    Sink sink_;
    bool running_ = false;
    int last_code_ = 0;
};

} // namespace voicetyper::platform
