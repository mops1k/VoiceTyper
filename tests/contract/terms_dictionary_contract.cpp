// Contract for the terms dictionary: parsing the settings string, the initial
// prompt it feeds to an engine that supports one, and the explicit
// "as heard=as written" replacements applied to a finished transcript.
//
// Portable and deterministic - no model, no engine, no OS - so it runs in every
// configuration. The replacement is deliberately engine-agnostic: it runs after
// recognition, which is the only mechanism that reaches all three engines
// (measured 2026-10-07: GigaAM reports no vocabulary support and ignores a
// context prompt, and the Parakeet C API has neither).
//
// Prints "terms-dictionary-contract: OK" on success; CTest asserts that marker.

#include "domain/settings.hpp"
#include "domain/terms_dictionary.hpp"
#include "domain/terms_dictionary_port.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using voicetyper::domain::apply_terms;
using voicetyper::domain::TermsDictionaryPort;
using voicetyper::domain::kDefaultTermsDictionary;
using voicetyper::domain::parse_terms_dictionary;
using voicetyper::domain::terms_initial_prompt;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

/// Indexed access that stays safe while the contract is red: a failed size check
/// must not turn into an out-of-range read that hides the remaining failures.
std::string term_at(const voicetyper::domain::TermsDictionary& dictionary, std::size_t index)
{
    return index < dictionary.terms.size() ? dictionary.terms[index] : std::string("<missing>");
}

std::string heard_at(const voicetyper::domain::TermsDictionary& dictionary, std::size_t index)
{
    return index < dictionary.replacements.size() ? dictionary.replacements[index].heard
                                                  : std::string("<missing>");
}

std::string written_at(const voicetyper::domain::TermsDictionary& dictionary, std::size_t index)
{
    return index < dictionary.replacements.size() ? dictionary.replacements[index].written
                                                  : std::string("<missing>");
}

void check_text(const std::string& actual, const std::string& expected, const std::string& what)
{
    if (actual != expected) {
        ++failures;
        std::cerr << "FAIL " << what << "\n  expected: [" << expected << "]\n  actual:   [" << actual
                  << "]\n";
    }
}

/// The default settings value has to keep working exactly as before the pairs
/// existed: a comma-separated list of terms, no replacements.
void check_default_dictionary_is_a_plain_list()
{
    const auto dictionary = parse_terms_dictionary(kDefaultTermsDictionary);
    check(dictionary.terms.size() == 10, "default dictionary: ten terms");
    check(dictionary.replacements.empty(), "default dictionary: no replacements");
    check(term_at(dictionary, 0) == "API", "default dictionary: first term is API");
    check(term_at(dictionary, 9) == "SQL", "default dictionary: last term is SQL");
}

void check_parses_pairs()
{
    const auto dictionary = parse_terms_dictionary("комит=commit, апи=API");
    check(dictionary.replacements.size() == 2, "pairs: two replacements");
    check(heard_at(dictionary, 0) == "комит", "pairs: heard side is stored");
    check(written_at(dictionary, 0) == "commit", "pairs: written side is stored verbatim");
    check(heard_at(dictionary, 1) == "апи", "pairs: second heard side");
    check(written_at(dictionary, 1) == "API", "pairs: written side keeps its case");
    check(dictionary.terms.empty(), "pairs: a pair is not also a prompt term");
}

void check_mixed_separators_and_whitespace()
{
    const auto dictionary = parse_terms_dictionary(
        "  API , комит = commit \n\n JSON,\n, ,GPU ,, ");
    check(dictionary.terms.size() == 3, "mixed: three terms");
    check(term_at(dictionary, 0) == "API" && term_at(dictionary, 1) == "JSON"
              && term_at(dictionary, 2) == "GPU",
        "mixed: order kept, whitespace trimmed, empty entries skipped");
    check(dictionary.replacements.size() == 1, "mixed: one replacement");
    check(heard_at(dictionary, 0) == "комит", "mixed: heard side trimmed");
    check(written_at(dictionary, 0) == "commit", "mixed: written side trimmed");
}

void check_first_equals_wins()
{
    // Latin letters on purpose: the check is about the first '=' deciding the
    // split, not about the script of the sides.
    const auto dictionary = parse_terms_dictionary("a=b=c");
    check(dictionary.replacements.size() == 1, "equals: one replacement");
    check(heard_at(dictionary, 0) == "a", "equals: heard is the part before the first =");
    check(written_at(dictionary, 0) == "b=c", "equals: the rest is the written side");
}

