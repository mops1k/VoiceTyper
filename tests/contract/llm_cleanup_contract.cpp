// Contract for the optional local cleaner (VT-DOM-1011..VT-DOM-1013, plan p_5d68d66503ca).
//
// The cleaner is best-effort: the model is asked only when the user chose one and its file is
// there, the weights are loaded once per choice, and every failure mode - a missing file, a
// runtime that cannot answer, an answer the guard refuses - ends with the rules' own text going
// on. A dictation must never become an error or an empty line because of the cleaning.
//
// Prints "llm-cleanup-contract: OK" on success; CTest asserts that marker.

#include "domain/llm_cleanup_port.hpp"
#include "domain/filler_words_port.hpp"
#include "domain/terms_dictionary_port.hpp"

#include <functional>
#include <iostream>
#include <string>
#include <utility>

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

/// A stand-in for the native runtime: it counts what it was asked to do and can fail on demand.
class FakeCleaner final : public CleanupRuntime {
public:
    [[nodiscard]] bool load(const std::string& path, std::string& error) override
    {
        ++loads;
        last_loaded = path;
        if (!load_succeeds) {
            error = "model file is unusable";
            return false;
        }
        ready = true;
        return true;
    }

    [[nodiscard]] bool loaded() const override { return ready; }

    [[nodiscard]] bool clean(std::string_view text, std::string_view prompt_appendix,
        std::string& cleaned, std::string& error) override
    {
        ++cleans;
        last_input = std::string(text);
        last_appendix = std::string(prompt_appendix);
        if (!clean_succeeds) {
            error = "cleanup timed out";
            return false;
        }
        cleaned = answer ? answer(std::string(text)) : std::string(text);
        return true;
    }

    void unload() override
    {
        ++unloads;
        ready = false;
    }

    int loads = 0;
    int cleans = 0;
    int unloads = 0;
    bool load_succeeds = true;
    bool clean_succeeds = true;
    bool ready = false;
    std::string last_loaded;
    std::string last_input;
    std::string last_appendix;
    std::function<std::string(std::string)> answer;
};

std::string run(audio::TranscriptionPort& port)
{
    const audio::SampleBuffer audio;
    const audio::SessionOptions options;
    auto result = port.transcribe(audio, options, CancellationToken{});
    if (result.is_error()) {
        return std::string("error");
    }
    return result.value();
}

void check_the_model_is_asked_only_when_chosen()
{
    FixedTranscriptPort engine("Ну, э-э, да, я посмотрел код.");
    FakeCleaner cleaner;
    LlmCleanupPort port(engine, cleaner, [] { return LlmCleanupPort::Settings{}; });

    check_text(run(port), "Ну, э-э, да, я посмотрел код.", "off hands the rules' text through");
    check(cleaner.loads == 0 && cleaner.cleans == 0, "off never touches the runtime");

    LlmCleanupPort without_file(engine, cleaner, [] {
        return LlmCleanupPort::Settings{CleanupModel::small, ""};
    });
    check_text(run(without_file), "Ну, э-э, да, я посмотрел код.",
        "a chosen model without a file still delivers the rules' text");
    check(cleaner.loads == 0, "a missing file is not even passed to the runtime");
}

void check_a_good_answer_is_delivered()
{
    FixedTranscriptPort engine("Ну, э-э, да, я посмотрел код.");
    FakeCleaner cleaner;
    cleaner.answer = [](std::string) { return std::string("Да, я посмотрел код."); };
    LlmCleanupPort port(engine, cleaner, [] {
        return LlmCleanupPort::Settings{CleanupModel::small, "C:/models/small.gguf"};
    });

    check_text(run(port), "Да, я посмотрел код.", "an accepted answer replaces the rules' text");
    check(cleaner.loads == 1 && cleaner.cleans == 1, "the model is loaded and asked once");
    check(cleaner.last_loaded == "C:/models/small.gguf", "the runtime gets the chosen path");
    check(cleaner.last_input == "Ну, э-э, да, я посмотрел код.",
        "the model is asked about the rules' text, not the raw transcript");

    check_text(run(port), "Да, я посмотрел код.", "a second dictation reuses the loaded model");
    check(cleaner.loads == 1 && cleaner.cleans == 2, "the weights are loaded once per choice");
}

