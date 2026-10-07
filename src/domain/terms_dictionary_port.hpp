#pragma once

#include "domain/recording_state_machine.hpp"
#include "domain/terms_dictionary.hpp"

#include <functional>
#include <string>
#include <utility>

namespace voicetyper::domain {

/// Applies the terms dictionary to whatever an engine returned.
///
/// It wraps *any* transcription port instead of living inside the Whisper
/// adapter, and that is the point: the explicit replacements are the only
/// dictionary mechanism that exists for every engine. Measured 2026-10-07 -
/// GigaAM answers "does not support vocabulary" and ignores a context prompt,
/// and the Parakeet C API has neither a prompt nor a vocabulary, while Whisper
/// has an initial prompt only. A mechanism inside the Whisper path could never
/// satisfy "the dictionary must work on all engines".
///
/// The dictionary is read per call, so a settings change applies to the next
/// dictation without an apply-order trap, at the cost of parsing a short string.
class TermsDictionaryPort final : public TranscriptionPort {
public:
    using DictionaryProvider = std::function<std::string()>;

    TermsDictionaryPort(TranscriptionPort& engine, DictionaryProvider dictionary)
        : engine_(engine)
        , dictionary_(std::move(dictionary))
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
        const TermsDictionary dictionary =
            parse_terms_dictionary(dictionary_ ? dictionary_() : std::string());
        // A dictionary of plain terms is NOT "nothing to do": the similarity rule
        // applies to them as well, which is exactly what a user who writes
        // "COMMIT, PUSH" expects. Returning early on an empty replacement list
        // skipped every such dictionary - reported from the running build on
        // 2026-10-07 ("Комит пуш" stayed Cyrillic).
        if (dictionary.empty()) {
            return result;
        }
        return apply_terms(result.value(), dictionary);
    }

private:
    TranscriptionPort& engine_;
    DictionaryProvider dictionary_;
};

} // namespace voicetyper::domain
