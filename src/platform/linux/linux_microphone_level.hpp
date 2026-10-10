#pragma once

// Linux implementation of the frozen platform::MicrophoneLevelPort contract.
//
// Evidence and contract:
//   * src/platform/api/microphone_level.hpp - "the same value the Sound control
//     panel edits, so the slider and the platform stay in step", unavailable
//     where the platform has no control, and read/write called from the UI
//     thread.
//   * The Windows backend uses IAudioEndpointVolume on the default capture
//     endpoint; the Linux equivalent is the volume and mute of the default
//     PulseAudio/PipeWire *source*, which is what pavucontrol edits.
//
// Mapping: the sound server stores a cubic volume; the linear scalar is
// obtained with pa_sw_volume_to_linear() and written back with
// pa_sw_volume_from_linear(), so 100 % on the slider is 100 % in pavucontrol.
//
// Thread affinity: read/write block on the sound server for a bounded time and
// are never called from an audio callback.

#include "platform/api/microphone_level.hpp"

#include <memory>
#include <string>

namespace voicetyper::platform::linuxos {

using platform::MicrophoneLevelPort;
using platform::MicrophoneLevelState;

class LinuxMicrophoneLevelPort final : public MicrophoneLevelPort {
public:
    LinuxMicrophoneLevelPort();
    ~LinuxMicrophoneLevelPort() override;

    LinuxMicrophoneLevelPort(const LinuxMicrophoneLevelPort&) = delete;
    LinuxMicrophoneLevelPort& operator=(const LinuxMicrophoneLevelPort&) = delete;
    LinuxMicrophoneLevelPort(LinuxMicrophoneLevelPort&&) = delete;
    LinuxMicrophoneLevelPort& operator=(LinuxMicrophoneLevelPort&&) = delete;

    /// The default source's volume and mute state. `available == false` means
    /// the session has no sound server or no source: the caller disables the
    /// slider rather than showing a zero.
    [[nodiscard]] MicrophoneLevelState read() override;

    /// Applies the level (clamped to 0..1) and the mute flag to the default
    /// source. Failure: unavailable when there is no control to write to.
    [[nodiscard]] domain::Status write(double level, bool muted) override;

    /// Why the last read or write failed, for the log.
    [[nodiscard]] std::string diagnostics() const;

private:
    mutable std::string diagnostics_;
};

/// The platform's level control, or nullptr where the platform has none.
[[nodiscard]] std::unique_ptr<MicrophoneLevelPort> create_linux_microphone_level();

} // namespace voicetyper::platform::linuxos
