#include "asr/native_engine_registry.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

using namespace voicetyper;
using namespace voicetyper::asr;

/// A Transcriber that only records which engine it was created for.
class TaggedTranscriber final : public platform::Transcriber {
public:
    explicit TaggedTranscriber(domain::TranscriptionEngine engine) : engine_(engine) {}
    domain::TranscriptionEngine engine() const noexcept override { return engine_; }
    const std::filesystem::path& model_path() const noexcept override { return model_; }
    platform::EngineCapabilities capabilities() const noexcept override { return {}; }
    bool is_ready() const noexcept override { return true; }
    domain::Status warmup() override { return domain::Status::success(); }
    domain::Status deep_warmup(const domain::CancellationToken&) override
    {
        return domain::Status::success();
    }
    domain::Result<std::string> transcribe(
        const domain::WavAudio&,
        const platform::TranscriptionRequest&,
        const domain::CancellationToken&) override
    {
        return std::string("tagged");
    }

private:
    domain::TranscriptionEngine engine_;
    std::filesystem::path model_ = "/models/created";
};

struct TempModel {
    explicit TempModel(const std::filesystem::path& path)
    {
        std::ofstream file(path, std::ios::binary);
        file << "not a real model, only a regular file for the availability probe";
    }
};

std::filesystem::path temp_dir()
{
    const auto dir = std::filesystem::temp_directory_path() / "voicetyper-registry-contract";
    std::filesystem::create_directories(dir);
    return dir;
}

void check_no_model_registered()
{
    NativeEngineRegistryOptions options;
    options.whisper_available = true;
    options.whisper_factory = [](const std::filesystem::path&) {
        return domain::Result<std::unique_ptr<platform::Transcriber>>::failure(
            domain::ErrorCode::engine_unavailable, "must not be called without a model");
    };
    NativeEngineRegistry registry(std::move(options));
    const auto state = registry.availability(domain::TranscriptionEngine::whisper);
    check(!state.available, "an engine without a model is not available");
    check(state.reason == platform::EngineAvailabilityReason::model_missing, "the reason is model_missing");
}

void check_missing_model_file()
{
    const auto dir = temp_dir();
    NativeEngineRegistryOptions options;
    options.whisper_available = true;
    options.whisper_factory = [](const std::filesystem::path&) {
        return domain::Result<std::unique_ptr<platform::Transcriber>>::failure(
            domain::ErrorCode::engine_unavailable, "must not be called for a missing file");
    };
    NativeEngineRegistry registry(std::move(options));
    static_cast<void>(registry.set_model_path(
        domain::TranscriptionEngine::whisper, dir / "does-not-exist.bin"));
    const auto state = registry.availability(domain::TranscriptionEngine::whisper);
    check(!state.available && state.reason == platform::EngineAvailabilityReason::model_missing,
        "a registered but absent model file is model_missing");
    const auto created = registry.create(domain::TranscriptionEngine::whisper, {});
    check(created.is_error() && created.code() == domain::ErrorCode::not_found,
        "create reports not_found for an absent model");
}

void check_build_without_whisper()
{
    const auto dir = temp_dir();
    const auto model = dir / "ggml-small-q8_0.bin";
    TempModel file(model);
    NativeEngineRegistryOptions options;
    options.whisper_available = false; // the GUI-off portable contract build
    NativeEngineRegistry registry(std::move(options));
    static_cast<void>(registry.set_model_path(domain::TranscriptionEngine::whisper, model));
    const auto state = registry.availability(domain::TranscriptionEngine::whisper);
    check(!state.available && state.reason == platform::EngineAvailabilityReason::platform_unsupported,
        "a build without whisper.cpp reports platform_unsupported, never a fallback");
}

void check_whisper_create()
{
    const auto dir = temp_dir();
    const auto model = dir / "ggml-small-q8_0.bin";
    TempModel file(model);
    std::atomic<int> factory_calls{0};
    NativeEngineRegistryOptions options;
    options.whisper_available = true;
    options.whisper_factory = [&](const std::filesystem::path&) {
        factory_calls.fetch_add(1);
        return domain::Result<std::unique_ptr<platform::Transcriber>>(
            std::unique_ptr<platform::Transcriber>(new TaggedTranscriber(domain::TranscriptionEngine::whisper)));
    };
    NativeEngineRegistry registry(std::move(options));
    static_cast<void>(registry.set_model_path(domain::TranscriptionEngine::whisper, model));
    check(registry.availability(domain::TranscriptionEngine::whisper).available, "whisper is available");
    const auto created = registry.create(domain::TranscriptionEngine::whisper, model);
    check(created.is_ok(), "whisper is created");
    check(created.value()->engine() == domain::TranscriptionEngine::whisper,
        "the created engine is the requested one");
    check(factory_calls.load() == 1, "the factory was called exactly once");
    check(registry.model_path(domain::TranscriptionEngine::whisper).value() == model, "the model path round-trips");
}