void check_empty_inputs()
{
    for (const char* text : {"", "   ", ",,,", "\n\n", " , \n , "}) {
        const auto dictionary = parse_terms_dictionary(text);
        check(dictionary.empty(), std::string("empty input stays empty: [") + text + "]");
    }
    // A pair with an empty side is not a usable rule and is dropped rather than
    // becoming a replacement that deletes every word.
    const auto half = parse_terms_dictionary("commit=, =commit, =");
    check(half.empty(), "an empty side is not a rule");
}

void check_duplicates_keep_the_first()
{
    const auto dictionary = parse_terms_dictionary("API,api,API\nкомит=commit,комит=commit1");
    check(dictionary.terms.size() == 1, "duplicates: one prompt term");
    check(term_at(dictionary, 0) == "API", "duplicates: the first spelling wins");
    check(dictionary.replacements.size() == 1, "duplicates: one replacement");
    check(written_at(dictionary, 0) == "commit", "duplicates: the first rule wins");
}

void check_replacement_basic()
{
    const auto dictionary = parse_terms_dictionary("комит=commit");
    check_text(apply_terms("Добавь комит в ветку.", dictionary), "Добавь commit в ветку.",
        "replace a whole word");
    check_text(apply_terms("комит", dictionary), "commit", "replace a bare word");
}

/// The written side is used exactly as the user typed it - that is the whole
/// point of the feature (an acronym must come back as an acronym).
void check_written_side_is_verbatim()
{
    const auto dictionary = parse_terms_dictionary("апи=API, джи сон=JSON");
    check_text(apply_terms("Отправь апи на сервер.", dictionary), "Отправь API на сервер.",
        "written side keeps its own case");
    check_text(apply_terms("Отправь джи сон на сервер.", dictionary), "Отправь JSON на сервер.",
        "a multi-word heard side is replaced as one phrase");
}

/// A word that merely starts with a dictionary entry must survive: replacing
/// inside a longer word would corrupt ordinary Russian text.
void check_whole_words_only()
{
    const auto dictionary = parse_terms_dictionary("комит=commit, апи=API");
    check_text(apply_terms("комитный апишка", dictionary), "комитный апишка",
        "no replacement inside a longer word");
    // A hyphen is a word boundary, so a Russian compound is replaced as well
    // ("API-ключ" is the reason this is the wanted behaviour).
    check_text(apply_terms("комит-хвост", dictionary), "commit-хвост",
        "a hyphen ends the word, so a compound is replaced");
    check_text(apply_terms("(комит)", dictionary), "(commit)", "punctuation around a word is fine");
}

void check_case_insensitive_matching()
{
    const auto dictionary = parse_terms_dictionary("комит=commit");
    check_text(apply_terms("КОМИТ", dictionary), "commit", "an upper-case word matches");
    check_text(apply_terms("Комит", dictionary), "Commit", "a capitalised word keeps its capital");
    check_text(apply_terms("КОМИТ, комит, Комит", dictionary), "commit, commit, Commit",
        "all occurrences are replaced, the capital follows the source word");
}

void check_multiple_occurrences_and_other_text_untouched()
{
    const auto dictionary = parse_terms_dictionary("апи=API");
    check_text(apply_terms("апи и ещё раз апи, но не апис", dictionary), "API и ещё раз API, но не апис",
        "every occurrence is replaced, longer words are not");
    check_text(apply_terms("Здесь нет терминов.", dictionary), "Здесь нет терминов.",
        "text without a match is returned byte for byte");
    check_text(apply_terms("", dictionary), "", "empty text stays empty");
}

/// Longest rule first: "джи сон" must not be consumed by a shorter "джи".
void check_longest_rule_wins()
{
    const auto dictionary = parse_terms_dictionary("джи=G, джи сон=JSON");
    check_text(apply_terms("джи сон", dictionary), "JSON", "the longer heard side wins");
    check_text(apply_terms("джи", dictionary), "G", "the shorter rule still applies alone");
}

void check_replacements_do_not_apply_to_the_prompt_terms()
{
    // A plain term is a prompt hint only: it must never rewrite Cyrillic text.
    const auto dictionary = parse_terms_dictionary("API");
    check_text(apply_terms("Отправь апи на сервер.", dictionary), "Отправь апи на сервер.",
        "a plain term is not a replacement rule");
}

