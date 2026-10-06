#include "asr/native_engine_registry.hpp"

#include <utility>

namespace voicetyper::asr {

NativeEngineRegistry::NativeEngineRegistry(NativeEngineRegistryOptions options)
    : options_(std::move(options))
{
}

NativeEngineRegistry::~NativeEngineRegistry() = default;

platform::EngineAvailability NativeEngineRegistry::availability(domain::TranscriptionEngine engine) const
{
    platform::EngineAvailability result;
    result.engine = engine;
    const auto registered = model_path(engine);
    if (!registered.has_value() || registered->empty()) {
        result.available = false;
        result.reason = platform::EngineAvailabilityReason::model_missing;
        return result;
    }

    switch (engine) {
    case domain::TranscriptionEngine::whisper:
        if (!options_.whisper_available || !options_.whisper_factory) {
            result.available = false;
            result.reason = platform::EngineAvailabilityReason::platform_unsupported;
            return result;
        }
        if (!std::filesystem::is_regular_file(*registered)) {
            result.available = false;
            result.reason = platform::EngineAvailabilityReason::model_missing;
            return result;
        }
        result.available = true;
        result.reason = platform::EngineAvailabilityReason::available;
        return result;

    case domain::TranscriptionEngine::parakeet:
        if (!options_.parakeet_probe) {
            result.available = false;
            result.reason = platform::EngineAvailabilityReason::platform_unsupported;
            return result;
        }
        // The probe loads the DLL, resolves the six frozen symbols and asserts
        // ABI 6. Its verdict is authoritative: the registry never guesses.
        return options_.parakeet_probe(engine, *registered);
    }
    result.available = false;
    result.reason = platform::EngineAvailabilityReason::platform_unsupported;
    return result;
}

domain::Result<std::unique_ptr<platform::Transcriber>> NativeEngineRegistry::create(
    domain::TranscriptionEngine engine, std::filesystem::path model)
{
    if (model.empty()) {
        model = model_path(engine).value_or(std::filesystem::path{});
    }
    if (model.empty() || !std::filesystem::is_regular_file(model)) {
        return domain::Result<std::unique_ptr<platform::Transcriber>>::failure(
            domain::ErrorCode::not_found, "the model file is absent");
    }
    const auto state = availability(engine);
    if (!state.available) {
        return domain::Result<std::unique_ptr<platform::Transcriber>>::failure(
            domain::ErrorCode::engine_unavailable,
            std::string("engine unavailable: ")
                + std::string(platform::engine_availability_reason_name(state.reason)));
    }

    switch (engine) {
    case domain::TranscriptionEngine::whisper:
        return options_.whisper_factory(model);
    case domain::TranscriptionEngine::parakeet:
        return options_.parakeet_factory(options_.parakeet_library, model);
    }
    return domain::Result<std::unique_ptr<platform::Transcriber>>::failure(
        domain::ErrorCode::engine_unavailable, "unknown engine");
}

domain::Status NativeEngineRegistry::set_model_path(
    domain::TranscriptionEngine engine, std::filesystem::path model)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_paths_[engine] = std::move(model);
    return domain::Status::success();
}

std::optional<std::filesystem::path> NativeEngineRegistry::model_path(
    domain::TranscriptionEngine engine) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = model_paths_.find(engine);
    if (it == model_paths_.end() || it->second.empty()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace voicetyper::asr
