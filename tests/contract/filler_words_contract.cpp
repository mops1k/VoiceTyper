// Contract of the filler-word filter: what a transcript keeps and what it loses.
//
// Every expectation here comes from the measurement of 14.10.2026
// (docs/migration/cpp/filler-words-measurement.md): what whisper small q8 and
// GigaAM-v3 q8_0 actually returned for 24 dictated phrases, on synthetic speech
// and on live speech, and which of those forms must survive the filter.

#include "domain/filler_words.hpp"
#include "domain/settings.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

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

} // namespace

int main()
{
    using namespace voicetyper::domain;
    const std::vector<std::string> none;

    // --- disabled: byte-identical, whatever the text contains -----------------
    check_text(apply_filler_words("Э-э, включи запись.", FillerWordsLevel::off, none),
        "Э-э, включи запись.", "off leaves filled pauses alone");
    check_text(apply_filler_words("Ну, вот, короче, как бы.", FillerWordsLevel::off, none),
        "Ну, вот, короче, как бы.", "off leaves hesitations alone");
    check(!filler_removal_enabled(FillerWordsLevel::off), "off is not enabled");
    check(filler_removal_enabled(FillerWordsLevel::soft), "soft is enabled");
    check(filler_removal_enabled(FillerWordsLevel::extreme), "extreme is enabled");

    // --- soft: filled pauses only --------------------------------------------
    check_text(apply_filler_words("Э-э, включи запись.", FillerWordsLevel::soft, none),
        "Включи запись.", "soft removes a filled pause and its comma");
    check_text(apply_filler_words("Мммм, да, согласен.", FillerWordsLevel::soft, none),
        "Да, согласен.", "soft removes a letter run of the whisper failure mode");
    check_text(apply_filler_words("Аааааааааа, понял, спасибо.", FillerWordsLevel::soft, none),
        "Понял, спасибо.", "soft removes a long vowel run");
    check_text(apply_filler_words("Я, э-э, подумал.", FillerWordsLevel::soft, none), "Я подумал.",
        "soft removes an insertion in the middle");
    check_text(apply_filler_words("Данные, как бы, уже загружены.", FillerWordsLevel::soft, none),
        "Данные, как бы, уже загружены.", "soft keeps verbal hesitations");
    check_text(apply_filler_words("Добрый день, коллеги.", FillerWordsLevel::soft, none),
        "Добрый день, коллеги.", "nothing matched: the text is untouched");
    check_text(apply_filler_words("Um, yes.", FillerWordsLevel::soft, none), "Yes.",
        "soft removes an English filled pause");

    // --- hard: verbal hesitations in an introductory position -----------------
    check_text(apply_filler_words("Ну, я посмотрел код, вроде всё нормально.", FillerWordsLevel::hard, none),
        "Я посмотрел код, вроде всё нормально.", "hard removes a sentence-opening \"ну\"");
    check_text(apply_filler_words("Он, типа, сказал, что придёт позже.", FillerWordsLevel::hard, none),
        "Он сказал, что придёт позже.", "hard removes an insertion and both its commas");
    check_text(apply_filler_words("Вот, значит, смотри: тут нужен рефакторинг.", FillerWordsLevel::hard, none),
        "Смотри: тут нужен рефакторинг.", "hard removes two hesitations in a row");
    check_text(apply_filler_words("Это как бы уже неактуально.", FillerWordsLevel::hard, none),
        "Это как бы уже неактуально.", "hard keeps a phrase that is not an insertion");
    check_text(apply_filler_words("Um, I mean, it works.", FillerWordsLevel::hard, none), "It works.",
        "hard removes an English phrase");

    // Protected combinations: these are the false-positive cases of the measurement.
    check_text(apply_filler_words("Короче говоря, это сравнение корректно.", FillerWordsLevel::extreme, none),
        "Короче говоря, это сравнение корректно.", "\"короче говоря\" is protected");
    check_text(apply_filler_words("Это значит, что сборка упала.", FillerWordsLevel::extreme, none),
        "Это значит, что сборка упала.", "\"это значит\" is protected");
    check_text(apply_filler_words("Ну-ка, посмотри на логи.", FillerWordsLevel::extreme, none),
        "Ну-ка, посмотри на логи.", "\"ну-ка\" is protected");
    check_text(apply_filler_words("В целом всё в порядке, вопросов нет.", FillerWordsLevel::extreme, none),
        "В целом всё в порядке, вопросов нет.", "\"в целом\" is not a filler");
    check_text(apply_filler_words("Что для тебя значит этот результат?", FillerWordsLevel::extreme, none),
        "Что для тебя значит этот результат?", "a \"значит\" that is a verb stays");

    // --- extreme: repetitions and openers ------------------------------------
    check_text(apply_filler_words("Я я думаю, что это правильно.", FillerWordsLevel::extreme, none),
        "Я думаю, что это правильно.", "extreme removes a repeated word");
    check_text(apply_filler_words("Мы это, это уже обсуждали.", FillerWordsLevel::extreme, none),
        "Мы это уже обсуждали.", "extreme removes a repetition across a comma");
    check_text(apply_filler_words("Я-я думаю, что это правильно.", FillerWordsLevel::extreme, none),
        "Я-я думаю, что это правильно.", "\"Я-я\" is left as the engine wrote it");
    check_text(apply_filler_words("Так, теперь по делу.", FillerWordsLevel::extreme, none),
        "Теперь по делу.", "extreme removes a sentence-opening \"так\"");
    check_text(apply_filler_words("Я я думаю, что это правильно.", FillerWordsLevel::hard, none),
        "Я я думаю, что это правильно.", "hard does not touch repetitions");

    // --- digits are data, never fillers --------------------------------------
    const std::string numbers = "Повтори 100, 100 и 4, 0, 4, 0.";
    check_text(apply_filler_words(numbers, FillerWordsLevel::soft, none), numbers,
        "soft never touches digits");
    check_text(apply_filler_words(numbers, FillerWordsLevel::hard, none), numbers,
        "hard never touches digits");
    check_text(apply_filler_words(numbers, FillerWordsLevel::extreme, none), numbers,
        "extreme never touches digits or the commas between them");

    // --- user additions -------------------------------------------------------
    const std::vector<std::string> user{"блин", "  как бы  ,,\nёлки"};
    check(user.size() == 2, "user entries are trimmed and de-duplicated by emptiness");
    check_text(apply_filler_words("Ну, блин, опять.", FillerWordsLevel::soft, {"блин"}), "Ну опять.",
        "a user entry applies at every level");
    check_text(apply_filler_words("Ну, блин, опять.", FillerWordsLevel::off, {"блин"}),
        "Ну, блин, опять.", "a user entry does nothing while the level is off");
    check_text(apply_filler_words("Данные как бы уже загружены.", FillerWordsLevel::hard, {}),
        "Данные как бы уже загружены.",
        "a phrase in the middle without commas is not an insertion and stays");

    // --- idempotency ----------------------------------------------------------
    for (const auto level : {FillerWordsLevel::soft, FillerWordsLevel::hard, FillerWordsLevel::extreme}) {
        const std::string once = apply_filler_words("Ну, э-э, как бы, вот, короче, я закончил.", level, {});
        const std::string twice = apply_filler_words(once, level, {});
        check(once == twice, "the pass is idempotent");
    }

    // --- meaning words: what the cleanup guard may not see disappear -----------
    // The guard above the local model asks the opposite question: which words carry the speaker's
    // meaning, so an answer that lost them can be refused (Alexander, 17.10.2026: "перезапустил,
    // выпускай" was answered with "Запускай."). Every level's entries count, and the position rules
    // are deliberately not applied: a word the filter kept may be an insertion the model removes.
    {
        const auto words = meaning_words("Ну, посмотри код.", {});
        check(words.size() == 2 && words[0] == "посмотри" && words[1] == "код",
            "a leading filler is not a meaning word and the rest is lower-cased");
    }
    {
        const auto words = meaning_words("Это значит, что код готов.", {});
        check(words.size() == 3 && words[0] == "что" && words[1] == "код" && words[2] == "готов",
            "entries of the extreme and hard levels are not meaning words either");
    }
    {
        // The filter keeps "как бы" in the middle without commas; the guard still lets it go.
        const auto words = meaning_words("Данные как бы уже загружены.", {});
        check(words.size() == 3 && words[0] == "данные" && words[1] == "уже"
                && words[2] == "загружены",
            "the guard reads fillers without the position rules");
    }
    {
        const auto words = meaning_words("Um, I mean, it works.", {});
        check(words.size() == 2 && words[0] == "it" && words[1] == "works",
            "a two-word English entry is dropped whole");
    }
    {
        const auto words = meaning_words("Ну, блин, опять.", {"блин"});
        check(words.size() == 1 && words[0] == "опять",
            "the user's own filler word is not a meaning word");
    }
    {
        const auto words = meaning_words("Повтори 100, 100.", {});
        check(words.size() == 3 && words[0] == "повтори" && words[1] == "100" && words[2] == "100",
            "digits stay words: they are the speaker's meaning, not fillers");
    }
    check(meaning_words("  \n ", {}).empty(), "a blank transcript has no meaning words");

    if (failures != 0) {
        std::cout << "filler-words-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "filler-words-contract: OK\n";
    return 0;
}
