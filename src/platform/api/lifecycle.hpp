#pragma once

// Process lifecycle contract: single instance, autostart and shutdown ordering.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §2 "Windows paths and
// lifecycle" and docs/migration/cpp/feature-parity.md rows "Single instance",
// "Application shell" and "Startup"; .NET reference
// VoiceTyper.App/App.axaml.cs and VoiceTyper.App/Services/StartupManager.cs.
//
// Frozen observable contract:
//   * The single-instance lock is the global named object
//     "Global\VoiceTyper_SingleInstance", the same name the .NET build uses, so
//     the two cannot run at the same time and write the same settings file.
//   * A second instance shows a dialog and exits. There is no activation IPC:
//     the first instance is not brought to the front, and this contract does not
//     invent one.
//   * Closing the settings window hides it; the process stays in the tray. An
//     explicit quit is the only path that ends the process, and it must not
//     leave an orphan process or a held global lock.
//   * Autostart writes the HKCU Run value "VoiceTyper" with the current
//     executable path in quotes. Autostart failures are diagnostic only: the
//     application must start anyway, because a packaged build may have no
//     writable Run key.
//   * startMinimized creates the window without showing it.
//
// Intent-parity decisions recorded here:
//   * The .NET build swallows autostart errors completely. The C++ contract
//     returns a Status so the failure is logged and can be surfaced, while the
//     caller still continues startup. That is a deliberate change from a
//     no-op gap and needs a Phase D test.
//   * The .NET build's `IsRunAtStartupEnabled()` only checks that the value
//     exists and is not wired to the UI. This contract reports the same fact and
//     explicitly does not claim it reflects the running process.
//
// Shutdown ordering is a contract because getting it wrong corrupts data:
//   cancel recording -> stop capture -> dispose transcriber -> dispose UI.
// The coordinator makes the order explicit rather than leaving it to destructor
// order.
//
// Thread affinity: acquire_single_instance() is called once on the startup
// thread before any window exists. set_run_at_startup() and the query are
// callable from the UI thread. begin_shutdown() must be callable from the UI
// thread and blocks until the coordinator has finished.
//
// Ownership: the guard returned by acquire_single_instance() owns the lock for
// its lifetime. Destroying it releases the lock. A failed acquisition means
// "another instance owns it" and the caller must exit.

#include "domain/error.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::platform {

using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// The step currently being executed during shutdown, in the required order.
enum class ShutdownStep : std::uint8_t {
    /// Signal the recording state machine to stop and discard audio.
    cancel_recording = 0,
    /// Stop the capture backend and release the device.
    stop_capture = 1,
    /// Release the loaded engine/model.
    dispose_transcriber = 2,
    /// Release hotkeys, gamepad polling, tray and overlay.
    dispose_input_and_ui = 3,
    /// Release the single-instance lock last.
    release_single_instance = 4,
};

[[nodiscard]] constexpr std::string_view shutdown_step_name(ShutdownStep step) noexcept
{
    switch (step) {
    case ShutdownStep::cancel_recording: return "cancel_recording";
    case ShutdownStep::stop_capture: return "stop_capture";
    case ShutdownStep::dispose_transcriber: return "dispose_transcriber";
    case ShutdownStep::dispose_input_and_ui: return "dispose_input_and_ui";
    case ShutdownStep::release_single_instance: return "release_single_instance";
    }
    return "unknown";
}

/// The full shutdown sequence, in the order it must run.
inline constexpr std::array<ShutdownStep, 5> kShutdownOrder{
    ShutdownStep::cancel_recording,
    ShutdownStep::stop_capture,
    ShutdownStep::dispose_transcriber,
    ShutdownStep::dispose_input_and_ui,
    ShutdownStep::release_single_instance,
};

/// Owns the cross-process single-instance lock for as long as it is alive.
/// Move-only: exactly one guard may own the lock.
class SingleInstanceGuard {
public:
    SingleInstanceGuard() = default;
    ~SingleInstanceGuard();

    SingleInstanceGuard(SingleInstanceGuard&& other) noexcept;
    SingleInstanceGuard& operator=(SingleInstanceGuard&& other) noexcept;

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    [[nodiscard]] bool owns_lock() const noexcept { return owned_; }
    void release() noexcept;

private:
    friend class LifecycleService;
    explicit SingleInstanceGuard(bool owned)
        : owned_(owned)
    {
    }

    bool owned_ = false;
};

/// Runs one shutdown step. Returning an error does not abort the sequence: a
/// failed capture stop must still be followed by releasing the global lock.
using ShutdownStepHandler = std::function<Status(ShutdownStep)>;

class LifecycleService {
public:
    virtual ~LifecycleService() = default;

    LifecycleService(const LifecycleService&) = delete;
    LifecycleService& operator=(const LifecycleService&) = delete;
    LifecycleService(LifecycleService&&) = delete;
    LifecycleService& operator=(LifecycleService&&) = delete;

    /// Tries to take the cross-process lock.
    ///
    /// Success -> a guard that owns the lock. Failure with already_exists ->
    /// another instance is running and this process must show its dialog and
    /// exit. Failure with unavailable -> the platform has no such mechanism.
    [[nodiscard]] virtual Result<SingleInstanceGuard> acquire_single_instance() = 0;

    /// Enables or disables autostart.
    ///
    /// Failure codes: unsupported (no autostart mechanism on this platform),
    /// permission_denied, io_failure. Callers must log the failure and continue;
    /// autostart is never a reason to refuse to start.
    virtual Status set_run_at_startup(bool enabled) = 0;

    /// Whether the autostart value exists. This only reports the value's
    /// presence, not whether the process is actually running from it.
    [[nodiscard]] virtual bool is_run_at_startup_enabled() = 0;

    /// The command line the autostart entry would run, i.e. the quoted current
    /// executable path. Exposed so the UI can show exactly what will be stored.
    [[nodiscard]] virtual std::string autostart_command_line() = 0;

    /// Runs kShutdownOrder through `handler` and collects the per-step results.
    /// Always attempts every step, even after a failure, so a stuck capture
    /// cannot leave the global lock held.
    [[nodiscard]] virtual Result<std::vector<ShutdownStep>> shutdown(const ShutdownStepHandler& handler) = 0;

protected:
    LifecycleService() = default;
};

} // namespace voicetyper::platform