void check_parakeet_abi_mismatch_never_falls_back()
{
    const auto dir = temp_dir();
    const auto model = dir / "tdt-0.6b-v3-q8_0.gguf";
    TempModel file(model);
    std::atomic<int> whisper_calls{0};
    std::atomic<int> parakeet_calls{0};
    NativeEngineRegistryOptions options;
    options.parakeet_library = dir / "parakeet.dll";
    options.whisper_available = true;
    options.whisper_factory = [&](const std::filesystem::path&) {
        whisper_calls.fetch_add(1);
        return domain::Result<std::unique_ptr<platform::Transcriber>>(
            std::unique_ptr<platform::Transcriber>(new TaggedTranscriber(domain::TranscriptionEngine::whisper)));
    };
    options.parakeet_factory = [&](const std::filesystem::path&, const std::filesystem::path&) {
        parakeet_calls.fetch_add(1);
        return domain::Result<std::unique_ptr<platform::Transcriber>>(
            std::unique_ptr<platform::Transcriber>(new TaggedTranscriber(domain::TranscriptionEngine::parakeet)));
    };
    options.parakeet_probe = [](domain::TranscriptionEngine engine, const std::filesystem::path&) {
        platform::EngineAvailability state;
        state.engine = engine;
        state.available = false;
        state.reason = platform::EngineAvailabilityReason::abi_mismatch;
        state.abi_version = 5;
        return state;
    };
    NativeEngineRegistry registry(std::move(options));
    static_cast<void>(registry.set_model_path(domain::TranscriptionEngine::parakeet, model));

    const auto state = registry.availability(domain::TranscriptionEngine::parakeet);
    check(!state.available && state.reason == platform::EngineAvailabilityReason::abi_mismatch,
        "an ABI mismatch is reported as abi_mismatch");
    check(state.abi_version.value_or(-1) == 5, "the observed ABI is passed through");

    const auto created = registry.create(domain::TranscriptionEngine::parakeet, model);
    check(created.is_error() && created.code() == domain::ErrorCode::engine_unavailable,
        "create fails with engine_unavailable");
    check(parakeet_calls.load() == 0, "the parakeet factory is not called for an ABI mismatch");
    check(whisper_calls.load() == 0, "and whisper is never substituted for parakeet");
}

void check_parakeet_create()
{
    const auto dir = temp_dir();
    const auto model = dir / "tdt-0.6b-v3-q8_0.gguf";
    TempModel file(model);
    std::atomic<int> parakeet_calls{0};
    std::filesystem::path seen_dll;
    std::filesystem::path seen_model;
    const auto expected_dll = dir / "parakeet.dll";
    NativeEngineRegistryOptions options;
    options.parakeet_library = expected_dll;
    options.parakeet_factory = [&](const std::filesystem::path& dll, const std::filesystem::path& model_path) {
        parakeet_calls.fetch_add(1);
        seen_dll = dll;
        seen_model = model_path;
        return domain::Result<std::unique_ptr<platform::Transcriber>>(
            std::unique_ptr<platform::Transcriber>(new TaggedTranscriber(domain::TranscriptionEngine::parakeet)));
    };
    options.parakeet_probe = [](domain::TranscriptionEngine engine, const std::filesystem::path&) {
        platform::EngineAvailability state;
        state.engine = engine;
        state.available = true;
        state.reason = platform::EngineAvailabilityReason::available;
        state.abi_version = 6;
        return state;
    };
    NativeEngineRegistry registry(std::move(options));
    static_cast<void>(registry.set_model_path(domain::TranscriptionEngine::parakeet, model));
    const auto created = registry.create(domain::TranscriptionEngine::parakeet, {});
    check(created.is_ok(), "parakeet is created when the probe says available");
    check(created.value()->engine() == domain::TranscriptionEngine::parakeet, "the created engine is parakeet");
    check(parakeet_calls.load() == 1, "the parakeet factory was called once");
    check(seen_dll == expected_dll, "the factory received the configured DLL path");
    check(seen_model == model,
        "the factory received the configured model path: a factory that drops it creates an engine that can never load a model");
}

} // namespace

int main()
{
    check_no_model_registered();
    check_missing_model_file();
    check_build_without_whisper();
    check_whisper_create();
    check_parakeet_abi_mismatch_never_falls_back();
    check_parakeet_create();

    if (failures != 0) {
        std::cerr << "native-engine-registry-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "native-engine-registry-contract: OK\n";
    return 0;
}
