#pragma once

// Concrete platform::EngineRegistry over the two native engines.
//
// Why the factories are injected: the registry's job is availability, model
// paths and the "never substitute another engine" rule. That rule is worth
// testing on every host, while whisper.cpp itself only exists in a build with
// VOICETYPER_BUILD_ASR=ON and parakeet.dll only exists on Windows. The
// composition root therefore supplies the two factory callables, and the
// registry stays portable, Qt-free and testable with fakes.

#include "domain/error.hpp"
#include "platform/api/engine_registry.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace voicetyper::asr {

/// Creates a Whisper engine for `model_path`.
using WhisperEngineFactory = std::function<
    domain::Result<std::unique_ptr<platform::Transcriber>>(const std::filesystem::path& model_path)>;

/// Creates a Parakeet engine from `dll_path` + `model_path`. The factory is
/// responsible for the ABI 6 assertion; the registry only reports what the probe
/// said before it was called.
using ParakeetEngineFactory = std::function<
    domain::Result<std::unique_ptr<platform::Transcriber>>(
        const std::filesystem::path& dll_path, const std::filesystem::path& model_path)>;

/// Probes the Parakeet DLL without loading a model. On a non-Windows host this
/// reports platform_unsupported, which is a real product state, not a stub.
using ParakeetProbeFn = std::function<platform::EngineAvailability(
    domain::TranscriptionEngine engine, const std::filesystem::path& model_path)>;

/// Creates a GigaAM engine from `dll_path` + `model_path`. The factory is
/// responsible for the version/struct-size assertion; the registry only reports
/// what the probe said before it was called. The speech segmenter a long
/// dictation needs is closed over by the composition's lambda, so the registry
/// interface stays free of it.
using GigaamEngineFactory = std::function<
    domain::Result<std::unique_ptr<platform::Transcriber>>(
        const std::filesystem::path& dll_path, const std::filesystem::path& model_path)>;

/// Probes the transcribe.cpp runtime library without loading a model.
using GigaamProbeFn = std::function<platform::EngineAvailability(
    domain::TranscriptionEngine engine, const std::filesystem::path& model_path)>;

struct NativeEngineRegistryOptions {
    WhisperEngineFactory whisper_factory;
    ParakeetEngineFactory parakeet_factory;
    ParakeetProbeFn parakeet_probe;
    GigaamEngineFactory gigaam_factory;
    GigaamProbeFn gigaam_probe;
    /// Full path of parakeet.dll, e.g. beside the executable.
    std::filesystem::path parakeet_library;
    /// Full path of the transcribe.cpp runtime (libtranscribe.dll), e.g. beside
    /// the executable.
    std::filesystem::path gigaam_library;
    /// True when this build links whisper.cpp. A GUI-off portable contract build
    /// has it off, and asking for Whisper then reports platform_unsupported.
    bool whisper_available = false;
};

class NativeEngineRegistry final : public platform::EngineRegistry {
public:
    explicit NativeEngineRegistry(NativeEngineRegistryOptions options);
    ~NativeEngineRegistry() override;

    [[nodiscard]] platform::EngineAvailability availability(
        domain::TranscriptionEngine engine) const override;
    [[nodiscard]] domain::Result<std::unique_ptr<platform::Transcriber>> create(
        domain::TranscriptionEngine engine, std::filesystem::path model_path) override;
    domain::Status set_model_path(
        domain::TranscriptionEngine engine, std::filesystem::path model_path) override;
    [[nodiscard]] std::optional<std::filesystem::path> model_path(
        domain::TranscriptionEngine engine) const override;

private:
    NativeEngineRegistryOptions options_;
    mutable std::mutex mutex_;
    std::map<domain::TranscriptionEngine, std::filesystem::path> model_paths_;
};

} // namespace voicetyper::asr
