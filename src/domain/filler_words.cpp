#include "domain/filler_words.hpp"

#include "domain/utf8_text.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace voicetyper::domain {
namespace {

using text::append_utf8;
using text::decode;
using text::encode;
using text::is_digit;
using text::is_letter;
using text::is_space;
using text::is_upper;
using text::is_word_character;
using text::lower_copy;
using text::split_entries;
using text::to_lower;
using text::to_upper;
using text::trim;

/// One word of the transcript: a range of code points plus its lower-cased form.
struct Word {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string lower;
};

/// One filler rule: the words it consists of ("как бы" is two) and where it may
/// be applied.
struct Entry {
    std::vector<std::string> words;
    /// `anywhere` for filled pauses and user entries, `insertion` for the
    /// hesitations of the hard level, `sentence_start` for the extreme level.
    enum class Position { anywhere, insertion, sentence_start } position = Position::anywhere;
};

std::vector<Word> split_words(const std::vector<char32_t>& points)
{
    std::vector<Word> words;
    std::size_t index = 0;
    while (index < points.size()) {
        if (!is_word_character(points[index])) {
            ++index;
            continue;
        }
        Word word;
        word.begin = index;
        while (index < points.size() && is_word_character(points[index])) {
            ++index;
        }
        word.end = index;
        word.lower.reserve(word.end - word.begin);
        for (std::size_t point = word.begin; point < word.end; ++point) {
            append_utf8(word.lower, to_lower(points[point]));
        }
        words.push_back(std::move(word));
    }
    return words;
}

std::vector<std::string> words_of(std::string_view entry)
{
    const auto points = decode(entry);
    std::vector<Word> words = split_words(points);
    std::vector<std::string> result;
    result.reserve(words.size());
    for (auto& word : words) {
        result.push_back(std::move(word.lower));
    }
    return result;
}

std::vector<Entry> entries_of(const std::vector<std::string>& raw, Entry::Position position)
{
    std::vector<Entry> entries;
    for (const auto& item : raw) {
        auto words = words_of(item);
        if (!words.empty()) {
            Entry entry;
            entry.words = std::move(words);
            entry.position = position;
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

/// Filled pauses. Both engines keep them (measured 2026-10-14).
const std::vector<std::string>& soft_words()
{
    static const std::vector<std::string> entries{
        "э-э", "ээ", "э-э-э", "эээ", "эм", "эм-м", "эмм", "м-м", "мм", "ммм", "а-а", "аа",
        "um", "uh", "uhm", "umm", "er", "erm", "hmm", "mm", "mmm", "ah",
    };
    return entries;
}

/// Verbal hesitations: removed only where they are an insertion.
const std::vector<std::string>& hard_words()
{
    static const std::vector<std::string> entries{
        "ну", "вот", "значит", "короче", "типа", "собственно",
        "как бы", "в общем", "так сказать", "это самое", "как говорится",
        "you know", "i mean", "sort of", "kind of", "like",
    };
    return entries;
}

/// Openers that can still carry meaning, plus the phrases built from them.
const std::vector<std::string>& extreme_words()
{
    static const std::vector<std::string> entries{
        "вообще", "так", "это", "в принципе",
        "well", "basically", "actually", "literally", "right",
    };
    return entries;
}

bool word_is(const std::vector<Word>& words, std::size_t index, std::string_view value)
{
    return index < words.size() && words[index].lower == value;
}

/// Skips spaces and tabs from `from`; returns the first other code point index.
std::size_t skip_blanks(const std::vector<char32_t>& points, std::size_t from)
{
    while (from < points.size() && (points[from] == U' ' || points[from] == U'\t')) {
        ++from;
    }
    return from;
}

/// "ну-ка": a hyphen right after the word glues a particle, so it is not a filler.
bool hyphen_after(const std::vector<char32_t>& points, std::size_t end)
{
    const std::size_t next = skip_blanks(points, end);
    return next < points.size() && points[next] == U'-';
}

bool comma_before(const std::vector<char32_t>& points, std::size_t begin)
{
    std::size_t index = begin;
    while (index > 0 && (points[index - 1] == U' ' || points[index - 1] == U'\t')) {
        --index;
    }
    return index > 0 && points[index - 1] == U',';
}

bool comma_after(const std::vector<char32_t>& points, std::size_t end)
{
    const std::size_t next = skip_blanks(points, end);
    return next < points.size() && points[next] == U',';
}

bool sentence_end_before(const std::vector<char32_t>& points, std::size_t begin)
{
    std::size_t index = begin;
    while (index > 0 && (points[index - 1] == U' ' || points[index - 1] == U'\t')) {
        --index;
    }
    if (index == 0) {
        return true;
    }
    const char32_t previous = points[index - 1];
    return previous == U'.' || previous == U'!' || previous == U'?';
}

bool digit_before(const std::vector<char32_t>& points, std::size_t begin)
{
    std::size_t index = begin;
    while (index > 0 && (points[index - 1] == U' ' || points[index - 1] == U'\t')) {
        --index;
    }
    return index > 0 && is_digit(points[index - 1]);
}

bool digit_after(const std::vector<char32_t>& points, std::size_t end)
{
    const std::size_t next = skip_blanks(points, end);
    return next < points.size() && is_digit(points[next]);
}

/// Protected combinations, checked before a match is accepted.
bool protected_at(const std::vector<char32_t>& points, const std::vector<Word>& words,
    std::size_t index, const std::vector<std::string>& entry)
{
    const std::string& first = entry.front();
    if (first == "короче") {
        // "короче говоря" is a connector, not a filler.
        return word_is(words, index + 1, "говоря");
    }
    if (first == "значит") {
        // "это значит" is a verb.
        return index > 0 && words[index - 1].lower == "это";
    }
    if (first == "ну") {
        return hyphen_after(points, words[index].end);
    }
    if (first == "это") {
        // "это значит" is a fixed expression, and the extreme level may open a
        // sentence with "это": removing the opener would break the pair.
        return word_is(words, index + 1, "значит");
    }
    if (first == "как" && entry.size() == 2) {
        // "как бы то ни было" is a fixed expression.
        return word_is(words, index + 2, "то") && word_is(words, index + 3, "ни")
            && word_is(words, index + 4, "было");
    }
    return false;
}

bool allowed_position(const std::vector<char32_t>& points, const std::vector<Word>& words,
    std::size_t index, Entry::Position position)
{
    if (position == Entry::Position::anywhere) {
        return true;
    }
    const bool at_sentence_start = sentence_end_before(points, words[index].begin);
    if (position == Entry::Position::sentence_start) {
        return at_sentence_start;
    }
    return at_sentence_start || comma_before(points, words[index].begin)
        || comma_after(points, words[index].end);
}

bool word_has_digit(const std::vector<char32_t>& points, const Word& word)
{
    for (std::size_t index = word.begin; index < word.end; ++index) {
        if (is_digit(points[index])) {
            return true;
        }
    }
    return false;
}

/// True when two identical words are separated by spaces and at most one comma -
/// the stutter GigaAM writes ("Мы это, это уже обсуждали"). A hyphen is not
/// included on purpose: "Я-я" is the engine's own spelling, and removing half of
/// it would leave a dangling hyphen (Alexander's policy, 14.10.2026).
bool repeat_separator(const std::vector<char32_t>& points, std::size_t from, std::size_t to)
{
    bool comma_seen = false;
    for (std::size_t index = from; index < to; ++index) {
        const char32_t point = points[index];
        if (point == U' ' || point == U'\t') {
            continue;
        }
        if (point == U',' && !comma_seen) {
            comma_seen = true;
            continue;
        }
        return false;
    }
    return true;
}

/// Copies the text without the marked words, taking a comma that delimited an
/// inserted filler with it, so "Он, типа, сказал" becomes "Он сказал" and not
/// "Он, , сказал".
std::vector<char32_t> without_removed(const std::vector<char32_t>& points,
    const std::vector<Word>& words, const std::vector<bool>& remove, bool& removed_at_sentence_start)
{
    std::vector<char32_t> out;
    out.reserve(points.size());
    std::size_t cursor = 0;
    bool previous_removed = false;
    for (std::size_t index = 0; index < words.size(); ++index) {
        if (!remove[index]) {
            previous_removed = false;
            continue;
        }
        if (!previous_removed) {
            out.insert(out.end(), points.begin() + static_cast<std::ptrdiff_t>(cursor),
                points.begin() + static_cast<std::ptrdiff_t>(words[index].begin));
        }
        cursor = words[index].end;
        previous_removed = true;

        const bool at_sentence_start = sentence_end_before(points, words[index].begin);
        if (at_sentence_start) {
            removed_at_sentence_start = true;
        }
        // A comma on both sides of the removed word is punctuation of the
        // insertion itself, so it goes too. Two digits around it mean the comma
        // is data ("4, 0"): then nothing is dropped.
        if (digit_before(points, words[index].begin) && digit_after(points, words[index].end)) {
            continue;
        }
        std::size_t end = out.size();
        while (end > 0 && (out[end - 1] == U' ' || out[end - 1] == U'\t')) {
            --end;
        }
        if (end > 0 && out[end - 1] == U',') {
            out.erase(out.begin() + static_cast<std::ptrdiff_t>(end - 1), out.end());
        }
        if (comma_after(points, cursor)) {
            cursor = skip_blanks(points, cursor);
            ++cursor; // the comma itself
        }
    }
    out.insert(out.end(), points.begin() + static_cast<std::ptrdiff_t>(cursor), points.end());
    return out;
}

/// Space and punctuation repair. Runs only when something was removed, so a
/// transcript that nothing matched stays byte-identical.
void cleanup(std::vector<char32_t>& points)
{
    const auto is_punctuation = [](char32_t point) {
        return point == U',' || point == U'.' || point == U'!' || point == U'?' || point == U':'
            || point == U';';
    };

    // One space between words.
    std::vector<char32_t> collapsed;
    collapsed.reserve(points.size());
    bool previous_space = false;
    for (const char32_t point : points) {
        const bool space = point == U' ' || point == U'\t';
        if (space && previous_space) {
            continue;
        }
        previous_space = space;
        collapsed.push_back(point);
    }

    // No space before punctuation, no comma doubled or left dangling.
    std::vector<char32_t> result;
    result.reserve(collapsed.size());
    for (std::size_t index = 0; index < collapsed.size(); ++index) {
        const char32_t point = collapsed[index];
        const bool space = point == U' ' || point == U'\t';
        if (space) {
            const std::size_t next = skip_blanks(collapsed, index);
            if (next < collapsed.size() && is_punctuation(collapsed[next])) {
                index = next - 1;
                continue;
            }
            result.push_back(U' ');
            continue;
        }
        if (point == U',') {
            const std::size_t next = skip_blanks(collapsed, index + 1);
            const bool dangling = next >= collapsed.size() || collapsed[next] == U','
                || is_punctuation(collapsed[next]);
            // A comma between two digits is data, not punctuation.
            if (dangling && !(digit_before(collapsed, index) && digit_after(collapsed, index + 1))) {
                continue;
            }
        }
        result.push_back(point);
    }

    // No leading or trailing space.
    while (!result.empty() && (result.front() == U' ')) {
        result.erase(result.begin());
    }
    while (!result.empty() && (result.back() == U' ')) {
        result.pop_back();
    }
    points = std::move(result);
}

/// A sentence that lost its first word starts lowercase; restore the capital.
void restore_capitals(std::vector<char32_t>& points, bool capitalize_after_end)
{
    bool expect_start = true;
    for (auto& point : points) {
        if (expect_start) {
            if (is_letter(point)) {
                if (!is_upper(point)) {
                    point = to_upper(point);
                }
                expect_start = false;
            } else if (is_digit(point)) {
                expect_start = false;
            }
            continue;
        }
        if (point == U'.' || point == U'!' || point == U'?') {
            expect_start = capitalize_after_end;
        }
    }
}

} // namespace

std::vector<std::string> parse_filler_words(std::string_view text)
{
    std::vector<std::string> entries;
    std::vector<std::string> lowered;
    for (const auto raw : split_entries(text)) {
        const auto entry = trim(raw);
        if (entry.empty()) {
            continue;
        }
        const std::string candidate(entry);
        const std::string key = lower_copy(candidate);
        if (std::find(lowered.begin(), lowered.end(), key) != lowered.end()) {
            continue;
        }
        lowered.push_back(key);
        entries.push_back(candidate);
    }
    return entries;
}

bool filler_removal_enabled(FillerWordsLevel level) noexcept
{
    return level != FillerWordsLevel::off;
}

std::string apply_filler_words(const std::string& text, FillerWordsLevel level,
    const std::vector<std::string>& user_entries)
{
    if (!filler_removal_enabled(level) || text.empty()) {
        return text;
    }

    const std::vector<char32_t> points = decode(text);
    const std::vector<Word> words = split_words(points);
    if (words.empty()) {
        return text;
    }

    std::vector<Entry> entries = entries_of(soft_words(), Entry::Position::anywhere);
    if (level == FillerWordsLevel::hard || level == FillerWordsLevel::extreme) {
        const auto hard = entries_of(hard_words(), Entry::Position::insertion);
        entries.insert(entries.end(), hard.begin(), hard.end());
    }
    if (level == FillerWordsLevel::extreme) {
        const auto extreme = entries_of(extreme_words(), Entry::Position::sentence_start);
        entries.insert(entries.end(), extreme.begin(), extreme.end());
    }
    // User entries are the user's own decision: applied wherever they match.
    const auto user = entries_of(user_entries, Entry::Position::anywhere);
    entries.insert(entries.end(), user.begin(), user.end());

    std::vector<bool> remove(words.size(), false);
    std::size_t removals = 0;

    for (const auto& entry : entries) {
        for (std::size_t index = 0; index + entry.words.size() <= words.size(); ++index) {
            bool match = true;
            for (std::size_t offset = 0; offset < entry.words.size(); ++offset) {
                if (words[index + offset].lower != entry.words[offset]) {
                    match = false;
                    break;
                }
            }
            if (!match || protected_at(points, words, index, entry.words)
                || !allowed_position(points, words, index, entry.position)) {
                continue;
            }
            for (std::size_t offset = 0; offset < entry.words.size(); ++offset) {
                if (!remove[index + offset]) {
                    remove[index + offset] = true;
                    ++removals;
                }
            }
        }
    }

    // A word that is one letter repeated four or more times is the whisper
    // failure mode ("Ааааа...", "Мммм"), never a word.
    for (std::size_t index = 0; index < words.size(); ++index) {
        if (remove[index] || words[index].end - words[index].begin < 4) {
            continue;
        }
        bool repeated = true;
        const char32_t first = to_lower(points[words[index].begin]);
        for (std::size_t point = words[index].begin + 1; point < words[index].end; ++point) {
            if (to_lower(points[point]) != first) {
                repeated = false;
                break;
            }
        }
        if (repeated) {
            remove[index] = true;
            ++removals;
        }
    }

    if (level == FillerWordsLevel::extreme) {
        for (std::size_t index = 1; index < words.size(); ++index) {
            if (remove[index] || words[index].lower != words[index - 1].lower) {
                continue;
            }
            if (word_has_digit(points, words[index])) {
                continue; // "100, 100" is data, not a stutter
            }
            if (!repeat_separator(points, words[index - 1].end, words[index].begin)) {
                continue; // "Я-я" keeps its hyphen and stays as the engine wrote it
            }
            remove[index] = true;
            ++removals;
        }
    }

    if (removals == 0) {
        return text; // nothing matched: byte-identical, as promised
    }

    bool removed_at_sentence_start = false;
    std::vector<char32_t> result =
        without_removed(points, words, remove, removed_at_sentence_start);
    cleanup(result);
    restore_capitals(result, removed_at_sentence_start);
    return encode(result);
}

std::vector<std::string> meaning_words(std::string_view text,
    const std::vector<std::string>& user_entries)
{
    const std::vector<char32_t> points = decode(text);
    const std::vector<Word> words = split_words(points);
    if (words.empty()) {
        return {};
    }

    // Every level, everywhere: this is not the removal pass. A word that is a filler at all may
    // disappear from the model's answer, whatever position it stood in.
    std::vector<Entry> entries = entries_of(soft_words(), Entry::Position::anywhere);
    const auto hard = entries_of(hard_words(), Entry::Position::anywhere);
    entries.insert(entries.end(), hard.begin(), hard.end());
    const auto extreme = entries_of(extreme_words(), Entry::Position::anywhere);
    entries.insert(entries.end(), extreme.begin(), extreme.end());
    const auto user = entries_of(user_entries, Entry::Position::anywhere);
    entries.insert(entries.end(), user.begin(), user.end());

    std::vector<bool> filler(words.size(), false);
    for (const auto& entry : entries) {
        for (std::size_t index = 0; index + entry.words.size() <= words.size(); ++index) {
            bool match = true;
            for (std::size_t offset = 0; offset < entry.words.size(); ++offset) {
                if (words[index + offset].lower != entry.words[offset]) {
                    match = false;
                    break;
                }
            }
            if (!match) {
                continue;
            }
            for (std::size_t offset = 0; offset < entry.words.size(); ++offset) {
                filler[index + offset] = true;
            }
        }
    }

    std::vector<std::string> result;
    result.reserve(words.size());
    for (std::size_t index = 0; index < words.size(); ++index) {
        if (!filler[index]) {
            result.push_back(words[index].lower);
        }
    }
    return result;
}

} // namespace voicetyper::domain
