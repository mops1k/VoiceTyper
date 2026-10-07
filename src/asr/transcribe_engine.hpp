#pragma once

// Abstract seam over the pinned transcribe.cpp runtime (GGUF engines: GigaAM).
//
// Why an interface instead of a direct binding: the policy that matters most
// here is OURS, not the library's - "a dictation longer than the model window is
// cut at a pause and the parts are joined, and audio is never silently dropped".
// That policy is contract-tested on every host against this seam, while the
// DLL-backed implementation lives in src/platform/windows/transcribe_runtime.cpp
// and is only exercised on Windows. Same split as the engine registry: policy
// portable, dependency behind it.
//
// Evidence: docs/migration/cpp/native-dependencies.json (pin, license, ABI),
// native/transcribe/transcribe.h (the C ABI of the pin, versioned next to the DLLs),
// and the spike measurements recorded in the plan p_72adce780b6c.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::asr {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// Exact library version the shipped runtime must report (TRANSCRIBE_VERSION of
/// the pinned build). The ABI is explicitly not stable below 1.0, so an exact
/// match is the guard; a mismatch is an error, never a silent downgrade.
inline constexpr std::string_view kTranscribeExpectedVersion = "0.3.1";
/// Pinned upstream commit of the shipped build (tag v0.3.1).
inline constexpr std::string_view kTranscribePinnedCommit = "3f32fbcc7bb3246851a0234263438bc3c0fa1cac";
/// The shipped runtime lives in its OWN directory beside the executable, not in
/// the executable's directory: it is built with a different MinGW generation than
/// the application, so its libstdc++/libgcc/ggml siblings must not be confused
/// with the application's. The loader is given that directory explicitly.
inline constexpr std::string_view kTranscribeLibraryDirectory = "transcribe";
/// File name of the shipped runtime library inside that directory.
inline constexpr std::string_view kTranscribeLibraryName = "libtranscribe.dll";
inline constexpr std::string_view kTranscribeLicense = "MIT";
inline constexpr std::string_view kTranscribeRepository = "https://github.com/handy-computer/transcribe.cpp";

/// The complete, closed list of symbols this product binds. Resolving more would
/// widen the ABI surface for nothing; a missing one is an explicit
/// unavailable/abi error, never a fallback to another engine.
///
/// The four `*_init` entries are bound on purpose: the library rejects a struct
/// whose `struct_size` was not stamped by its own init function, so calling them
/// is the documented way to pass parameters across this ABI.
inline constexpr std::array<std::string_view, 14> kTranscribeRequiredSymbols = {
    "transcribe_version",
    "transcribe_abi_struct_size",
    "transcribe_model_load_params_init",
    "transcribe_session_params_init",
    "transcribe_run_params_init",
    "transcribe_capabilities_init",
    "transcribe_open",
    "transcribe_session_free",
    "transcribe_get_model",
    "transcribe_model_get_capabilities",
    "transcribe_set_abort_callback",
    "transcribe_was_aborted",
    "transcribe_run",
    "transcribe_full_text",
};

/// Result of inspecting the shipped library without loading a model.
struct TranscribeProbe {
    /// True when the module was opened and every required symbol resolved.
    bool library_loaded = false;
    /// Value returned by transcribe_version(); empty when it was not called.
    std::string version;
    /// Value returned by transcribe_version_commit() when bound; may be
    /// "unknown" for a build from a source archive without git metadata.
    std::string version_commit;
    /// True only for a library whose version and struct sizes match the pin.
    bool usable = false;
    /// Human-readable cause, never a bare "unavailable".
    std::string detail;
    std::vector<std::string> missing_symbols;
    std::filesystem::path library_path;
};

/// Opens `dll_path`, resolves the closed symbol list, compares the reported
/// version with kTranscribeExpectedVersion and re-checks the public struct sizes
/// the binding actually passes across the ABI. Never loads a model, so it is
/// safe from a UI poll or a test.
[[nodiscard]] TranscribeProbe probe_transcribe_runtime(const std::filesystem::path& dll_path);

/// Full path of the runtime library that belongs to the running executable.
/// Returns an empty path off Windows.
[[nodiscard]] std::filesystem::path transcribe_library_beside_executable();

/// One loaded GGUF model with one session attached.
///
/// Threading: the underlying session is single-threaded-at-a-time (the library
/// documents that sessions are not thread-safe), so the owner serializes calls -
/// the transcriber holds one mutex, exactly like the other engines.
class TranscribeEngine {
public:
    virtual ~TranscribeEngine() = default;

    TranscribeEngine(const TranscribeEngine&) = delete;
    TranscribeEngine& operator=(const TranscribeEngine&) = delete;
    TranscribeEngine(TranscribeEngine&&) = delete;
    TranscribeEngine& operator=(TranscribeEngine&&) = delete;

    [[nodiscard]] virtual bool is_ready() const noexcept = 0;

    /// Loads the GGUF model and opens the session, with the CPU backend pinned
    /// explicitly: this product is an offline CPU recognizer, so an accidental
    /// GPU/Vulkan pick on a machine with a driver present is not acceptable.
    /// Failure: not_found, corrupt_data, unsupported, unavailable, cancelled.
    [[nodiscard]] virtual Status load_model(
        const std::filesystem::path& gguf_path, const CancellationToken& cancellation) = 0;

    /// Transcribes one complete 16 kHz mono float32 utterance.
    ///
    /// Cancellation is cooperative through the library's abort callback, which
    /// is polled between decode steps; the caller observes `cancelled` and the
    /// partial result is discarded.
    ///
    /// Failure: model_not_ready, out_of_range (audio beyond the model window),
    /// cancelled, unavailable.
    [[nodiscard]] virtual Result<std::string> transcribe(
        const std::vector<float>& samples, const CancellationToken& cancellation) = 0;

    /// Releases the model and the session. Idempotent, also run by the destructor.
    virtual void free_model() noexcept = 0;

    /// Human-readable last error of the library, or "" when there is none.
    [[nodiscard]] virtual std::string last_error() const = 0;

    /// Input window the MODEL declares, in seconds, read from its own
    /// capabilities (transcribe_capabilities::max_audio_ms). 0 means the model
    /// has no practical limit. The product never hardcodes this number, so a
    /// different GGUF cannot silently be cut with someone else's window.
    [[nodiscard]] virtual double max_audio_seconds() const = 0;

protected:
    TranscribeEngine() = default;
};

/// Opens the library and returns a ready-to-load engine.
///
/// Failure codes: unavailable (library missing or unloadable), unsupported
/// (wrong version, struct-size mismatch, or a non-Windows host), cancelled.
[[nodiscard]] Result<std::unique_ptr<TranscribeEngine>> open_transcribe_engine(
    const std::filesystem::path& dll_path,
    int threads,
    const CancellationToken& cancellation);

} // namespace voicetyper::asr
