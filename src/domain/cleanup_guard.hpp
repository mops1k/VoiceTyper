#pragma once

#include "domain/filler_words.hpp"
#include "domain/utf8_text.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace voicetyper::domain {

/// The verdict of checking a language model's answer against the text it was asked to clean
/// (plan p_5d68d66503ca). The cleaner is an optional, best-effort step: a refusal is never an
/// error for the user, it just means the rules' own result is delivered instead.
struct CleanupVerdict {
    bool accepted = false;
    /// Empty when accepted; otherwise the rule that refused the answer, for tests and logs.
    std::string reason;
};

/// How much longer than the source the answer may be. A cleanup removes filler words and may
/// repair punctuation; a model that answers with a paragraph, a list or an explanation is not
/// cleaning the transcript any more.
inline constexpr std::size_t kCleanupExtraCharacters = 60;

/// The answer may not lose more than two thirds of the words: the transcript is the user's
/// speech, and a model that summarises it is wrong even when the text looks tidy. Two thirds
/// rather than a half, because in a short phrase the fillers *are* most of the words
/// ("Um, I mean, it works." → "It works." keeps two words out of five).
inline constexpr std::size_t kCleanupMinWordRatioDenominator = 3;

/// Letters of one script that must be present before the transcript's language counts as
/// decided. Below this, a phrase is too short to tell a translation from a borrowed word
/// ("Ну вот" → "Well" is not caught, and that is accepted: the rules' own text is delivered
/// anyway when the port refuses, and a wrong refusal on a two-word phrase is worse).
inline constexpr std::size_t kCleanupScriptMinLetters = 6;

/// The share of the source's dominant script the answer must still have. Alexander reported
/// (14.10.2026) that the small model sometimes translated his Russian speech into English, and
/// the answer reached the text: the guard has to refuse a language change even when the model
/// ignores the instruction in the prompt.
inline constexpr std::size_t kCleanupScriptSharePercent = 50;

/// How many words an answer must have before "almost everything in it is new" means anything.
inline constexpr std::size_t kCleanupMinWordsForNovelty = 6;

/// The share of the answer's words that may be absent from the transcript. A cleanup rewrites the
/// speaker's own words; a sentence about prompts, instructions or refusals does not, and such a
/// sentence reached the text when the user's addition happened to mention the prompt itself
/// (Alexander, 14.10.2026: "нельзя менять базовый промпт"). The bound is loose on purpose - a
/// model that rephrases strongly must still pass.
inline constexpr std::size_t kCleanupMaxNovelWordPercent = 70;

/// How many of the source's meaning-bearing words an answer may lose, in percent of them.
///
/// A rephrasing that keeps every number, stays in the language and even keeps the length can still
/// say what the speaker did not. Alexander dictated "перезапустил, выпускай" and the model answered
/// "Запускай." or "Перезапустить.": both verbs of the phrase were replaced by one, and the second
/// half of what he said was gone (17.10.2026). Counting words cannot see that - two words out of
/// two are above the length floor - and the novelty rule does not look at answers shorter than six
/// words.
///
/// A third of the meaning words is where an answer stops being the speaker's sentence: losing one
/// word of a long phrase (a repaired term, a smoothed ending, a form that inflected differently)
/// still passes, while "перезапустил, выпускай" -> "Запускай." and "Сегодня хорошая погода, но
/// завтра будет дождь." -> "Завтра будет дождь." do not.
inline constexpr std::size_t kCleanupMaxMissingMeaningPercent = 30;

/// Code points two words must share from the start before they count as one word in two forms:
/// "код" and "кода" are, "надо" and "нужно" are not. Four is what the guard can defend without a
/// dictionary - enough for Russian inflection, too little to join two unrelated words.
inline constexpr std::size_t kCleanupWordMatchPrefix = 4;

namespace detail {

/// Runs of ASCII digits, in order. The engines emit ASCII digits (measured 2026-10-14), and the
/// rule is deliberately byte-based so a digit can never be "normalised" away.
[[nodiscard]] inline std::vector<std::string> cleanup_digit_runs(std::string_view text)
{
    std::vector<std::string> runs;
    std::size_t index = 0;
    while (index < text.size()) {
        if (text[index] >= '0' && text[index] <= '9') {
            const std::size_t start = index;
            while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
                ++index;
            }
            runs.emplace_back(text.substr(start, index - start));
            continue;
        }
        ++index;
    }
    return runs;
}

/// Words as runs of non-space bytes: enough for the length limits and independent of any
/// locale or Unicode casing rule.
[[nodiscard]] inline std::size_t cleanup_word_count(std::string_view text)
{
    std::size_t words = 0;
    bool in_word = false;
    for (const char byte : text) {
        const bool space = byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
        if (space) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            ++words;
        }
    }
    return words;
}

[[nodiscard]] inline bool cleanup_blank(std::string_view text)
{
    for (const char byte : text) {
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
            return false;
        }
    }
    return true;
}

