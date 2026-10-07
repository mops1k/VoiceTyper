#pragma once

// The terms dictionary: what a dictation should spell a certain way.
//
// The settings field (settings.termsDictionary, the "Словарь терминов" editor)
// accepts two kinds of entry, separated by commas or newlines:
//
//   * a plain term - "API", "commit". It only feeds the initial prompt of an
//     engine that supports one, which biases the decoder towards the Latin
//     spelling of technical words.
//   * a pair "as heard=as written" - "комит=commit". After recognition the
//     as-heard spelling is replaced by the as-written one, exactly as the user
//     typed it.
//
// Why both: measured on real speech (Windows, Microsoft Irina ru-RU fixture,
// whisper small q8), the language of a dictation forces a strong Cyrillic prior,
// so an initial prompt alone leaves "комит" as "комит" - the prompt is a hint,
// not a guarantee. The pair mechanism is deterministic: nothing is guessed, a
// replacement happens only where the user wrote a pair. That is why the prompt
// stays for what it can do (helping the model produce Latin terms on its own)
// and the pairs carry the promise.
//
// Evidence: docs/migration/cpp/parity-ledger.md, row "Terms dictionary".

#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::domain {

/// One explicit "as heard=as written" rule.
struct TermReplacement {
    /// What the engine produces, lower-cased (matching is case-insensitive).
    std::string heard;
    /// What the text must say instead, verbatim as the user typed it.
    std::string written;
};

struct TermsDictionary {
    /// Terms for the engine prompt, in the order the user wrote them.
    std::vector<std::string> terms;
    /// Explicit replacements, in the order the user wrote them.
    std::vector<TermReplacement> replacements;

    [[nodiscard]] bool empty() const noexcept
    {
        return terms.empty() && replacements.empty();
    }
};

/// Parses the settings string. Leading and trailing whitespace of every entry is
/// dropped, empty entries are skipped, an entry with several '=' splits at the
/// first one, duplicates keep their first occurrence. A string without any '='
/// behaves exactly as it did before the pairs existed.
[[nodiscard]] TermsDictionary parse_terms_dictionary(std::string_view text);

/// The initial prompt for an engine that supports one (Whisper). Built as a
/// natural phrase that carries the Latin spellings rather than as a bare list:
/// a prompt is read by the model as previous text, and a sentence keeps the
/// script of the terms more reliably. Empty dictionary, empty prompt.
[[nodiscard]] std::string terms_initial_prompt(const TermsDictionary& dictionary);

/// Applies the explicit replacements to a finished transcript. Matching is
/// case-insensitive and whole-word only (Cyrillic included: the check is done on
/// code points, not bytes, so no locale or regex boundary is involved). The
/// written side is used verbatim; only a word that was already capitalised in
/// the transcript keeps a capital. Text with no match is returned unchanged.
[[nodiscard]] std::string apply_terms(const std::string& text, const TermsDictionary& dictionary);

} // namespace voicetyper::domain
