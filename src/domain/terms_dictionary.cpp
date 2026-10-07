#include "domain/terms_dictionary.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::domain {

namespace {

// ---------------------------------------------------------------------------
// UTF-8 as code points.
//
// Matching has to be case-insensitive for Cyrillic and has to know what a word
// character is. Both are impossible on raw bytes: a Cyrillic letter is two
// bytes, so std::regex word boundaries and tolower() are wrong here (and locale
// dependent). Decoding once keeps the rules exact and testable.
// ---------------------------------------------------------------------------

std::vector<char32_t> decode(std::string_view text)
{
    std::vector<char32_t> points;
    points.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t extra = 0;
        char32_t value = 0;
        if (lead < 0x80) {
            value = lead;
            extra = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            value = lead & 0x1F;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            value = lead & 0x0F;
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            value = lead & 0x07;
            extra = 3;
        } else {
            // An invalid lead byte is kept as-is (as a byte value) so a corrupt
            // text still round-trips instead of losing characters.
            points.push_back(lead);
            ++index;
            continue;
        }
        if (index + extra >= text.size()) {
            points.push_back(lead);
            ++index;
            continue;
        }
        bool valid = true;
        for (std::size_t offset = 1; offset <= extra; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            value = (value << 6) | (next & 0x3F);
        }
        if (!valid) {
            points.push_back(lead);
            ++index;
            continue;
        }
        points.push_back(value);
        index += extra + 1;
    }
    return points;
}

void append_utf8(std::string& out, char32_t point)
{
    if (point < 0x80) {
        out.push_back(static_cast<char>(point));
    } else if (point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (point >> 6)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else if (point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    }
}

std::string encode(const std::vector<char32_t>& points)
{
    std::string out;
    out.reserve(points.size());
    for (const auto point : points) {
        append_utf8(out, point);
    }
    return out;
}

char32_t to_lower(char32_t point)
{
    if (point >= U'A' && point <= U'Z') {
        return point + 32;
    }
    // Cyrillic А..Я and Ё: the same offset as ASCII, with Ё handled separately.
    if (point >= 0x0410 && point <= 0x042F) {
        return point + 0x20;
    }
    if (point == 0x0401) {
        return 0x0451;
    }
    return point;
}

char32_t to_upper(char32_t point)
{
    if (point >= U'a' && point <= U'z') {
        return point - 32;
    }
    if (point >= 0x0430 && point <= 0x044F) {
        return point - 0x20;
    }
    if (point == 0x0451) {
        return 0x0401;
    }
    return point;
}

bool is_upper(char32_t point)
{
    return to_lower(point) != point;
}

bool is_letter(char32_t point)
{
    if (point < 0x80) {
        return (point >= U'A' && point <= U'Z') || (point >= U'a' && point <= U'z');
    }
    return point >= 0x0400 && point <= 0x04FF;
}

/// A word character is anything a term must not be glued to: letters and digits
/// (so "комитный" is one word and stays intact, and "комит2" is not a word).
bool is_word_character(char32_t point)
{
    if (point < 0x80) {
        return (point >= U'0' && point <= U'9') || (point >= U'A' && point <= U'Z')
            || (point >= U'a' && point <= U'z');
    }
    return point >= 0x0400 && point <= 0x04FF;
}

bool all_upper(const std::vector<char32_t>& points, std::size_t from, std::size_t count)
{
    bool any_letter = false;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const char32_t point = points[from + offset];
        if (!is_letter(point)) {
            continue;
        }
        any_letter = true;
        if (to_lower(point) == point) {
            return false;
        }
    }
    return any_letter;
}

bool is_space(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

std::string_view trim(std::string_view text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin])) {
        ++begin;
    }
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::vector<std::string_view> split_entries(std::string_view text)
{
    std::vector<std::string_view> entries;
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        const bool separator = index == text.size() || text[index] == ',' || text[index] == '\n';
        if (separator) {
            entries.push_back(text.substr(begin, index - begin));
            begin = index + 1;
        }
    }
    return entries;
}

std::string lower_copy(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const auto point : decode(text)) {
        append_utf8(out, to_lower(point));
    }
    return out;
}

bool contains_ignoring_case(const std::vector<std::string>& values, std::string_view candidate)
{
    const std::string lowered = lower_copy(candidate);
    return std::any_of(values.begin(), values.end(), [&lowered](const std::string& value) {
        return lower_copy(value) == lowered;
    });
}