void check_a_change_of_choice_reloads()
{
    FixedTranscriptPort engine("Ну, да, я посмотрел код.");
    FakeCleaner cleaner;
    CleanupModel chosen = CleanupModel::small;
    LlmCleanupPort port(engine, cleaner, [&chosen] {
        return LlmCleanupPort::Settings{chosen,
            chosen == CleanupModel::large ? "C:/models/large.gguf" : "C:/models/small.gguf"};
    });

    static_cast<void>(run(port));
    check(cleaner.loads == 1, "the first dictation loads the chosen model");
    chosen = CleanupModel::large;
    static_cast<void>(run(port));
    check(cleaner.loads == 2 && cleaner.last_loaded == "C:/models/large.gguf",
        "choosing another model reloads the runtime");
}

void check_every_failure_keeps_the_rules_text()
{
    const std::string rules_text = "Повтори 100, 100 и 4, 0, 4, 0.";

    {
        FixedTranscriptPort engine(rules_text);
        FakeCleaner cleaner;
        cleaner.load_succeeds = false;
        LlmCleanupPort port(engine, cleaner, [] {
            return LlmCleanupPort::Settings{CleanupModel::medium, "C:/models/medium.gguf"};
        });
        check_text(run(port), rules_text, "a model that cannot load leaves the rules' text");
        cleaner.load_succeeds = true;
        cleaner.answer = [](std::string) { return std::string("Повтори 100, 100 и 4, 0, 4, 0."); };
        check_text(run(port), rules_text, "the next dictation retries and succeeds");
        check(cleaner.loads == 2, "a failed load is not remembered as loaded");
    }

    {
        FixedTranscriptPort engine(rules_text);
        FakeCleaner cleaner;
        cleaner.clean_succeeds = false;
        LlmCleanupPort port(engine, cleaner, [] {
            return LlmCleanupPort::Settings{CleanupModel::large, "C:/models/large.gguf"};
        });
        check_text(run(port), rules_text, "a runtime that cannot answer leaves the rules' text");
    }

    {
        FixedTranscriptPort engine(rules_text);
        FakeCleaner cleaner;
        // The model merged the numbers: the guard must refuse the answer, not the whole dictation.
        cleaner.answer = [](std::string) { return std::string("Повтори 100 и 400."); };
        LlmCleanupPort port(engine, cleaner, [] {
            return LlmCleanupPort::Settings{CleanupModel::large, "C:/models/large.gguf"};
        });
        check_text(run(port), rules_text, "an answer that loses a number is refused silently");
    }

    {
        FixedTranscriptPort engine(rules_text);
        FakeCleaner cleaner;
        cleaner.answer = [](std::string) { return std::string("```\nПовтори 100, 100 и 4, 0, 4, 0.\n```"); };
        LlmCleanupPort port(engine, cleaner, [] {
            return LlmCleanupPort::Settings{CleanupModel::large, "C:/models/large.gguf"};
        });
        check_text(run(port), rules_text, "an answer wrapped in a code fence is refused");
    }
}

void check_the_whole_chain_runs_in_order()
{
    // The real composition: engine → terms dictionary → filler rules → local model. The model
    // must be asked about the text the user will actually see minus the model's own step, i.e.
    // with the term spelled as the user wants it and without the fillers: that is what makes the
    // model a last, optional polish instead of a competing rewrite.
    FixedTranscriptPort engine("Ну, э-э, добавь комит в ветку.");
    FakeCleaner cleaner;
    cleaner.answer = [](std::string) { return std::string("Добавь commit в ветку."); };

    TermsDictionaryPort dictionary(engine, [] { return std::string("комит=commit"); });
    FillerWordsPort rules(dictionary, [] {
        return FillerWordsPort::Settings{FillerWordsLevel::hard, {}};
    });
    LlmCleanupPort cleaner_port(rules, cleaner, [] {
        return LlmCleanupPort::Settings{CleanupModel::small, "C:/models/small.gguf"};
    });

    check_text(run(cleaner_port), "Добавь commit в ветку.",
        "the user gets the model's answer after both earlier steps");
    check(cleaner.last_input == "Добавь commit в ветку.",
        "the model is asked about the dictionary-corrected, filler-free text");
}