/// The engine does not always write a term the way the dictionary spells it: the
/// same word comes back as commit, comit, comite or a Cyrillic rendering. The
/// dictionary has to catch those by sound and shape, not by exact spelling.
void check_similar_words_are_replaced()
{
    const auto dictionary = parse_terms_dictionary("commit, JSON");
    struct Case {
        const char* heard;
        const char* expected;
    };
    for (const Case& item : {
             Case{"commit", "commit"},
             Case{"comit", "commit"},
             Case{"comite", "commit"},
             Case{"Comit", "Commit"},
             Case{"комит", "commit"},
             Case{"коммит", "commit"},
             Case{"камит", "commit"},
             // "камыт" is not a Russian word, and ы is a vowel here: it is
             // another plausible rendering of the same term.
             Case{"камыт", "commit"},
             Case{"Коммит", "Commit"},
             Case{"джсон", "JSON"},
         }) {
        check_text(apply_terms(item.heard, dictionary), item.expected,
            std::string("similar: ") + item.heard + " is corrected");
    }
    check_text(apply_terms("Добавь комит в ветку.", dictionary), "Добавь commit в ветку.",
        "similar: a word inside a sentence is corrected");
}

/// The guards exist to protect ordinary speech: a word that only looks like a
/// term must survive, including the real word "комик" that shares most letters.
/// Latin digraphs are read as one sound, so a term written with "sh" matches the
/// way Russian speech spells it ("PUSH" vs "пуш"). A term whose skeleton is shorter
/// than three consonants stays exact-only, exactly like "API" ("phone", "фон").
void check_latin_digraphs_are_one_sound()
{
    const auto dictionary = parse_terms_dictionary("PUSH, SHIP, phone");
    check_text(apply_terms("пуш в репозиторий", dictionary), "PUSH в репозиторий",
        "digraph: 'пуш' matches PUSH");
    check_text(apply_terms("шип", dictionary), "SHIP", "digraph: 'шип' matches SHIP");
    check_text(apply_terms("пушка", dictionary), "пушка",
        "digraph: a longer word is still not touched");
    check_text(apply_terms("фон", dictionary), "фон",
        "digraph: a two-consonant term is not matched by similarity");
}

void check_lookalikes_are_left_alone()
{
    const auto dictionary = parse_terms_dictionary("commit, JSON");
    for (const char* word : {"комик", "комика", "комок", "камин", "комитет", "комитету", "кот", "дом",
             "команда", "джаз", "камыш"}) {
        check_text(apply_terms(word, dictionary), word,
            std::string("lookalike stays: ") + word);
    }
    check_text(apply_terms("Он комик, а не комит.", dictionary), "Он комик, а не commit.",
        "lookalike: only the term survives correction");
}

void check_similarity_keeps_punctuation()
{
    const auto dictionary = parse_terms_dictionary("commit");
    check_text(apply_terms("Отправь комит, пожалуйста!", dictionary), "Отправь commit, пожалуйста!",
        "punctuation after a corrected word is untouched");
    check_text(apply_terms("(комит)", dictionary), "(commit)", "brackets are untouched");
    check_text(apply_terms("комит.", dictionary), "commit.", "a full stop is untouched");
}

/// A term whose consonant skeleton is shorter than three letters is only ever
/// matched exactly: "API" must not start capturing ordinary Russian words.
void check_short_terms_need_an_exact_pair()
{
    const auto dictionary = parse_terms_dictionary("API, IDE, CPU");
    check_text(apply_terms("апи иде кпу", dictionary), "апи иде кпу",
        "short terms are not matched by similarity");
    const auto with_pairs = parse_terms_dictionary("апи=API, иде=IDE");
    check_text(apply_terms("апи иде кпу", with_pairs), "API IDE кпу",
        "an explicit pair still replaces a short term");
}

/// Two different terms may share a key ("commit" and "comit" normalize the same
/// way): guessing between them would be worse than leaving the word alone.
void check_ambiguous_similarity_is_skipped()
{
    const auto dictionary = parse_terms_dictionary("commit, comit");
    check_text(apply_terms("комит", dictionary), "комит",
        "ambiguous: a word matching two terms is left alone");
    check_text(apply_terms("commit", dictionary), "commit",
        "ambiguous: the spelling that is already a term stays");
}