void push_unique_ignoring_case(std::vector<std::string>& values, std::string_view candidate)
{
    if (candidate.empty() || contains_ignoring_case(values, candidate)) {
        return;
    }
    values.emplace_back(candidate);
}

// ---------------------------------------------------------------------------
// Similarity.
//
// An engine writes the same term differently: "commit" comes back as "comit",
// "comite" or as a Cyrillic rendering. Comparing spellings is therefore not
// enough. Two independent signals are required and both must agree:
//
//   1. the consonant skeleton after transliteration - sounds survive spelling
//      ("commit" -> kmt, "комит" -> kmt, but "комик" -> kmk is a different word);
//   2. the distance between the full transliterated forms - a small edit budget
//      that rejects a word which merely shares the skeleton ("комитет").
//
// Guard rails on purpose: a skeleton of fewer than three consonants is never
// matched by similarity (that is why "API", "CPU" and "IDE" need an explicit
// pair), the forms may not differ by more than three letters, and a word that
// matches two different entries is left alone instead of guessed.
// ---------------------------------------------------------------------------

std::string collapse_doubles(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char byte : text) {
        if (out.empty() || out.back() != byte) {
            out.push_back(byte);
        }
    }
    return out;
}

std::string normalized_form(std::string_view word)
{
    const auto points = decode(word);
    std::string out;
    out.reserve(points.size());
    for (std::size_t index = 0; index < points.size(); ++index) {
        const char32_t point = to_lower(points[index]);
        // The Cyrillic digraph "дж" sounds like the Latin "j" ("джсон" -> json).
        if (point == 0x0434 && index + 1 < points.size() && to_lower(points[index + 1]) == 0x0436) {
            out.push_back('j');
            ++index;
            continue;
        }
        if (point < 0x80) {
            const char byte = static_cast<char>(point);
            if (byte < 'a' || byte > 'z') {
                continue;
            }
            // Latin digraphs are read as one sound, exactly like the Cyrillic
            // "дж": otherwise "PUSH" would normalize to "puskh" and never match
            // the Russian "пуш" it is written as in a dictation.
            const char next = index + 1 < points.size() ? static_cast<char>(to_lower(points[index + 1])) : '\0';
            if (next == 'h') {
                switch (byte) {
                case 's': out += "sh"; ++index; continue;
                case 'c': out += "ch"; ++index; continue;
                case 'z': out += "zh"; ++index; continue;
                case 'k': out += "kh"; ++index; continue;
                case 'p': out.push_back('f'); ++index; continue;
                case 't':
                case 'g':
                    out.push_back(byte);
                    ++index;
                    continue;
                default:
                    break;
                }
            }
            switch (byte) {
            case 'c':
            case 'q':
                out.push_back('k');
                break;
            case 'w':
                out.push_back('v');
                break;
            case 'x':
                out += "ks";
                break;
            case 'y':
                out.push_back('i');
                break;
            case 'h':
                out += "kh";
                break;
            default:
                out.push_back(byte);
                break;
            }
            continue;
        }
        switch (point) {
        case 0x0430: out.push_back('a'); break; // а
        case 0x0431: out.push_back('b'); break; // б
        case 0x0432: out.push_back('v'); break; // в
        case 0x0433: out.push_back('g'); break; // г
        case 0x0434: out.push_back('d'); break; // д
        case 0x0435: out.push_back('e'); break; // е
        case 0x0451: out.push_back('e'); break; // ё
        case 0x0436: out += "zh"; break;        // ж
        case 0x0437: out.push_back('z'); break; // з
        case 0x0438: out.push_back('i'); break; // и
        case 0x0439: out.push_back('i'); break; // й
        case 0x043a: out.push_back('k'); break; // к
        case 0x043b: out.push_back('l'); break; // л
        case 0x043c: out.push_back('m'); break; // м
        case 0x043d: out.push_back('n'); break; // н
        case 0x043e: out.push_back('o'); break; // о
        case 0x043f: out.push_back('p'); break; // п
        case 0x0440: out.push_back('r'); break; // р
        case 0x0441: out.push_back('s'); break; // с
        case 0x0442: out.push_back('t'); break; // т
        case 0x0443: out.push_back('u'); break; // у
        case 0x0444: out.push_back('f'); break; // ф
        case 0x0445: out += "kh"; break;        // х
        case 0x0446: out += "ts"; break;        // ц
        case 0x0447: out += "ch"; break;        // ч
        case 0x0448: out += "sh"; break;        // ш
        case 0x0449: out += "shch"; break;      // щ
        case 0x044a: break;                     // ъ
        case 0x044b: out.push_back('i'); break; // ы
        case 0x044c: break;                     // ь
        case 0x044d: out.push_back('e'); break; // э
        case 0x044e: out.push_back('u'); break; // ю
        case 0x044f: out.push_back('a'); break; // я
        default: break;
        }
    }
    return collapse_doubles(out);
}

