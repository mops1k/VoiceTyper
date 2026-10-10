#pragma once

// Small shared helper for the libpulse-based Linux audio backends.
//
// Every backend needs the same three things: a main loop, a context connected to
// the sound server, and a bounded "iterate until this operation is done" call.
// Duplicating that in three files is how the three of them end up with three
// different timeouts; it lives here once.
//
// The helper is deliberately synchronous: enumeration, a volume read and a
// volume write all happen on the application thread and must not need an event
// loop of their own. Capture uses pa_simple instead (see linux_audio_capture.cpp)
// and does not go through this file.

#include <pulse/pulseaudio.h>

#include <chrono>
#include <string>

namespace voicetyper::platform::linuxos {

/// A connected PulseAudio/PipeWire context with its own main loop.
///
/// Lifetime: connect() may fail, and every use must check connected() first.
/// Destroying the object disconnects and frees both.
class PulseConnection {
public:
    PulseConnection();
    ~PulseConnection();

    PulseConnection(const PulseConnection&) = delete;
    PulseConnection& operator=(const PulseConnection&) = delete;

    /// Connects and waits up to `timeout` for PA_CONTEXT_READY. Returns false and
    /// fills `error` with the sound server's own message otherwise.
    bool connect(std::chrono::milliseconds timeout, std::string& error);

    /// Iterates the main loop until `operation` is done, or `timeout` expires.
    /// Returns false on timeout, on a failed operation, or when there is no
    /// operation at all.
    bool wait(pa_operation* operation, std::chrono::milliseconds timeout, std::string& error);

    /// Iterates the loop once for up to `timeout`.
    void iterate(std::chrono::milliseconds timeout);

    [[nodiscard]] bool connected() const noexcept { return context_ != nullptr && ready_; }
    [[nodiscard]] pa_context* context() const noexcept { return context_; }

    /// The name of the default source, empty when the server did not report one.
    [[nodiscard]] std::string default_source_name(std::chrono::milliseconds timeout, std::string& error);

    /// The server's own error text for the last failed call.
    [[nodiscard]] static std::string last_error(pa_context* context);

private:
    pa_mainloop* loop_ = nullptr;
    pa_context* context_ = nullptr;
    bool ready_ = false;
};

/// The name the sound server knows this client by; it shows up in pavucontrol
/// and in the server log, so it is a constant rather than a literal.
inline constexpr const char* kPulseClientName = "VoiceTyper";

} // namespace voicetyper::platform::linuxos
