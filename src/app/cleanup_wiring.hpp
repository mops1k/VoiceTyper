#pragma once

#include "core/support/model_download_service.hpp"
#include "domain/cleanup_guard.hpp"
#include "domain/cleanup_runtime.hpp"
#include "domain/settings.hpp"
#include "platform/api/model_store.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#ifdef VOICETYPER_HAVE_LLM_RUNTIME
#include "llama_cleanup_runtime.hpp"
#endif

namespace voicetyper::app {

/// Address the chosen cleanup model is fetched from. The three models live in three different
/// repositories rather than under one base URL, so the address is per model; it comes from the
/// same header constants the catalog builds its descriptors from, so the two cannot drift.
[[nodiscard]] inline std::string_view cleanup_model_url(domain::CleanupModel model)
{
    switch (model) {
    case domain::CleanupModel::off: return {};
    case domain::CleanupModel::small: return platform::kCleanupSmallModelUrl;
    case domain::CleanupModel::medium: return platform::kCleanupMediumModelUrl;
    case domain::CleanupModel::large: return platform::kCleanupLargeModelUrl;
    }
    return {};
}

/// Absolute path of the chosen cleanup model inside the models directory, or an empty path when
/// the model is off or its file is not there yet. The file names come from the same core-support
/// header the recognisers use, and the catalog holds the same rows for the store; the port treats
/// an empty path as "no model", so a missing download simply means the rules' text goes on.
[[nodiscard]] inline std::string cleanup_model_path(
    domain::CleanupModel chosen, const std::filesystem::path& models_directory)
{
    const std::string_view file_name = core::support::cleanup_model_file_name(chosen);
    if (file_name.empty()) {
        return {};
    }
    const std::filesystem::path file = models_directory / std::filesystem::path(std::string(file_name));
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        return {};
    }
    return file.string();
}

/// The runtime the composition uses until the llama.cpp library is linked in (plan
/// p_5d68d66503ca, phase B). It never loads and never answers, which is exactly what the port
/// expects from a model that is not there: the text of the rules goes on untouched. Keeping the
/// seam in place means the real runtime replaces this one constructor argument and nothing else.
class NullCleanupRuntime final : public domain::CleanupRuntime {
public:
    [[nodiscard]] bool load(const std::string& path, std::string& error) override
    {
        static_cast<void>(path);
        error = "the local cleaner runtime is not linked into this build";
        return false;
    }

    [[nodiscard]] bool loaded() const override { return false; }

    [[nodiscard]] bool clean(std::string_view text, std::string_view prompt_appendix,
        std::string& cleaned, std::string& error) override
    {
        static_cast<void>(text);
        static_cast<void>(prompt_appendix);
        static_cast<void>(cleaned);
        error = "the local cleaner runtime is not linked into this build";
        return false;
    }

    void unload() override { }
};

/// The runtime the composition uses. When the build carries the pinned llama.cpp runtime
/// (VOICETYPER_BUILD_LLM, which links voicetyper_llm_cleanup and defines
/// VOICETYPER_HAVE_LLM_RUNTIME), this is the real cleaner. Without it the null runtime keeps the
/// same seam: it never loads and never answers, so the rules' text goes on unchanged and a build
/// with no local model behaves exactly as before this feature existed.
[[nodiscard]] inline std::unique_ptr<domain::CleanupRuntime> make_cleanup_runtime()
{
#ifdef VOICETYPER_HAVE_LLM_RUNTIME
    return std::make_unique<llm::LlamaCleanupRuntime>();
#else
    return std::make_unique<NullCleanupRuntime>();
#endif
}

} // namespace voicetyper::app
