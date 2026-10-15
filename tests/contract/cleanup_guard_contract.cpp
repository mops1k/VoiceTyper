// Contract for the check that stands between a local language model and the user's text
// (VT-DOM-1011.., plan p_5d68d66503ca). The cleaner is best-effort: whatever the model does,
// the rules' result is always available, and the one thing that must never happen is a changed
// number in the transcript.
//
// Prints "cleanup-guard-contract: OK" on success; CTest asserts that marker.

#include "domain/cleanup_guard.hpp"

#include <iostream>
#include <string>

namespace {

using namespace voicetyper::domain;

int failures = 0;

void check(bool condition, const std::string& name)
{
    if (!condition) {
        ++failures;
        std::cout << "FAIL " << name << "\n";
    }
}

void check_accepted(const std::string& source, const std::string& cleaned, const std::string& name)
{
    const auto verdict = check_cleanup_output(source, cleaned);
    if (!verdict.accepted) {
        ++failures;
        std::cout << "FAIL " << name << "\n  refused with: " << verdict.reason << "\n";
    }
}

void check_refused(const std::string& source, const std::string& cleaned, const std::string& name)
{
    const auto verdict = check_cleanup_output(source, cleaned);
    if (verdict.accepted) {
        ++failures;
        std::cout << "FAIL " << name << "\n  accepted: [" << cleaned << "]\n";
    }
    if (verdict.accepted || verdict.reason.empty()) {
        ++failures;
        std::cout << "FAIL " << name << ": a refusal must say why\n";
    }
}

void check_ordinary_cleanup_is_accepted()
{
    // The measured shape: the model drops the filled pause and the hesitation and repairs the
    // commas, which is exactly what the cleaner is for.
    check_accepted("Ну, э-э, да, я посмотрел код.", "Да, я посмотрел код.",
        "a normal cleanup with fillers removed is accepted");
    check_accepted("Um, I mean, it works.", "It works.", "an English cleanup is accepted");
    check_accepted("Добрый день, коллеги.", "Добрый день, коллеги.",
        "an answer identical to the source is accepted");
}

void check_digits_are_untouchable()
{
    const std::string source = "Повтори 100, 100 и 4, 0, 4, 0.";
    check_accepted(source, "Повтори 100, 100 и 4, 0, 4, 0.", "digits kept in place are accepted");
    check_refused(source, "Повтори 100 и 400.", "dropping the second 100 is refused");
    check_refused(source, "Повтори 100, 100 и 400.", "merging the digits 4 and 0 is refused");
    check_refused(source, "Повтори 100, 100, 0, 4, 0 и 4.", "reordering digits is refused");
    // The guard watches digits, not punctuation: repairing a colon into a space keeps every
    // digit run intact, and refusing it here would block the punctuation repair the cleaner is
    // asked to do. Only a changed or lost digit is refused.
    check_accepted("Встреча в 15:30.", "Встреча в 15 30.",
        "punctuation around intact digits is the cleaner's business");
    check_refused("Встреча в 15:30.", "Встреча в 15:00.", "changing a digit is refused");
    check_refused("Встреча в 15:30.", "Встреча в 15.", "losing a digit run is refused");
    check_accepted("Встреча в 15:30 и в 16:00.", "Встреча в 15:30 и в 16:00.",
        "two times with digits are accepted");
    check_refused("Встреча в 15:30 и в 16:00.", "Встреча в 15:30 и в 16:30.",
        "swapping a digit in the second time is not accepted silently");
}

void check_shape_limits()
{
    check_refused("Ну, я посмотрел код.", "   ", "a blank answer is refused");
    check_refused("Ну, я посмотрел код.", "", "an empty answer is refused");
    check_refused("Ну, я посмотрел код.", "```\nЯ посмотрел код.\n```",
        "a markdown code fence is refused");
    check_refused("Ну, я посмотрел код.", "Я посмотрел код.\nНадеюсь, помог!",
        "an added line break is refused");
    check_refused("Ну, я посмотрел код, вроде всё нормально.", "Код.",
        "losing most of the words is refused");
    check_accepted("Ну, я посмотрел код, вроде всё нормально.", "Я посмотрел код, вроде всё нормально.",
        "losing only the fillers keeps the words");
    check_accepted("Um, I mean, it works.", "It works.",
        "keeping one word in three is enough for a short phrase");
    check_refused("Коротко.", std::string("Коротко.").append(kCleanupExtraCharacters + 20, 'а'),
        "growing beyond the character limit is refused");
}

void check_digit_runs_helper()
{
    const auto runs = detail::cleanup_digit_runs("Повтори 100, 100 и 4, 0, 4, 0.");
    check(runs.size() == 6 && runs[0] == "100" && runs[1] == "100" && runs[2] == "4"
            && runs[3] == "0" && runs[4] == "4" && runs[5] == "0",
        "digit runs are read in order, one run per number");
    check(detail::cleanup_digit_runs("нет цифр").empty(), "text without digits has no runs");
    check(detail::cleanup_word_count("Ну, я посмотрел код.") == 4,
        "words are counted as runs of non-space bytes");
}

// The small model translated Alexander's Russian speech into English on live dictation
// (14.10.2026), and the English text reached the user. A model that answers in another language
// is never cleaning the transcript, so the guard refuses it and the rules' text goes on.
void check_the_language_is_kept()
{
    check_refused("Ну, это, как бы, тестовая фраза, вот.", "Well, this is, like, a test phrase.",
        "a Russian transcript answered in English is refused");
    check_refused("Э-э, ну, я, типа, не знаю, как бы.", "Uh, well, I, like, do not know.",
        "another Russian transcript answered in English is refused");
    check_refused("Ну, посмотри, что там с коммитом.", "Well, look at what happened with the commit.",
        "a longer Russian sentence answered in English is refused");
    check_refused("Um, this is, like, a test phrase, you know.", "Э-э, это тестовая фраза.",
        "an English transcript answered in Russian is refused");

    // A Russian phrase carrying a borrowed Latin term is not a translation.
    check_accepted("добавь commit в ветку, ну, как бы", "Добавь commit в ветку.",
        "a Latin term inside a Russian phrase is accepted");
    check_accepted("Ну, это, как бы, тестовая фраза, вот.", "Это тестовая фраза.",
        "an ordinary Russian cleanup is accepted");
    // Too short to decide: the guard stays out of the way.
    check_accepted("Ну вот", "Well", "a phrase too short to decide the language is accepted");

    const auto cyrillic = detail::cleanup_script_counts("добавь commit в ветку");
    check(cyrillic.cyrillic == 12 && cyrillic.latin == 6,
        "letters are counted per script, one code point per Cyrillic letter");
}

// An answer that is not a rewrite of what the speaker said: the small model, asked (through the
// user's own addition) about the prompt, answered "нельзя менять базовый промпт" and that
// sentence reached the text (Alexander, 14.10.2026). The bound is loose, so a strong rewrite of a
// long phrase still passes.
void check_the_answer_is_a_rewrite()
{
    check_refused("Также я хотел дополнить промпт вот этим текстом.",
        "Нельзя менять базовый промпт: его можно только дополнить.",
        "an answer about the prompt itself is refused");
    check_refused("Ну, добавь коммит в ветку, как бы.",
        "Конечно! Вот несколько советов, как работать с git: сначала создайте ветку.",
        "an answer that explains instead of cleaning is refused");
    check_accepted(
        "Ну, короче, я вот тут подумал, как бы, что нам, типа, надо, э-э, сначала посмотреть на "
        "этот код, ну, а потом уже, значит, что-то менять, вот.",
        "Я подумал, что нам нужно посмотреть на этот код, а потом уже что-то менять.",
        "a strong rewrite of a long phrase is still accepted");
    check_accepted("Ну, добавь commit в ветку, как бы.", "Добавь commit в ветку.",
        "an ordinary cleanup passes the novelty rule as well");
}

// The model must not rewrite what the speaker said. Alexander dictated "перезапустил, выпускай"
// and the small and medium models answered "Запускай." / "Перезапустить.": the second half of the
// phrase and one of its two verbs were gone, and the meaning with them (17.10.2026). Counting
// words cannot see it (two words out of two are above the length floor) and the novelty rule only
// looks at answers of six words or more, so the guard asks a narrower question: which words of the
// transcript carry the speaker's meaning, and did the answer keep them? Fillers may go - they are
// what the cleaner is for.
void check_the_speakers_meaning_survives()
{
    check_refused("перезапустил, выпускай", "Запускай.",
        "a two-word phrase replaced by one verb is refused");
    check_refused("перезапустил, выпускай", "Перезапустить.",
        "the phrase merged into a single infinitive is refused");
    check_refused("перезапустил, выпускай", "Запустил, выпускай.",
        "a synonym swapped into the first verb is refused");
    check_refused("перезапустил, выпускай", "запустил, выпускай",
        "the same swap in lower case is refused too");
    check_accepted("перезапустил, выпускай", "Перезапустил, выпускай.",
        "a repaired full stop keeps the whole phrase");
    check_accepted("перезапустил, выпускай", "Перезапустил и выпускай.",
        "an added conjunction keeps the whole phrase");
    check_accepted("перезапустил, выпускай", "Выпускай после перезапуска.",
        "the same words in another order are still the speaker's sentence");

    // Summarising is the same failure one phrase longer: half of the sentence disappeared.
    check_refused("Сегодня хорошая погода, но завтра будет дождь.", "Завтра будет дождь.",
        "an answer that keeps only the second half is refused");
    // A word that changed its form is the same word: the boundary is a third of the meaning words.
    check_accepted("Ну, короче, надо посмотреть код, а потом уже что-то менять.",
        "Нужно посмотреть код, а потом уже что-то менять.",
        "\"надо\" replaced by \"нужно\" is the same meaning, not a lost word");
    check_accepted("один два три четыре пять шесть семь восемь девять десять",
        "Один два три четыре пять шесть семь.",
        "losing exactly a third of the meaning words is still accepted");
    check_refused("один два три четыре пять шесть семь восемь девять десять",
        "Один два три четыре пять шесть.",
        "losing more than a third of the meaning words is refused");

    // The user's own filler list is settings, so it arrives as an argument: their word may go.
    check(!check_cleanup_output("Ну, блин, посмотри код.", "Посмотри код.").accepted,
        "without the user's list, an unknown word counts as meaning");
    check(check_cleanup_output("Ну, блин, посмотри код.", "Посмотри код.", {"блин"}).accepted,
        "a word from the user's own filler list may disappear");
    check(check_cleanup_output("Ну, блин, посмотри код.", "Ну, блин, посмотри код.", {"блин"}).accepted,
        "an answer that keeps everything the user wrote is accepted as well");
}

} // namespace

int main()
{
    check_ordinary_cleanup_is_accepted();
    check_digits_are_untouchable();
    check_shape_limits();
    check_digit_runs_helper();
    check_the_language_is_kept();
    check_the_answer_is_a_rewrite();
    check_the_speakers_meaning_survives();

    if (failures != 0) {
        std::cout << "cleanup-guard-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "cleanup-guard-contract: OK\n";
    return 0;
}
