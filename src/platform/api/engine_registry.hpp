#pragma once

// Transcription engine registry contract.
//
// Evidence: docs/migration/cpp/feature-parity.md row "Parakeet" ("no silent
// Whisper fallback") and the acceptance note "ABI check, load/free, real model
// smoke, no silent Whisper fallback"; .NET reference
// VoiceTyper.Core/Services/Transcription/EngineManager.cs and ParakeetNative.cs.
//
// The central rule of this interface:
//   * Asking for an engine that cannot be used MUST fail with
//     ErrorCode::engine_unavailable. The registry never returns a different
//     engine, never degrades to Whisper because Parakeet's native library is
//     missing, and never reports success for an engine it did not create.
//   * Availability is a property of the machine, not of the user's last choice:
//     it is queried explicitly so the UI can show a precise "not available"
//     state instead of silently changing the engine.
//
// ABI: the pinned Parakeet native build is commit
// e75de9b6b9b688fd293aa22f7e27aa724ea286f8 with C header ABI version 6. The
// registry reports the observed version so a mismatch is diagnosable; a
// mismatch is an error, never a fallback.
//
// Ownership: the registry owns the engine *factories*, not the engines.
// create() returns a uniquely owned Transcriber whose lifetime the caller
// controls; dropping it must unload the model and release the native context.
//
// Thread affinity: is_available() and abi_version() are safe from any thread.
// create() may load a model, so it must be called from a worker thread.

#include "domain/error.hpp"
#include "domain/settings.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include "platform/api/transcriber.hpp"

namespace voicetyper::platform {

using domain::ErrorCode;
using domain::Result;
using domain::Status;
using domain::TranscriptionEngine;

/// C header ABI version of the pinned parakeet.cpp build this project targets.
inline constexpr int kParakeetAbiVersion = 6;

/// Pinned upstream commit of the parakeet.cpp build, for diagnostics.
inline constexpr std::string_view kParakeetPinnedCommit = "e75de9b6b9b688fd293aa22f7e27aa724ea286f8";

/// Native library filename that must sit beside the executable for Parakeet.
inline constexpr std::string_view kParakeetLibraryName = "parakeet.dll";
/// Native library filename of the preferred WASAPI capture backend.
inline constexpr std::string_view kNativeWasapiLibraryName = "mc_wasapi.dll";

/// Why an engine is or is not usable. Reported so the UI can show a specific
/// state rather than a generic failure.
enum class EngineAvailabilityReason : std::uint8_t {
    /// The engine can be created.
    available = 0,
    /// The native library was not found beside the executable.
    native_library_missing = 1,
    /// The native library loaded but reports a different ABI version.
    abi_mismatch = 2,
    /// The model file is absent.
    model_missing = 3,
    /// The platform has no support for this engine at all.
    platform_unsupported = 4,
};

[[nodiscard]] constexpr std::string_view engine_availability_reason_name(EngineAvailabilityReason reason) noexcept
{
    switch (reason) {
    case EngineAvailabilityReason::available: return "available";
    case EngineAvailabilityReason::native_library_missing: return "native_library_missing";
    case EngineAvailabilityReason::abi_mismatch: return "abi_mismatch";
    case EngineAvailabilityReason::model_missing: return "model_missing";
    case EngineAvailabilityReason::platform_unsupported: return "platform_unsupported";
    }
    return "unknown";
}

/// Availability of one engine on this machine.
struct EngineAvailability {
    TranscriptionEngine engine = TranscriptionEngine::whisper;
    bool available = false;
    EngineAvailabilityReason reason = EngineAvailabilityReason::available;
    /// Observed native ABI version, when a library was found and loaded.
    std::optional<int> abi_version;
};

/// Creates Transcriber instances for the engines this build supports.
class EngineRegistry {
public:
    virtual ~EngineRegistry() = default;

    EngineRegistry(const EngineRegistry&) = delete;
    EngineRegistry& operator=(const EngineRegistry&) = delete;
    EngineRegistry(EngineRegistry&&) = delete;
    EngineRegistry& operator=(EngineRegistry&&) = delete;

    /// Reports whether `engine` can be used right now. Cheap and side-effect
    /// free apart from probing the native library, so the UI may poll it.
    [[nodiscard]] virtual EngineAvailability availability(TranscriptionEngine engine) const = 0;

    /// Creates an engine bound to `model_path`.
    ///
    /// Failure codes:
    ///   engine_unavailable - the engine cannot run here (missing library, ABI
    ///                       mismatch, unsupported platform). The caller must
    ///                       surface this to the user.
    ///   not_found          - the model file does not exist.
    ///   model_not_ready    - the model exists but could not be loaded.
    /// There is deliberately no path that returns a different engine.
    [[nodiscard]] virtual Result<std::unique_ptr<Transcriber>> create(
        TranscriptionEngine engine, std::filesystem::path model_path) = 0;

    /// Records which engine the user selected, so the next availability query can
    /// report a precise model_missing state. Does not create anything.
    virtual Status set_model_path(TranscriptionEngine engine, std::filesystem::path model_path) = 0;

    /// Model file registered for `engine`, if any.
    [[nodiscard]] virtual std::optional<std::filesystem::path> model_path(TranscriptionEngine engine) const = 0;

protected:
    EngineRegistry() = default;
};

} // namespace voicetyper::platform
