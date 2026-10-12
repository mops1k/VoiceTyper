#pragma once

// Transcription contract of the audio layer: what a session may be asked to
// recognize (SessionOptions) and the port every engine and every decorator
// implements (TranscriptionPort).
//
// Why this lives in src/audio and not in src/domain: the port's signature carries
// audio types (SampleBuffer) and the session parameters together, so a header that
// declares it has to include the audio layer. Declaring it here is what keeps the
// dependency one-way - src/domain/recording_state_machine.hpp includes this file
// (domain -> audio) and the audio decorators (SilenceTrimmingPort) live next to
// the contract instead of inside the recording machine.
//
// The audio layer itself depends only on the portable vocabulary below it
// (domain/error.hpp, domain/cancellation.hpp, domain/settings.hpp), never on the
// recording machine, Qt or an OS header.

#include "audio/audio_wav.hpp"
#include "audio/speech_segments.hpp"
#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/settings.hpp"

#include <string>

namespace voicetyper::audio {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::RecognitionLanguage;
using domain::Result;
using domain::Status;

/// The .NET machine always asks for 3 greedy candidates for the final result.
inline constexpr int kFinalBestOf = 3;

/// The persisted setting starts at the machine's own value, so the first dictation
/// after an update behaves exactly as before. settings.hpp cannot include this
/// header (it includes settings.hpp), so the two defaults are repeated and pinned
/// here instead of drifting apart silently.
static_assert(kFinalBestOf == domain::kDefaultBestOf,
    "AppSettings::best_of must default to the recording machine's candidate count");

/// Per-session recognition/output parameters, read on every session so a settings
/// change applies without recreating the machine (the .NET optionsProvider).
struct SessionOptions {
    RecognitionLanguage language = RecognitionLanguage::ru;
    std::string prompt;
    double temperature = 0.0;
    bool condition_on_previous_text = false;
    /// Passed through to the text output port (clipboard first, paste optional).
    bool auto_paste = true;
    int best_of = kFinalBestOf;
    /// Background noise suppression, ported from the .NET build. Off by default, exactly
    /// like the setting it comes from.
    bool noise_suppression = false;
    /// What a detector already found for THIS audio, when a caller ran one (the
    /// trimming decorator does). Empty means "the engine detects for itself".
    SpeechMap speech_map;
};

/// Transcription port: one full final transcript per call.
///
/// An empty string is a *successful* result, and a whitespace-only transcript
/// means "nothing to deliver" (the machine then does not call the output port).
/// There is deliberately no partial/streaming entry point.
class TranscriptionPort {
public:
    virtual ~TranscriptionPort() = default;

    TranscriptionPort(const TranscriptionPort&) = delete;
    TranscriptionPort& operator=(const TranscriptionPort&) = delete;
    TranscriptionPort(TranscriptionPort&&) = delete;
    TranscriptionPort& operator=(TranscriptionPort&&) = delete;

    /// Transcribes the whole session audio. Blocking, called from a worker
    /// thread and never from the thread that pressed the hotkey. It must observe
    /// `cancellation` and return ErrorCode::cancelled when it fires. Failure
    /// codes: engine_unavailable, model_not_ready, cancelled, io_failure.
    [[nodiscard]] virtual Result<std::string> transcribe(
        const SampleBuffer& audio,
        const SessionOptions& options,
        const CancellationToken& cancellation) = 0;

protected:
    TranscriptionPort() = default;
};

} // namespace voicetyper::audio
