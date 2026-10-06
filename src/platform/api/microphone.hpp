#pragma once

// Microphone enumeration contract.
//
// Evidence: docs/migration/cpp/feature-parity.md rows "Windows microphone" and
// "Recording start gate"; .NET reference
// VoiceTyper.Core/Services/MicrophoneService.cs (MMDeviceEnumerator over
// DataFlow.Capture, DeviceState.Active).
//
// Frozen observable contract:
//   * Enumeration returns *active capture* endpoints only, as (id, name) pairs.
//   * The id is an opaque backend string that is persisted in
//     settings.microphoneDeviceId and handed back to AudioCapture. An empty
//     stored value means "system default device".
//
// Intent-parity decision recorded here, because the .NET build has a known gap
// (feature-parity.md "Defects and explicit decisions before cutover", item 1):
// the current SettingsViewModel never loads MicrophoneDeviceId back into the UI,
// so a selected device is silently lost. The C++ contract therefore *requires*
// the id to round-trip through settings and to be handed to the capture start
// request; the behavior implementation and its regression test are Phase B/D
// work. This header does not decide which backends honour the id.
//
// Thread affinity: safe from any thread. Enumeration may be slow on some
// backends, so callers should not run it on the UI thread per keystroke.
//
// Ownership: the returned device list is owned by the caller; ids and names are
// copied strings.

#include "domain/audio_format.hpp"
#include "domain/error.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace voicetyper::platform {

using domain::AudioFormat;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// One capture endpoint.
struct MicrophoneDevice {
    /// Opaque backend id, persisted in settings and passed to AudioCapture.
    std::string id;
    /// Human-readable name shown in the settings dropdown.
    std::string name;
    /// True for the system default endpoint. At most one device has this set.
    bool is_default = false;
    /// Format the device currently offers, when the backend could determine it.
    /// An invalid AudioFormat (rate 0) means "not probed yet".
    AudioFormat native_format;
};

/// Why a device became unusable, reported by change notifications.
enum class MicrophoneChange : std::uint8_t {
    /// A device was connected or became active.
    added = 0,
    /// A device was removed or disabled.
    removed = 1,
    /// The active default device changed.
    default_changed = 2,
    /// The device list could not be enumerated; the previous list is kept.
    enumeration_failed = 3,
};

[[nodiscard]] constexpr std::string_view microphone_change_name(MicrophoneChange change) noexcept
{
    switch (change) {
    case MicrophoneChange::added: return "added";
    case MicrophoneChange::removed: return "removed";
    case MicrophoneChange::default_changed: return "default_changed";
    case MicrophoneChange::enumeration_failed: return "enumeration_failed";
    }
    return "unknown";
}

class MicrophoneService {
public:
    virtual ~MicrophoneService() = default;

    MicrophoneService(const MicrophoneService&) = delete;
    MicrophoneService& operator=(const MicrophoneService&) = delete;
    MicrophoneService(MicrophoneService&&) = delete;
    MicrophoneService& operator=(MicrophoneService&&) = delete;

    /// Lists active capture endpoints.
    ///
    /// Failure codes: unavailable (no capture backend / enumeration refused),
    /// permission_denied (OS privacy settings block device names).
    /// An empty list is a *success*: the user may genuinely have no microphone,
    /// and the settings page must show that as "no devices", not as an error.
    [[nodiscard]] virtual std::vector<MicrophoneDevice> list_devices() = 0;

    /// The id of the system default capture endpoint, empty when unknown.
    [[nodiscard]] virtual std::string default_device_id() = 0;

    /// Whether `device_id` is still present and usable. Used before a capture
    /// start to produce a precise "device disappeared" diagnostic instead of a
    /// generic capture failure.
    [[nodiscard]] virtual bool is_device_available(const std::string& device_id) = 0;

protected:
    MicrophoneService() = default;
};

} // namespace voicetyper::platform
