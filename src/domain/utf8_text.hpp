#pragma once

// UTF-8 as code points, shared by the text post-processors.
//
// Both the terms dictionary and the filler filter match words case-insensitively
// in Russian and English, and both have to know what a word character is. Neither
// is possible on raw bytes: a Cyrillic letter is two bytes, so std::regex word
// boundaries and tolower() are wrong here (and locale dependent). Decoding once
// keeps the rules exact and testable.
//
// The helpers below are the ones terms_dictionary.cpp already carried in its
// anonymous namespace; they live here now so a second post-processor does not
// grow a second, subtly different copy. terms_dictionary.cpp still has its own
// copy and is expected to migrate onto this header (plan p_ffecadcca9af).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::domain::text {

/// Decodes UTF-8 into code points. An invalid byte is kept as its own value so a
/// corrupt text still round-trips instead of losing characters.
[[nodiscard]] inline std::vector<char32_t> decode(std::string_view text)
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

inline void append_utf8(std::string& out, char32_t point)
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

[[nodiscard]] inline std::string encode(const std::vector<char32_t>& points)
{
    std::string out;
    out.reserve(points.size());
    for (const auto point : points) {
        append_utf8(out, point);
    }
    return out;
}

/// ASCII plus Cyrillic А..Я, Ё. Everything else is returned unchanged: the
/// recognisers work in these two scripts and a wrong case fold is worse than none.
[[nodiscard]] inline char32_t to_lower(char32_t point)
{
    if (point >= U'A' && point <= U'Z') {
        return point + 32;
    }
    if (point >= 0x0410 && point <= 0x042F) {
        return point + 0x20;
    }
    if (point == 0x0401) {
        return 0x0451;
    }
    return point;
}

[[nodiscard]] inline char32_t to_upper(char32_t point)
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

[[nodiscard]] inline bool is_upper(char32_t point)
{
    return to_lower(point) != point;
}

[[nodiscard]] inline bool is_letter(char32_t point)
{
    if (point < 0x80) {
        return (point >= U'A' && point <= U'Z') || (point >= U'a' && point <= U'z');
    }
    return point >= 0x0400 && point <= 0x04FF;
}

[[nodiscard]] inline bool is_digit(char32_t point)
{
    return point >= U'0' && point <= U'9';
}

/// Anything a word must not be glued to: letters and digits.
[[nodiscard]] inline bool is_word_character(char32_t point)
{
    return is_digit(point) || is_letter(point);
}

[[nodiscard]] inline bool is_space(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

[[nodiscard]] inline std::string_view trim(std::string_view text)
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

/// Splits an entries string on commas and newlines; entries are trimmed later.
[[nodiscard]] inline std::vector<std::string_view> split_entries(std::string_view text)
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

[[nodiscard]] inline std::string lower_copy(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const auto point : decode(text)) {
        append_utf8(out, to_lower(point));
    }
    return out;
}

} // namespace voicetyper::domain::text
