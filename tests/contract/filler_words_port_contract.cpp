// Contract for the decorator that applies the filler-word rules to an engine's
// transcript (VT-DOM-1010): the level is read on every call, "off" hands the text
// through untouched, an engine error is never filtered, and the composition runs
// the terms dictionary FIRST - so a term the user asked for can never be eaten as
// a filler.
//
// Prints "filler-words-port-contract: OK" on success; CTest asserts that marker.

#include "domain/filler_words_port.hpp"
#include "domain/terms_dictionary_port.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace voicetyper;
using namespace voicetyper::domain;

int failures = 0;

void check(bool condition, const std::string& name)
{
    if (!condition) {
        ++failures;
        std::cout << "FAIL " << name << "\n";
    }
}

void check_text(const std::string& actual, const std::string& expected, const std::string& name)
{
    if (actual != expected) {
        ++failures;
        std::cout << "FAIL " << name << "\n  expected: [" << expected << "]\n  actual:   [" << actual
                  << "]\n";
    }
}

class FixedTranscriptPort final : public audio::TranscriptionPort {
public:
    explicit FixedTranscriptPort(std::string text)
        : text_(std::move(text))
    {
    }

    [[nodiscard]] Result<std::string> transcribe(const audio::SampleBuffer&,
        const audio::SessionOptions&,
        const CancellationToken&) override
    {
        return text_;
    }

private:
    std::string text_;
};

class FailingPort final : public audio::TranscriptionPort {
public:
    [[nodiscard]] Result<std::string> transcribe(const audio::SampleBuffer&,
        const audio::SessionOptions&,
        const CancellationToken&) override
    {
        return Result<std::string>::failure(ErrorCode::engine_unavailable, "no engine");
    }
};

std::string run(audio::TranscriptionPort& port)
{
    const audio::SampleBuffer audio;
    const audio::SessionOptions options;
    auto result = port.transcribe(audio, options, CancellationToken{});
    if (result.is_error()) {
        return std::string("error:") + result.error().to_string();
    }
    return result.value();
}

void check_off_hands_the_text_through()
{
    FixedTranscriptPort engine("Э-э, ну, включи запись.");
    FillerWordsPort port(engine, [] { return FillerWordsPort::Settings{}; });
    check_text(run(port), "Э-э, ну, включи запись.", "off returns the engine text byte-for-byte");
}

void check_levels()
{
    FixedTranscriptPort soft_engine("Э-э, ну, включи запись.");
    FillerWordsPort soft(soft_engine, [] {
        return FillerWordsPort::Settings{FillerWordsLevel::soft, {}};
    });
    check_text(run(soft), "Ну, включи запись.", "soft removes the filled pause and keeps \"ну\"");

    FixedTranscriptPort hard_engine("Э-э, ну, включи запись.");
    FillerWordsPort hard(hard_engine, [] {
        return FillerWordsPort::Settings{FillerWordsLevel::hard, {}};
    });
    check_text(run(hard), "Включи запись.", "hard removes the filled pause and the hesitation");
}

void check_settings_are_read_on_every_call()
{
    // A live settings object, exactly as the composition reads the presenter.
    FillerWordsLevel level = FillerWordsLevel::soft;
    std::vector<std::string> entries;
    FixedTranscriptPort engine("Э-э, блин, включи запись.");
    FillerWordsPort port(engine, [&level, &entries] {
        return FillerWordsPort::Settings{level, entries};
    });

    // "блин" is not a built-in filler, so the soft level keeps it - and that is
    // exactly what makes the next line meaningful.
    check_text(run(port), "Блин, включи запись.", "soft removes only the built-in filled pause");
    entries = {"блин"};
    check_text(run(port), "Включи запись.", "a user entry added later applies to the next call");
    level = FillerWordsLevel::off;
    check_text(run(port), "Э-э, блин, включи запись.", "turning the level off takes effect at once");
}

void check_engine_errors_are_not_filtered()
{
    FailingPort engine;
    FillerWordsPort port(engine, [] {
        return FillerWordsPort::Settings{FillerWordsLevel::extreme, {}};
    });
    const audio::SampleBuffer audio;
    const audio::SessionOptions options;
    auto result = port.transcribe(audio, options, CancellationToken{});
    check(result.is_error(), "an engine error is passed through instead of becoming text");
    check(result.is_error() && result.error().code() == ErrorCode::engine_unavailable,
        "the error keeps the engine's own code");
}

void check_the_dictionary_runs_first()
{
    // The user asked for "ну" to be written as "нужно". The dictionary therefore has
    // to see the engine's word before the filler filter does: with the filter first,
    // "ну" would already be gone and the requested replacement could never happen.
    FixedTranscriptPort engine("ну");
    TermsDictionaryPort dictionary(engine, [] { return std::string("ну=нужно"); });
    FillerWordsPort port(dictionary, [] {
        return FillerWordsPort::Settings{FillerWordsLevel::extreme, {}};
    });
    check_text(run(port), "нужно",
        "a user term survives the filter, which proves the dictionary is applied first");
}

} // namespace

int main()
{
    check_off_hands_the_text_through();
    check_levels();
    check_settings_are_read_on_every_call();
    check_engine_errors_are_not_filtered();
    check_the_dictionary_runs_first();

    if (failures != 0) {
        std::cout << "filler-words-port-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "filler-words-port-contract: OK\n";
    return 0;
}