/// True when every digit run of the source appears in the answer, in the same order. Extra
/// digits are allowed only *after* the whole sequence: a model that invents a number is wrong,
/// but the check refuses as soon as one source run is missing or out of order.
[[nodiscard]] inline bool cleanup_keeps_digits(
    const std::vector<std::string>& source_runs, const std::vector<std::string>& answer_runs)
{
    std::size_t answer_index = 0;
    for (const auto& run : source_runs) {
        bool found = false;
        while (answer_index < answer_runs.size()) {
            if (answer_runs[answer_index] == run) {
                found = true;
                ++answer_index;
                break;
            }
            // A foreign number before this one means the order changed: refuse.
            return false;
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

/// The two scripts the product actually meets, counted as code points so a two-byte Cyrillic
/// letter counts once. Latin is ASCII only, which is what the recognisers emit for terms.
struct CleanupScriptCounts {
    std::size_t cyrillic = 0;
    std::size_t latin = 0;

    [[nodiscard]] std::size_t total() const { return cyrillic + latin; }
};

/// Cyrillic in UTF-8 starts with 0xD0 or 0xD1 (U+0400..U+04FF) and runs two bytes.
[[nodiscard]] inline bool cleanup_is_cyrillic_lead(unsigned char lead) noexcept
{
    return lead == 0xD0 || lead == 0xD1;
}

[[nodiscard]] inline bool cleanup_is_latin_letter(unsigned char byte) noexcept
{
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

[[nodiscard]] inline CleanupScriptCounts cleanup_script_counts(std::string_view text)
{
    CleanupScriptCounts counts;
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        if (cleanup_is_cyrillic_lead(byte) && index + 1 < text.size()
            && (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80) {
            ++counts.cyrillic;
            index += 2;
            continue;
        }
        if (cleanup_is_latin_letter(byte)) {
            ++counts.latin;
            ++index;
            continue;
        }
        ++index;
        while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) {
            ++index;
        }
    }
    return counts;
}

/// True while the answer is still in the language of the transcript. Only a decided language is
/// judged, and only its dominant script: a Russian phrase that carries a Latin term
/// ("добавь commit в ветку") keeps most of its letters Cyrillic and stays accepted.
[[nodiscard]] inline CleanupVerdict cleanup_language_verdict(
    std::string_view source, std::string_view cleaned)
{
    const CleanupScriptCounts source_counts = cleanup_script_counts(source);
    const CleanupScriptCounts answer_counts = cleanup_script_counts(cleaned);
    if (source_counts.total() < kCleanupScriptMinLetters || answer_counts.total() == 0) {
        return {true, {}};
    }
    const bool source_is_cyrillic = source_counts.cyrillic * 2 >= source_counts.total();
    const std::size_t kept = source_is_cyrillic ? answer_counts.cyrillic : answer_counts.latin;
    if (kept * 100 / answer_counts.total() < kCleanupScriptSharePercent) {
        return {false, source_is_cyrillic ? "the answer is no longer Russian"
                                          : "the answer is no longer English"};
    }
    return {true, {}};
}

/// A crude word stem: the first six bytes, ASCII lower-cased. Enough for "промпт"/"промпта" to
/// count as the same word without any locale, dictionary or Unicode casing table.
[[nodiscard]] inline std::string cleanup_word_stem(std::string_view word)
{
    std::string stem;
    for (const char byte : word) {
        if (stem.size() >= 6) {
            break;
        }
        const unsigned char value = static_cast<unsigned char>(byte);
        stem.push_back(
            value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : byte);
    }
    return stem;
}

/// The stems of every letter-or-digit run in `text`.
[[nodiscard]] inline std::vector<std::string> cleanup_word_stems(std::string_view text)
{
    std::vector<std::string> stems;
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        const bool word_start = cleanup_is_latin_letter(byte) || (byte >= '0' && byte <= '9')
            || cleanup_is_cyrillic_lead(byte);
        if (!word_start) {
            ++index;
            while (index < text.size()
                && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) {
                ++index;
            }
            continue;
        }
        const std::size_t start = index;
        while (index < text.size()) {
            const unsigned char next = static_cast<unsigned char>(text[index]);
            if (cleanup_is_cyrillic_lead(next) && index + 1 < text.size()) {
                index += 2;
                continue;
            }
            if (cleanup_is_latin_letter(next) || (next >= '0' && next <= '9')) {
                ++index;
                continue;
            }
            break;
        }
        stems.push_back(cleanup_word_stem(text.substr(start, index - start)));
    }
    return stems;
}

/// True when almost every word of the answer is absent from the transcript: that is not a rewrite
/// of what the user said but a sentence about something else - an explanation, a refusal, or a
/// translation. Used as a separate rule so the reason names what really happened.
[[nodiscard]] inline bool cleanup_mostly_new_words(std::string_view source, std::string_view cleaned)
{
    const std::vector<std::string> answer_stems = cleanup_word_stems(cleaned);
    if (answer_stems.size() < kCleanupMinWordsForNovelty) {
        return false;
    }
    const std::vector<std::string> source_stems = cleanup_word_stems(source);
    std::size_t novel = 0;
    for (const auto& stem : answer_stems) {
        if (std::find(source_stems.begin(), source_stems.end(), stem) == source_stems.end()) {
            ++novel;
        }
    }
    return novel * 100 / answer_stems.size() > kCleanupMaxNovelWordPercent;
}

/// The answer's words, lower-cased. The same notion of a word that meaning_words() uses for the
/// transcript, so both sides of the meaning check are read the same way.
[[nodiscard]] inline std::vector<std::string> cleanup_lower_words(std::string_view text)
{
    std::vector<std::string> words;
    std::string current;
    for (const char32_t point : text::decode(text)) {
        if (text::is_word_character(point)) {
            text::append_utf8(current, text::to_lower(point));
            continue;
        }
        if (!current.empty()) {
            words.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }
    return words;
}

/// True when the two lower-cased words are one word in two forms. Whole code points are compared,
/// never halves of a Cyrillic letter, so the byte-based stem of the novelty rule is not reused
/// here: it cannot lower-case Cyrillic and it splits "код"/"кода" apart.
[[nodiscard]] inline bool cleanup_same_word(std::string_view left, std::string_view right)
{
    const std::vector<char32_t> left_points = text::decode(left);
    const std::vector<char32_t> right_points = text::decode(right);
    const std::size_t shortest = std::min(left_points.size(), right_points.size());
    std::size_t shared = 0;
    while (shared < shortest && left_points[shared] == right_points[shared]) {
        ++shared;
    }
    return shared >= std::min(kCleanupWordMatchPrefix, shortest);
}

/// True when the answer lost so many of the speaker's meaning words that it is no longer the
/// speaker's sentence. Filler words may disappear - that is what the cleaner is for - and so may a
/// word whose form changed; everything else is what the speaker said.
[[nodiscard]] inline bool cleanup_lost_meaning(std::string_view source, std::string_view cleaned,
    const std::vector<std::string>& user_filler_words)
{
    const std::vector<std::string> source_words = meaning_words(source, user_filler_words);
    if (source_words.empty()) {
        // Nothing but fillers: the length rule below still refuses an invented replacement.
        return false;
    }
    const std::vector<std::string> answer_words = cleanup_lower_words(cleaned);
    std::size_t missing = 0;
    for (const auto& word : source_words) {
        bool found = false;
        for (const auto& answer : answer_words) {
            if (cleanup_same_word(word, answer)) {
                found = true;
                break;
            }
        }
        if (!found) {
            ++missing;
        }
    }
    return missing * 100 > source_words.size() * kCleanupMaxMissingMeaningPercent;
}

} // namespace detail

/// Checks one model answer. Everything here is offline and pure: no model, no file, no clock.
///
/// `user_filler_words` is the user's own filler list (settings `fillerWords`, already parsed): such
/// a word is not the speaker's meaning and may disappear from the answer. The built-in lists of
/// every level count as well, so an answer that only removes fillers is never refused for that.
///
/// Refused when: the answer is empty or blank, it contains a code fence (the model wrapped the
/// text in markdown), it adds a line break to a single-line transcript, a digit run of the source
/// is missing or out of order, it lost more than two thirds of the words, it grew by more than
/// kCleanupExtraCharacters, it changed the language of the transcript, it lost more than
/// kCleanupMaxMissingMeaningPercent of the transcript's meaning words, or it is not a rewrite of
/// the transcript at all.
[[nodiscard]] inline CleanupVerdict check_cleanup_output(std::string_view source,
    std::string_view cleaned, const std::vector<std::string>& user_filler_words = {})
{
    if (detail::cleanup_blank(cleaned)) {
        return {false, "empty answer"};
    }
    if (cleaned.find("```") != std::string_view::npos) {
        return {false, "code fence in the answer"};
    }
    if (source.find('\n') == std::string_view::npos && cleaned.find('\n') != std::string_view::npos) {
        return {false, "a line break was added"};
    }

    const auto source_runs = detail::cleanup_digit_runs(source);
    if (!source_runs.empty()
        && !detail::cleanup_keeps_digits(source_runs, detail::cleanup_digit_runs(cleaned))) {
        return {false, "a number went missing or changed order"};
    }

    const std::size_t source_words = detail::cleanup_word_count(source);
    if (source_words > 0
        && detail::cleanup_word_count(cleaned) * kCleanupMinWordRatioDenominator < source_words) {
        return {false, "too much of the text was dropped"};
    }
    if (cleaned.size() > source.size() + kCleanupExtraCharacters) {
        return {false, "the answer grew beyond the limit"};
    }
    const CleanupVerdict language = detail::cleanup_language_verdict(source, cleaned);
    if (!language.accepted) {
        return language;
    }
    if (detail::cleanup_lost_meaning(source, cleaned, user_filler_words)) {
        return {false, "a meaning-bearing word was dropped or replaced"};
    }
    if (detail::cleanup_mostly_new_words(source, cleaned)) {
        return {false, "the answer is not a rewrite of the transcript"};
    }
    return {true, {}};
}

} // namespace voicetyper::domain
