#pragma once

// Filler-word removal: what a dictation loses before it reaches the clipboard.
//
// Decided by Alexander 2026-10-14 and measured in
// docs/migration/cpp/filler-words-measurement.md:
//   * both engines keep filled pauses ("э-э", "эм-м", "мм") and verbal hesitations
//     ("ну", "вот", "значит", "как бы", "типа", "короче", "в общем"), so a word
//     list applied to the finished text is the only mechanism that works for
//     every engine;
//   * whisper collapses word repetitions itself and can emit a pathological run
//     of one letter ("Ааааа..." for "А-а"), while GigaAM keeps repetitions ("Я-я",
//     "Мы это, это");
//   * both engines mangle numbers, so no rule may touch a digit.
//
// The level is the switch as well: FillerWordsLevel::off means "hand the text on
// exactly as the engine returned it", and that guarantee is literal - with
// nothing removed the input string is returned unchanged, byte for byte.

#include "domain/settings.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::domain {

/// Parses settings.fillerWords: entries separated by commas or newlines, trimmed,
/// empty entries and case-insensitive duplicates dropped, order preserved.
[[nodiscard]] std::vector<std::string> parse_filler_words(std::string_view text);

/// True for every level that removes something.
[[nodiscard]] bool filler_removal_enabled(FillerWordsLevel level) noexcept;

/// Removes filler words from a finished transcript and repairs what the removal
/// left behind.
///
/// Guarantees, most important first:
///   * a digit is never touched and punctuation between two digits is left alone,
///     so "Повтори 100, 100 и 4, 0, 4, 0." survives every level unchanged;
///   * protected combinations are never shortened: "короче говоря", "это значит",
///     "ну-ка", "как бы то ни было";
///   * a single-word entry of the `hard` level and every entry of the `extreme`
///     level are removed only in an introductory position (first word of a
///     sentence, or a word standing next to a comma), so "в целом" and a
///     "значит" that is a verb stay;
///   * a word that is one letter repeated four or more times is removed whole
///     ("Ааааа", "Мммм"): that is the whisper failure mode, not a word;
///   * at the `extreme` level a word repeated immediately after itself loses the
///     second copy - unless the word carries a digit, which is never touched;
///   * user entries are added to the built-in list, never replace it;
///   * applying the pass to its own output changes nothing (idempotent).
[[nodiscard]] std::string apply_filler_words(const std::string& text, FillerWordsLevel level,
    const std::vector<std::string>& user_entries);

/// The words of `text` that carry the speaker's meaning: every word except the filler entries,
/// lower-cased, in order. Entries of ALL levels count - both the built-in lists and the user's own
/// - and the position rules are deliberately ignored.
///
/// The lenient reading is what the guard above the local cleanup model needs (Alexander,
/// 17.10.2026): the guard must know which words MAY disappear before a sentence stops meaning
/// what the speaker said. A word the rules kept because of its position can still be an insertion
/// the model is right to remove, and refusing that answer would cost the user the cleanup itself.
[[nodiscard]] std::vector<std::string> meaning_words(std::string_view text,
    const std::vector<std::string>& user_entries);

} // namespace voicetyper::domain
