#pragma once

#include "audio/audio_wav.hpp"
#include "domain/cleanup_guard.hpp"
#include "domain/cleanup_runtime.hpp"
#include "domain/recording_state_machine.hpp"
#include "domain/settings.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace voicetyper::domain {

/// The optional last step of the pipeline: a local language model rewrites the transcript that
/// the terms dictionary and the filler rules already produced (plan p_5d68d66503ca).
///
/// The order in the composition is deliberate: engine → terms dictionary → filler rules → here.
/// The rules are the floor, not a fallback path that rarely happens: whatever goes wrong here -
/// the model is off, its file is missing, loading fails, the answer never comes, the guard
/// refuses it - the text that goes on is exactly what the rules produced. Nothing in this port
/// may turn a dictation into an error or an empty line.
class LlmCleanupPort final : public TranscriptionPort {
public:
    struct Settings {
        /// Which model the user picked; `off` is the «Не использовать» row, and then the
        /// runtime is not even asked.
        CleanupModel model = CleanupModel::off;
        /// Absolute path of the GGUF file. The composition resolves it from the model store,
        /// so an empty path means "the file is not there yet".
        std::string model_path;
        /// What the user added to the prompt (settings `cleanupPrompt`), or empty. It extends the
        /// built-in instruction and never replaces it.
        std::string prompt_appendix;
        /// The user's own filler words (settings `fillerWords`, already parsed by
        /// parse_filler_words), or empty. The guard needs them to tell a word the user asked to
        /// drop from a word that carries their meaning: without the list, an answer that removed
        /// the user's own filler would be refused as a lost meaning word (17.10.2026).
        std::vector<std::string> filler_entries{};
    };

    using SettingsProvider = std::function<Settings()>;

    LlmCleanupPort(TranscriptionPort& engine, CleanupRuntime& runtime, SettingsProvider settings)
        : engine_(engine)
        , runtime_(runtime)
        , settings_(std::move(settings))
    {
    }

    [[nodiscard]] Result<std::string> transcribe(const SampleBuffer& audio,
        const SessionOptions& options,
        const CancellationToken& cancellation) override
    {
        auto result = engine_.transcribe(audio, options, cancellation);
        if (result.is_error()) {
            return result;
        }
        const Settings current = settings_ ? settings_() : Settings{};
        if (current.model == CleanupModel::off || current.model_path.empty()) {
            // «Не использовать» - and equally a model whose file is not there - must give the
            // memory and the file back. A loaded GGUF stays mapped by the runtime: it keeps a
            // gigabyte of RAM alive and, on Windows, blocks the file from being deleted
            // (observed 14.10.2026: "could not be removed: access denied" while the interface
            // stayed silent).
            release_model();
            return result;
        }
        return clean_with_the_model(result.value(), current);
    }

private:
    void release_model()
    {
        if (runtime_.loaded()) {
            runtime_.unload();
        }
        loaded_path_.clear();
    }
    [[nodiscard]] std::string clean_with_the_model(const std::string& text, const Settings& current)
    {
        const std::string& path = current.model_path;
        // One load per chosen model: the runtime keeps the weights, and a change of the choice
        // in the interface is what makes it reload.
        if (!runtime_.loaded() || loaded_path_ != path) {
            std::string error;
            if (!runtime_.load(path, error)) {
                return text; // no model, no cleaning: the rules' text is what the user gets
            }
            loaded_path_ = path;
        }
        std::string cleaned;
        std::string error;
        if (!runtime_.clean(text, current.prompt_appendix, cleaned, error)) {
            return text;
        }
        if (!check_cleanup_output(text, cleaned, current.filler_entries).accepted) {
            return text;
        }
        return cleaned;
    }

    TranscriptionPort& engine_;
    CleanupRuntime& runtime_;
    SettingsProvider settings_;
    /// The path the runtime currently holds, so a model is loaded once per choice.
    std::string loaded_path_;
};

} // namespace voicetyper::domain