std::string consonant_skeleton(std::string_view word)
{
    const std::string form = normalized_form(word);
    std::string consonants;
    consonants.reserve(form.size());
    for (const char byte : form) {
        if (byte == 'a' || byte == 'e' || byte == 'i' || byte == 'o' || byte == 'u') {
            continue;
        }
        consonants.push_back(byte);
    }
    return collapse_doubles(consonants);
}

std::size_t edit_distance(const std::string& left, const std::string& right)
{
    std::vector<std::size_t> previous(right.size() + 1);
    std::vector<std::size_t> current(right.size() + 1);
    for (std::size_t index = 0; index <= right.size(); ++index) {
        previous[index] = index;
    }
    for (std::size_t row = 1; row <= left.size(); ++row) {
        current[0] = row;
        for (std::size_t column = 1; column <= right.size(); ++column) {
            const std::size_t substitution =
                previous[column - 1] + (left[row - 1] == right[column - 1] ? 0 : 1);
            current[column] = std::min({previous[column] + 1, current[column - 1] + 1, substitution});
        }
        std::swap(previous, current);
    }
    return previous[right.size()];
}

/// How many single-letter edits a form of this length may differ by. Short words
/// get no budget at all, otherwise one wrong consonant would be enough.
std::size_t edit_budget(std::size_t form_length)
{
    const std::size_t base = form_length <= 4 ? 0 : (form_length <= 7 ? 1 : 2);
    return std::min(base, form_length / 4);
}

constexpr std::size_t kMinimumSkeleton = 3;
constexpr std::size_t kMaximumLengthDifference = 3;

/// The prompt is read by Whisper as text it has already produced, so it is a
/// sentence that carries the target spellings rather than a bare list. It is
/// bounded: Whisper tokenises a limited prompt, and a dictionary with hundreds of
/// terms must be cut here, visibly, instead of being truncated inside the engine.
constexpr std::size_t kPromptByteBudget = 400;

} // namespace

TermsDictionary parse_terms_dictionary(std::string_view text)
{
    TermsDictionary dictionary;
    for (const auto raw : split_entries(text)) {
        const auto entry = trim(raw);
        if (entry.empty()) {
            continue;
        }
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            push_unique_ignoring_case(dictionary.terms, entry);
            continue;
        }
        const auto heard = trim(entry.substr(0, equals));
        const auto written = trim(entry.substr(equals + 1));
        // A rule with an empty side would either never match or delete words:
        // both are worse than ignoring the entry.
        if (heard.empty() || written.empty()) {
            continue;
        }
        const std::string heard_lower = lower_copy(heard);
        const bool duplicate = std::any_of(dictionary.replacements.begin(),
            dictionary.replacements.end(),
            [&heard_lower](const TermReplacement& rule) { return rule.heard == heard_lower; });
        if (!duplicate) {
            dictionary.replacements.push_back(TermReplacement{heard_lower, std::string(written)});
        }
    }
    return dictionary;
}

std::string terms_initial_prompt(const TermsDictionary& dictionary)
{
    if (dictionary.empty()) {
        return std::string();
    }
    std::vector<std::string> wanted;
    for (const auto& term : dictionary.terms) {
        push_unique_ignoring_case(wanted, term);
    }
    for (const auto& rule : dictionary.replacements) {
        push_unique_ignoring_case(wanted, rule.written);
    }
    if (wanted.empty()) {
        return std::string();
    }

    std::string prompt = "Термины пишутся латиницей: ";
    bool first = true;
    for (const auto& term : wanted) {
        const std::size_t addition = term.size() + (first ? 0 : 2);
        if (prompt.size() + addition + 1 > kPromptByteBudget) {
            break;
        }
        if (!first) {
            prompt += ", ";
        }
        prompt += term;
        first = false;
    }
    prompt += '.';
    return prompt;
}

