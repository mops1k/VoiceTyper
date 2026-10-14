#pragma once

// Debug-only transcript capture, excluded from a release build at compile time.
//
// Decision (Alexander, 2026-10-13), amending LOG-01 / VT-SYS-073: a DEBUG build
// may write recognized text to a log, a RELEASE build never writes it anywhere
// but the clipboard. The switch is the compile-time macro
// VOICETYPER_DEBUG_TRANSCRIPTS, which CMake defines for Debug configurations
// only, so a release binary contains neither the write call nor the marker
// literal - this is deliberately not a runtime flag that could be flipped in a
// shipped build.
//
// The file is separate from voiceTyper.log on purpose: the main log's path,
// format and archives are a frozen contract that support tooling parses, and the
// Windows smoke check "Log hygiene" reads it. Keeping the transcripts in their
// own file leaves all of that untouched.
//
// Two capture points, both needed:
//   * the engine boundary (TranscriptDebugPort), i.e. what the ASR engine
//     returned before any decorator rewrote it - the only way to tell "the engine
//     dropped the filler itself" from "the terms dictionary replaced it";
//   * the finished text, immediately before the clipboard and the paste.
//
// Lines carry the marker "TRANSCRIPT-DEBUG" so they can be grepped out of a file
// that is about to be shared.

#include "audio/audio_wav.hpp"
#include "domain/recording_state_machine.hpp"
#include "platform/api/logger.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace voicetyper::domain {

/// File the debug transcripts go to, next to voiceTyper.log in the log directory.
inline constexpr std::string_view kDebugTranscriptFileName = "voiceTyper-debug-transcripts.log";

/// Prefix of every debug transcript line.
inline constexpr std::string_view kDebugTranscriptMarker = "TRANSCRIPT-DEBUG";

/// Writes one already-recognized text to the debug transcript log.
///
/// A null logger - which is what a release build always passes, because the
/// logging block below is compiled out - makes this a no-op. A logging failure is
/// swallowed by contract: logging must never take the application down.
inline void log_debug_transcript(
    platform::Logger* logger,
    std::string_view tag,
    std::string_view text)
{
#if defined(VOICETYPER_DEBUG_TRANSCRIPTS)
    if (logger == nullptr) {
        return;
    }
    static_cast<void>(logger->write(platform::LogLevel::info,
        std::string(kDebugTranscriptMarker) + " " + std::string(tag),
        text));
#else
    static_cast<void>(logger);
    static_cast<void>(tag);
    static_cast<void>(text);
#endif
}

/// Passes audio and text through unchanged and records what the wrapped engine
/// returned. The logger is a pointer so one composition source compiles in both
/// configurations.
class TranscriptDebugPort final : public TranscriptionPort {
public:
    TranscriptDebugPort(TranscriptionPort& engine, platform::Logger* logger, std::string tag)
        : engine_(engine)
        , logger_(logger)
        , tag_(std::move(tag))
    {
    }

    [[nodiscard]] Result<std::string> transcribe(const SampleBuffer& audio,
        const SessionOptions& options,
        const CancellationToken& cancellation) override
    {
        auto result = engine_.transcribe(audio, options, cancellation);
        if (!result.is_error()) {
            log_debug_transcript(logger_, tag_, result.value());
        }
        return result;
    }

private:
    TranscriptionPort& engine_;
    platform::Logger* logger_;
    std::string tag_;
};

} // namespace voicetyper::domain
