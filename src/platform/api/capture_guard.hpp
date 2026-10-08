#pragma once

// A best-effort "fuse" for the microphone.
//
// Why: a capture client left behind by a dead process keeps the recording endpoint
// allocated INSIDE THE AUDIO DRIVER, and then the microphone is unusable for every
// application on the machine, not just this one. Measured on the target machine on
// 2026-10-06: MME answered MMSYSERR_ALLOCATED (11), WASAPI answered E_INVALIDARG
// (0x80070057) for every format and endpoint, mc_wasapi.dll opened the device and
// delivered zero buffers, and the installed .NET build's own microphone test reported
// "no data" - while the microphone was unmuted, the privacy setting allowed desktop
// apps and no application reported using it.
//
// What this class guarantees: a registered release runs at most once, and only while
// it is armed - from release_now(), or from the crash hook the platform installs.
// What it cannot do: survive TerminateProcess (Task Manager "end task"), a kill from
// outside or a power loss; no in-process code can intercept those. That is why the
// adapter ALSO releases the client as soon as it is idle (see
// platform/windows/windows_audio_capture.hpp) and why a wedged device is reported to
// the user instead of being retried forever.

#include <atomic>
#include <functional>
#include <mutex>

namespace voicetyper::platform {

class CaptureGuard {
public:
    CaptureGuard() = default;
    CaptureGuard(const CaptureGuard&) = delete;
    CaptureGuard& operator=(const CaptureGuard&) = delete;

    /// Registers the release for the session that is about to start.
    ///
    /// The callback runs from a crash path when the process dies, so it must not
    /// throw; releasing the device implies taking locks the application already takes
    /// on its normal path, and that is accepted here deliberately: a best-effort
    /// release in a dying process beats leaving the microphone allocated for hours.
    void arm(std::function<void()> release);

    /// Forgets the release without running it: the session ended on its own.
    void disarm();

    /// Runs the armed release exactly once and disarms. Safe to call from a crash
    /// hook: no mutex is taken on this path.
    void release_now();

    [[nodiscard]] bool armed() const noexcept;

private:
    std::atomic<bool> armed_{false};
    std::function<void()> release_;
    std::mutex mutex_;
};

/// Installs the platform hooks that call release_now() when the process dies from an
/// unhandled exception, an abort or a fatal signal. Idempotent; on a platform without
/// such hooks it does nothing and the guard still works through release_now().
void install_crash_release_hook(CaptureGuard& guard);

/// Appends a symbolised stack of the CALLING thread to wrong-thread-<pid>.txt in the log
/// directory. For the case where the process is healthy but something is being done from
/// the wrong thread: Qt's own warning names the symptom and this names the culprit.
/// A no-op on a platform without dbghelp.
void log_stack_trace(const char* reason);

} // namespace voicetyper::platform