// Switching the model in the interface must switch the runtime: the other file is loaded, the old
// weights are released, and «Не использовать» releases them too. Alexander asked for exactly this
// check (14.10.2026). The release is not a nicety: a mapped GGUF keeps a gigabyte of RAM alive and
// makes the file undeletable on Windows.
void check_the_runtime_follows_the_choice()
{
    FixedTranscriptPort engine("Ну, добавь коммит.");
    FakeCleaner cleaner;
    std::string path = "C:/models/small.gguf";
    LlmCleanupPort port(engine, cleaner, [&path] {
        return LlmCleanupPort::Settings{
            path.empty() ? CleanupModel::off : CleanupModel::small, path};
    });

    run(port);
    check(cleaner.loads == 1, "the chosen model is loaded once");
    check(cleaner.last_loaded == "C:/models/small.gguf", "the file that is loaded is the chosen one");

    run(port);
    check(cleaner.loads == 1, "the same choice does not read the weights again");

    path = "C:/models/large.gguf";
    run(port);
    check(cleaner.loads == 2, "another choice loads the other model");
    check(cleaner.last_loaded == "C:/models/large.gguf", "the other file is the one that loads");

    path.clear();
    run(port);
    check(cleaner.unloads == 1 && !cleaner.loaded(), "«Не использовать» releases the loaded model");
    check(cleaner.cleans == 3, "the model is not asked while it is off");
}

void check_the_prompt_addition_reaches_the_runtime()
{
    // The user extends the prompt, never replaces it (Alexander, 14.10.2026). What the port has
    // to guarantee is that the addition arrives; how it is worded and capped is the runtime's
    // business (VT-DOM-1017), and how safe the answer is, is the guard's.
    FixedTranscriptPort engine("Ну, добавь коммит.");
    FakeCleaner cleaner;
    LlmCleanupPort port(engine, cleaner, [] {
        return LlmCleanupPort::Settings{CleanupModel::small, "C:/models/small.gguf",
            "сделай текст ровнее и убери повторы"};
    });
    run(port);
    check(cleaner.last_appendix == "сделай текст ровнее и убери повторы",
        "the user's addition to the prompt reaches the runtime");
}

void check_engine_errors_are_not_touched()
{
    FailingPort engine;
    FakeCleaner cleaner;
    cleaner.answer = [](std::string) { return std::string("что-то другое"); };
    LlmCleanupPort port(engine, cleaner, [] {
        return LlmCleanupPort::Settings{CleanupModel::large, "C:/models/large.gguf"};
    });
    const audio::SampleBuffer audio;
    const audio::SessionOptions options;
    auto result = port.transcribe(audio, options, CancellationToken{});
    check(result.is_error() && result.error().code() == ErrorCode::engine_unavailable,
        "an engine error passes through with its own code");
    check(cleaner.loads == 0 && cleaner.cleans == 0, "the cleaner is not asked without text");
}

} // namespace

int main()
{
    check_the_model_is_asked_only_when_chosen();
    check_a_good_answer_is_delivered();
    check_a_change_of_choice_reloads();
    check_every_failure_keeps_the_rules_text();
    check_the_whole_chain_runs_in_order();
    check_the_runtime_follows_the_choice();
    check_the_prompt_addition_reaches_the_runtime();
    check_engine_errors_are_not_touched();

    if (failures != 0) {
        std::cout << "llm-cleanup-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "llm-cleanup-contract: OK\n";
    return 0;
}
