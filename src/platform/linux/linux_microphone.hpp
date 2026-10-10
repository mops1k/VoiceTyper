#pragma once

// Linux implementation of the frozen platform::MicrophoneService contract.
//
// Evidence and contract:
//   * src/platform/api/microphone.hpp - active capture endpoints only, an opaque
//     id that round-trips through settings and is handed back to the capture
//     backend, at most one device flagged as the default, and "an empty list is
//     a success".
//   * docs/migration/cpp/release-gates.md risk R9 - "Prefer the native audio
//     API, bounded queue, device-change tests, exact diagnostics": the sound
//     server (PipeWire through pipewire-pulse on the target machine, or a real
//     PulseAudio) is queried directly, not through a helper binary.
//
// What is enumerated: PulseAudio/PipeWire *sources* that are not monitors. A
// monitor source is a loopback of an output device and would otherwise show up
// in the microphone dropdown as a device that hears the speakers.
//
// The id is the source name (`alsa_input.pci-0000_00_1f.3.analog-stereo`), which
// is stable across restarts and is exactly what the capture backend passes to
// the sound server. The empty id means "the system default source".
//
// Thread affinity: enumeration blocks on the sound server for a bounded time, so
// callers should not run it on the UI thread per keystroke; it is safe from any
// thread.
//
// Ownership: the returned list is owned by the caller; ids and names are copied
// strings.

#include "platform/api/microphone.hpp"

#include <string>
#include <vector>

namespace voicetyper::platform::linuxos {

using platform::MicrophoneDevice;

class LinuxMicrophoneService final : public platform::MicrophoneService {
public:
    LinuxMicrophoneService();
    ~LinuxMicrophoneService() override;

    LinuxMicrophoneService(const LinuxMicrophoneService&) = delete;
    LinuxMicrophoneService& operator=(const LinuxMicrophoneService&) = delete;
    LinuxMicrophoneService(LinuxMicrophoneService&&) = delete;
    LinuxMicrophoneService& operator=(LinuxMicrophoneService&&) = delete;

    /// Lists active, non-monitor capture sources. An empty list is a success.
    [[nodiscard]] std::vector<MicrophoneDevice> list_devices() override;

    /// Name of the default source, empty when the server did not report one.
    [[nodiscard]] std::string default_device_id() override;

    /// True when the source is in the current list.
    [[nodiscard]] bool is_device_available(const std::string& device_id) override;

    /// Why the last enumeration returned nothing (no sound server, refused
    /// connection, timeout). Empty after a successful enumeration, so
    /// "no microphone attached" can be told from "this session cannot see one".
    [[nodiscard]] std::string diagnostics() const;

private:
    mutable std::string diagnostics_;
};

} // namespace voicetyper::platform::linuxos