std::string apply_terms(const std::string& text, const TermsDictionary& dictionary)
{
    if (text.empty() || dictionary.empty()) {
        return text;
    }

    struct CompiledRule {
        std::vector<char32_t> heard;
        std::vector<char32_t> written;
    };
    struct FuzzyTarget {
        std::string written;
        std::string form;
        std::string skeleton;
    };

    std::vector<CompiledRule> rules;
    rules.reserve(dictionary.replacements.size());
    for (const auto& rule : dictionary.replacements) {
        rules.push_back(CompiledRule{decode(rule.heard), decode(rule.written)});
    }
    // Longest heard side first, so "джи сон" is not consumed by a shorter "джи".
    std::stable_sort(rules.begin(), rules.end(), [](const CompiledRule& left, const CompiledRule& right) {
        return left.heard.size() > right.heard.size();
    });

    // Every entry takes part in similarity matching: a plain term by its own
    // spelling, a pair by the spelling the user wants in the text.
    std::vector<FuzzyTarget> targets;
    for (const auto& term : dictionary.terms) {
        targets.push_back(FuzzyTarget{term, normalized_form(term), consonant_skeleton(term)});
    }
    for (const auto& rule : dictionary.replacements) {
        targets.push_back(
            FuzzyTarget{rule.written, normalized_form(rule.written), consonant_skeleton(rule.written)});
    }

    const std::vector<char32_t> source = decode(text);
    std::vector<char32_t> lowered_points;
    lowered_points.reserve(source.size());
    for (const auto point : source) {
        lowered_points.push_back(to_lower(point));
    }

    const auto matches_at = [&source, &lowered_points](std::size_t position,
                                  const std::vector<char32_t>& heard) {
        if (position + heard.size() > source.size()) {
            return false;
        }
        for (std::size_t offset = 0; offset < heard.size(); ++offset) {
            char32_t expected = heard[offset];
            // A space inside a rule matches any single space character.
            if (expected == U' ') {
                if (source[position + offset] != U' ') {
                    return false;
                }
                continue;
            }
            expected = to_lower(expected);
            if (lowered_points[position + offset] != expected) {
                return false;
            }
        }
        return true;
    };

    const auto at_word_start = [&source](std::size_t position) {
        return position == 0 || !is_word_character(source[position - 1]);
    };
    const auto at_word_end = [&source](std::size_t position) {
        return position >= source.size() || !is_word_character(source[position]);
    };

    std::vector<char32_t> result;
    result.reserve(source.size());
    std::size_t index = 0;
    while (index < source.size()) {
        bool replaced = false;
        if (at_word_start(index)) {
            for (const auto& rule : rules) {
                if (matches_at(index, rule.heard) && at_word_end(index + rule.heard.size())) {
                    // The written side is used exactly as the user typed it. A word
                    // that was capitalised in the transcript (and was not all caps)
                    // keeps that capital.
                    const bool capitalised =
                        is_upper(source[index]) && !all_upper(source, index, rule.heard.size());
                    bool first = true;
                    for (const auto point : rule.written) {
                        result.push_back(first && capitalised ? to_upper(point) : point);
                        first = false;
                    }
                    index += rule.heard.size();
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                // A single word, compared by sound and shape. Digits and
                // punctuation are not part of it, so they are preserved verbatim.
                std::size_t end = index;
                while (end < source.size() && is_letter(source[end])) {
                    ++end;
                }
                if (end > index && at_word_end(end)) {
                    const std::string word = encode(std::vector<char32_t>(
                        source.begin() + static_cast<std::ptrdiff_t>(index),
                        source.begin() + static_cast<std::ptrdiff_t>(end)));
                    const std::string form = normalized_form(word);
                    const std::string skeleton = consonant_skeleton(word);
                    const std::string* match = nullptr;
                    bool ambiguous = false;
                    if (skeleton.size() >= kMinimumSkeleton) {
                        for (const auto& target : targets) {
                            const std::size_t difference = form.size() > target.form.size()
                                ? form.size() - target.form.size()
                                : target.form.size() - form.size();
                            if (target.skeleton != skeleton || difference > kMaximumLengthDifference
                                || edit_distance(form, target.form) > edit_budget(target.form.size())) {
                                continue;
                            }
                            if (match != nullptr && *match != target.written) {
                                ambiguous = true;
                                break;
                            }
                            match = &target.written;
                        }
                    }
                    if (match != nullptr && !ambiguous) {
                        const bool capitalised =
                            is_upper(source[index]) && !all_upper(source, index, end - index);
                        std::size_t written_index = 0;
                        for (const auto point : decode(*match)) {
                            result.push_back(written_index == 0 && capitalised ? to_upper(point) : point);
                            ++written_index;
                        }
                        index = end;
                        replaced = true;
                    }
                }
            }
        }
        if (!replaced) {
            result.push_back(source[index]);
            ++index;
        }
    }

    return encode(result);
}

} // namespace voicetyper::domain