void check_similarity_is_idempotent_and_ordered()
{
    const auto dictionary = parse_terms_dictionary("commit");
    const std::string once = apply_terms("Добавь комит в ветку.", dictionary);
    const std::string twice = apply_terms(once, dictionary);
    check_text(twice, once, "similarity: a second pass changes nothing");

    // The explicit rule is the stronger promise and wins when both could apply.
    const auto explicit_wins = parse_terms_dictionary("комит=коммит, commit");
    check_text(apply_terms("комит", explicit_wins), "коммит",
        "an explicit pair beats the similarity rule");
}

/// Similarity works word by word; a multi-word entry is only honoured as an
/// explicit pair (there is nothing to compare sound-by-sound otherwise).
void check_multiword_terms_are_exact_only()
{
    const auto plain = parse_terms_dictionary("джи сон");
    check_text(apply_terms("джи сон", plain), "джи сон",
        "a multi-word term without '=' is not a replacement rule");
    const auto pair = parse_terms_dictionary("джи сон=JSON");
    check_text(apply_terms("джи сон", pair), "JSON",
        "a multi-word pair still replaces exactly");
}

void check_prompt_is_built_from_terms()
{
    const auto empty = parse_terms_dictionary("");
    check(terms_initial_prompt(empty).empty(), "prompt: empty dictionary gives an empty prompt");

    const auto dictionary = parse_terms_dictionary("API, CPU, комит=commit");
    const std::string prompt = terms_initial_prompt(dictionary);
    check(!prompt.empty(), "prompt: non-empty dictionary gives a prompt");
    check(prompt.find("API") != std::string::npos, "prompt: carries the first term");
    check(prompt.find("CPU") != std::string::npos, "prompt: carries every term");
    check(prompt.find("commit") != std::string::npos,
        "prompt: the written side of a pair is asked for by its target spelling");
    check(prompt.find("комит") == std::string::npos,
        "prompt: the heard side of a pair is not asked for");
    check(prompt.find('\n') == std::string::npos, "prompt: stays one line");
}

/// Whisper reads a bounded prompt; an over-long dictionary must be cut, not
/// passed through to be silently truncated by the engine.
void check_prompt_is_bounded()
{
    std::string huge;
    for (int index = 0; index < 200; ++index) {
        huge += "term" + std::to_string(index) + ",";
    }
    const std::string prompt = terms_initial_prompt(parse_terms_dictionary(huge));
    check(!prompt.empty(), "bounded prompt: still built");
    check(prompt.size() <= 400, "bounded prompt: at most 400 bytes");
}

/// A port that answers with a fixed transcript. The decorator is tested through
/// a port and not through an engine on purpose: the replacement must be
/// independent of which engine produced the text.
class FixedTranscriptPort final : public voicetyper::domain::TranscriptionPort {
public:
    explicit FixedTranscriptPort(std::string text)
        : text_(std::move(text))
    {
    }

    [[nodiscard]] voicetyper::domain::Result<std::string> transcribe(
        const voicetyper::domain::SampleBuffer&,
        const voicetyper::domain::SessionOptions&,
        const voicetyper::domain::CancellationToken&) override
    {
        return text_;
    }

private:
    std::string text_;
};

void check_port_applies_the_dictionary()
{
    FixedTranscriptPort whisper_like("Добавь комит в ветку.");
    TermsDictionaryPort decorated(whisper_like, [] { return std::string("комит=commit"); });
    const auto result = decorated.transcribe(
        voicetyper::domain::SampleBuffer{}, voicetyper::domain::SessionOptions{},
        voicetyper::domain::CancellationToken{});
    check(result.is_ok(), "port: a successful transcription stays successful");
    check_text(result.is_ok() ? result.value() : std::string(), "Добавь commit в ветку.",
        "port: the dictionary is applied to the engine output");

    // The same decorator around another engine's port behaves identically, which
    // is what "works on every engine" means here.
    FixedTranscriptPort gigaam_like("Отправь апи на сервер.");
    TermsDictionaryPort other(gigaam_like, [] { return std::string("апи=API"); });
    const auto other_result = other.transcribe(
        voicetyper::domain::SampleBuffer{}, voicetyper::domain::SessionOptions{},
        voicetyper::domain::CancellationToken{});
    check_text(other_result.is_ok() ? other_result.value() : std::string(), "Отправь API на сервер.",
        "port: the same decorator works for a different engine");

    // An empty dictionary must not touch the text at all.
    FixedTranscriptPort plain("Добавь комит в ветку.");
    TermsDictionaryPort untouched(plain, [] { return std::string(); });
    const auto plain_result = untouched.transcribe(
        voicetyper::domain::SampleBuffer{}, voicetyper::domain::SessionOptions{},
        voicetyper::domain::CancellationToken{});
    check_text(plain_result.is_ok() ? plain_result.value() : std::string(), "Добавь комит в ветку.",
        "port: without a dictionary the transcript is returned as it was");

    // The provider is read per call, so a settings change reaches the next dictation.
    std::string live = "комит=commit";
    FixedTranscriptPort changing("комит");
    TermsDictionaryPort live_port(changing, [&live] { return live; });
    const auto first = live_port.transcribe(voicetyper::domain::SampleBuffer{},
        voicetyper::domain::SessionOptions{}, voicetyper::domain::CancellationToken{});
    check_text(first.is_ok() ? first.value() : std::string(), "commit",
        "port: first call uses the current dictionary");
    live = "комит=коммит";
    const auto second = live_port.transcribe(voicetyper::domain::SampleBuffer{},
        voicetyper::domain::SessionOptions{}, voicetyper::domain::CancellationToken{});
    check_text(second.is_ok() ? second.value() : std::string(), "коммит",
        "port: a changed dictionary applies to the next call");
}

