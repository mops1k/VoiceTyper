#pragma once

#include "audio/audio_wav.hpp"
#include "domain/filler_words.hpp"
#include "domain/recording_state_machine.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace voicetyper::domain {

/// Applies the filler-word filter to whatever an engine returned.
///
/// It wraps *any* transcription port instead of living inside an engine, exactly
/// like TermsDictionaryPort: the measurement of 14.10.2026 showed that every
/// engine keeps the fillers, so the filter has to be the same code for all three.
///
/// Order in the composition is deliberate and tested: the terms dictionary runs
/// first (it restores the spellings the user wrote), this filter runs second on
/// text the dictionary already fixed. A user term therefore can never be eaten as
/// a filler, and a filler can never be resurrected by a term replacement.
///
/// The settings are read per call, so a change applies to the next dictation
/// without an apply-order trap; parsing a short string per dictation is the price
/// the terms dictionary already pays for the same property.
class FillerWordsPort final : public TranscriptionPort {
public:
    /// The live settings the filter needs: the level and the user's own entries.
    struct Settings {
        FillerWordsLevel level = FillerWordsLevel::off;
        std::vector<std::string> entries;
    };

    using SettingsProvider = std::function<Settings()>;

    FillerWordsPort(TranscriptionPort& engine, SettingsProvider settings)
        : engine_(engine)
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
        if (!filler_removal_enabled(current.level)) {
            return result; // off: the text is handed on untouched
        }
        return apply_filler_words(result.value(), current.level, current.entries);
    }

private:
    TranscriptionPort& engine_;
    SettingsProvider settings_;
};

} // namespace voicetyper::domain
