#pragma once

// Parakeet native runtime: the only place that talks to the shipped
// parakeet.dll.
//
// Evidence and contract:
//   * VoiceTyper.App/Native/parakeet/include/parakeet_capi.h - the C ABI,
//     version 6, flat C entry points, malloc'd UTF-8 strings owned by the
//     caller, last-error string owned by the context.
//   * VoiceTyper.Core/Interop/ParakeetNative.cs - the .NET P/Invoke surface.
//   * src/platform/api/engine_registry.hpp - ABI 6 and the pinned commit
//     e75de9b6b9b688fd293aa22f7e27aa724ea286f8 are frozen there; the loader
//     asserts the same values.
//   * docs/migration/cpp/native-dependencies.json - repository, pin, license,
//     SHA-256 and the exact list of symbols that may be bound.
//
// Why a runtime loader instead of a link-time dependency: parakeet.dll is an
// optional, Windows-only engine. Resolving it explicitly is what lets the
// product report "Parakeet is unavailable here" instead of failing to start or
// silently running a different engine. There is no Whisper fallback here, and
// no path that returns a transcript from another model.
//
// Platform boundary: this is Windows platform code, but the header stays
// standard-C++20 and free of <windows.h>, so contract tests can build and run
// the "library absent" and "cannot be loaded on this platform" cases on any
// host. The Win32 calls live in parakeet_runtime.cpp.
//
// Deliberately *not* bound: every other parakeet_capi_* entry point (streaming,
// batch, JSON/timestamps, nbest, logits, events). The product transcribes a
// complete final clip; binding more would widen the ABI surface for nothing.
// In particular the C++ `parakeet` target that whisper.cpp's own repository
// happens to build is NOT a supported surface: it has no ABI versioning and is
// unrelated to the shipped DLL.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "platform/api/engine_registry.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::platform {

using domain::CancellationToken;
using domain::Error;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// Decoder selector values of the pinned ABI (parakeet_capi.h).
inline constexpr int kParakeetDecoderDefault = 0;
inline constexpr int kParakeetDecoderCtc = 1;
inline constexpr int kParakeetDecoderTdt = 2;

/// The complete, closed list of symbols this product may bind. Resolving
/// anything else is a bug; a missing one is an explicit error.
inline constexpr std::array<std::string_view, 6> kParakeetRequiredSymbols = {
    "parakeet_capi_abi_version",
    "parakeet_capi_load",
    "parakeet_capi_free",
    "parakeet_capi_transcribe_pcm_lang",
    "parakeet_capi_free_string",
    "parakeet_capi_last_error",
};

/// Result of inspecting a parakeet.dll without loading a model.
///
/// `reason` is the coarse bucket the UI shows; `detail` carries the exact
/// cause (path, Win32 error code, observed ABI, missing symbol names), so a
/// failure is never reported as a bare "unavailable".
struct ParakeetProbe {
    /// True when the library was opened and every required symbol resolved.
    bool library_loaded = false;
    /// Value returned by parakeet_capi_abi_version(); 0 when it was not called.
    int abi_version = 0;
    /// True only for a usable library reporting the required ABI version.
    bool usable = false;
    EngineAvailabilityReason reason = EngineAvailabilityReason::native_library_missing;
    std::string detail;
    std::vector<std::string> missing_symbols;
    std::filesystem::path library_path;
};

/// Loads `dll_path`, resolves the six required symbols, and asserts ABI
/// version kParakeetAbiVersion (6). Never throws and never loads a model, so
/// it is safe to call from a test or a UI poll.
///
/// Reason mapping, in the order the checks run:
///   file missing / not a regular file -> native_library_missing
///   file present on a non-Windows host -> platform_unsupported
///   LoadLibraryW failed               -> native_library_missing (detail has the
///                                        Win32 error code, e.g. a missing UCRT
///                                        dependency of the DLL itself)
///   a required symbol is absent       -> abi_mismatch (the DLL is not the
///                                        pinned build)
///   abi_version() != 6                -> abi_mismatch (detail has both values)
[[nodiscard]] ParakeetProbe probe_parakeet_runtime(const std::filesystem::path& dll_path);

/// Full path of the parakeet.dll that belongs to the running executable, the
/// C++ equivalent of the .NET `AppContext.BaseDirectory` lookup - placed in
/// platform code on purpose, because that lookup is one of the OS-leakage sites
/// of the .NET Core service. Returns an empty path off Windows.
[[nodiscard]] std::filesystem::path parakeet_library_beside_executable();

/// An opened parakeet.dll with at most one loaded model.
///
/// Lifetime/ownership: open() resolves the symbols and keeps the module handle;
/// ~ParakeetRuntime frees the model and unloads the module. Inference is not
/// thread-safe inside the native context, so one instance is used from one
/// thread at a time.
///
/// Cancellation (contract decision D1): Parakeet inference is *not*
/// interruptible. The token is polled before the native call and after it, and
/// the maximum observed cancellation latency is therefore one inference; the
/// context is never killed from another thread.
class ParakeetRuntime {
public:
    ~ParakeetRuntime();

    ParakeetRuntime(const ParakeetRuntime&) = delete;
    ParakeetRuntime& operator=(const ParakeetRuntime&) = delete;
    ParakeetRuntime(ParakeetRuntime&&) = delete;
    ParakeetRuntime& operator=(ParakeetRuntime&&) = delete;

    /// Opens the DLL and asserts ABI 6.
    /// Failure: engine_unavailable (missing, unloadable, wrong ABI),
    /// cancelled.
    [[nodiscard]] static Result<std::unique_ptr<ParakeetRuntime>> open(
        const std::filesystem::path& dll_path, const CancellationToken& cancellation);

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_ready() const noexcept;
    [[nodiscard]] int abi_version() const noexcept;
    [[nodiscard]] const ParakeetProbe& probe() const noexcept;

    /// Loads a GGUF model without running inference (the .NET Warmup()).
    /// Failure: model_not_ready (no open runtime), not_found, corrupt_data,
    /// invalid_argument, cancelled, engine_unavailable.
    Status load_model(const std::filesystem::path& gguf_path, const CancellationToken& cancellation);

    /// Transcribes a complete mono float32 clip and returns the trimmed text.
    ///
    /// `target_lang` empty means "the model's own language" (the .NET
    /// `targetLang: ""` behaviour). parakeet_capi_transcribe_pcm_lang is the
    /// only transcribe entry point bound here: its header states that an empty
    /// locale is exactly what parakeet_capi_transcribe_pcm would pass, so the
    /// language-agnostic case is not a different code path.
    ///
    /// Failure: model_not_ready, invalid_argument, cancelled, engine_unavailable
    /// (with the native last error as the detail).
    [[nodiscard]] Result<std::string> transcribe_pcm(
        const float* samples,
        std::size_t sample_count,
        int sample_rate,
        int decoder,
        const std::string& target_lang,
        const CancellationToken& cancellation);

    /// Releases the model context. Idempotent, and also run by the destructor.
    void free_model() noexcept;

    /// Human-readable last error of the loaded model, or "" when there is none.
    [[nodiscard]] std::string last_error() const;

private:
    ParakeetRuntime();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voicetyper::platform