/// A failure must travel through the decorator unchanged: no text, no rewrite.
class FailingPort final : public voicetyper::domain::TranscriptionPort {
public:
    [[nodiscard]] voicetyper::domain::Result<std::string> transcribe(
        const voicetyper::domain::SampleBuffer&,
        const voicetyper::domain::SessionOptions&,
        const voicetyper::domain::CancellationToken&) override
    {
        return voicetyper::domain::Result<std::string>::failure(
            voicetyper::domain::ErrorCode::engine_unavailable, "no engine");
    }
};

/// The dictionary of a user who wrote plain terms only: no '=' anywhere, and yet
/// the correction has to happen. This is the case that reached the product broken
/// (reported 2026-10-07: "Комит пуш" stayed Cyrillic), because the port returned
/// early whenever the replacement list was empty and the similarity rule never ran.
void check_port_corrects_plain_terms()
{
    FixedTranscriptPort engine_like("Добавь комит в ветку и сделай пуш.");
    TermsDictionaryPort decorated(engine_like, [] { return std::string("COMMIT, PUSH"); });
    const auto result = decorated.transcribe(
        voicetyper::domain::SampleBuffer{}, voicetyper::domain::SessionOptions{},
        voicetyper::domain::CancellationToken{});
    check(result.is_ok(), "port plain: a successful transcription stays successful");
    check_text(result.is_ok() ? result.value() : std::string(),
        "Добавь COMMIT в ветку и сделай PUSH.",
        "port plain: terms without '=' are corrected by similarity");
}

void check_port_passes_failures_through()
{
    FailingPort failing;
    TermsDictionaryPort decorated(failing, [] { return std::string("комит=commit"); });
    const auto result = decorated.transcribe(
        voicetyper::domain::SampleBuffer{}, voicetyper::domain::SessionOptions{},
        voicetyper::domain::CancellationToken{});
    check(result.is_error(), "port: a failure is still a failure");
    check(result.is_error() && result.error().code() == voicetyper::domain::ErrorCode::engine_unavailable,
        "port: the failure code is preserved");
}

} // namespace

int main()
{
    check_default_dictionary_is_a_plain_list();
    check_parses_pairs();
    check_mixed_separators_and_whitespace();
    check_first_equals_wins();
    check_empty_inputs();
    check_duplicates_keep_the_first();
    check_replacement_basic();
    check_written_side_is_verbatim();
    check_whole_words_only();
    check_case_insensitive_matching();
    check_multiple_occurrences_and_other_text_untouched();
    check_longest_rule_wins();
    check_replacements_do_not_apply_to_the_prompt_terms();
    check_prompt_is_built_from_terms();
    check_prompt_is_bounded();
    check_similar_words_are_replaced();
    check_latin_digraphs_are_one_sound();
    check_lookalikes_are_left_alone();
    check_similarity_keeps_punctuation();
    check_short_terms_need_an_exact_pair();
    check_ambiguous_similarity_is_skipped();
    check_similarity_is_idempotent_and_ordered();
    check_multiword_terms_are_exact_only();
    check_port_applies_the_dictionary();
    check_port_corrects_plain_terms();
    check_port_passes_failures_through();

    if (failures != 0) {
        std::cerr << "terms-dictionary-contract: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "terms-dictionary-contract: OK\n";
    return 0;
}
