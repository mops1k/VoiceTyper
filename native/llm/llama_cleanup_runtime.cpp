// Implementation of the domain cleanup port over the pinned llama.cpp runtime.
//
// Everything here is best-effort by contract: a failure is a `false` plus a
// short reason, never an exception, never an abort and never an unbounded wait.
// The port in src/domain/cleanup_runtime.hpp documents that policy; this file
// is where it is actually kept.

#include "llama_cleanup_runtime.hpp"

#include "domain/cleanup_guard.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <utility>

namespace voicetyper::llm {
namespace {

/// The instruction the model is given. Kept short and imperative on purpose:
/// the cleanup models are 0.5-3B instruct models, and a long prompt costs
/// seconds of CPU per transcript. It states the four things the port's guards
/// rely on - only the rewritten text, no commentary, no markdown, numbers
/// untouched - and, since 17.10.2026, the invariant the guards measure: the
/// answer is the speaker's sentence, not a new one.
///
/// The ban on paraphrasing is written out because the model did exactly that
/// without it. Alexander dictated "перезапустил, выпускай" and the small and
/// medium models answered "Запускай." or "Перезапустить." - one verb instead of
/// two, and the meaning of the phrase gone. "Оговорки" used to be in the list of
/// things to remove; deciding that a word was a slip is deciding what the
/// speaker meant, which is the rephrasing this instruction now forbids, so the
/// word is gone from the list on purpose.
constexpr std::string_view kSystemPrompt =
    "Ты редактор расшифровок речи. Твоя правка минимальна: убери слова-паразиты и повторы, "
    "исправь очевидные ошибки распознавания и расставь знаки препинания. "
    "Запрещено: перефразировать, заменять слова синонимами, менять их форму и порядок, "
    "выбрасывать и добавлять смысловые слова, склеивать и разбивать предложения. "
    "Каждое смысловое слово сказанного остаётся в ответе, смысл не меняется. "
    "Верни ТОЛЬКО переписанный текст, без пояснений, без кавычек и без разметки. "
    "Числа и их порядок не меняй. Язык ответа не меняй.";

bool is_space(char c) noexcept
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string trimmed(std::string text)
{
    const auto first = std::find_if_not(text.begin(), text.end(), is_space);
    if (first == text.end()) {
        return {};
    }
    const auto last = std::find_if_not(text.rbegin(), text.rend(), is_space).base();
    return std::string(first, last);
}

/// Removes the wrappers an instruct model adds around an otherwise clean answer:
/// a full pair of quotes, the "assistant:" role prefix, a stray end-of-turn
/// marker. An answer is only unwrapped when the transcript did not already use
/// the very same wrapper, so a genuinely quoted transcript survives.
std::string tidy_answer(std::string answer, const std::string& source)
{
    answer = trimmed(std::move(answer));

    constexpr std::string_view kEndOfTurn = "<|im_end|>";
    if (const auto marker = answer.rfind(kEndOfTurn); marker != std::string::npos
        && marker + kEndOfTurn.size() == answer.size()) {
        answer = trimmed(answer.substr(0, marker));
    }

    const auto strip_prefix = [&answer](std::string_view prefix) {
        if (answer.size() >= prefix.size() && answer.compare(0, prefix.size(), prefix) == 0) {
            answer = trimmed(answer.substr(prefix.size()));
            return true;
        }
        return false;
    };
    strip_prefix("assistant:");

    if (answer.size() >= 2) {
        // Straight double quotes: unwrap when the source itself is not quoted.
        if (answer.front() == '"' && answer.back() == '"' && source.find('"') == std::string::npos) {
            answer = trimmed(answer.substr(1, answer.size() - 2));
        }
        // Russian quotes « ... »: two bytes each in UTF-8.
        constexpr std::string_view kOpen = "\xC2\xAB";   // «
        constexpr std::string_view kClose = "\xC2\xBB";  // »
        if (answer.size() >= kOpen.size() + kClose.size()
            && answer.compare(0, kOpen.size(), kOpen) == 0
            && answer.compare(answer.size() - kClose.size(), kClose.size(), kClose) == 0
            && source.find("\xC2\xAB") == std::string::npos) {
            answer = trimmed(answer.substr(kOpen.size(), answer.size() - kOpen.size() - kClose.size()));
        }
    }
    return answer;
}

/// Token budget of one answer: generous for the transcript that is being
/// rewritten, but never the full default cap. UTF-8 Russian runs about two
/// bytes per character and less than one token per character, so half the byte
/// length plus a margin covers a rewrite; the model stops at its end-of-turn
/// token long before the budget is reached.
int output_budget(std::size_t source_bytes, int cap)
{
    const std::size_t estimate = source_bytes / 2 + 64;
    const int bounded = static_cast<int>(std::min<std::size_t>(estimate, static_cast<std::size_t>(cap)));
    return std::max(bounded, 64);
}

/// The user's addition to the prompt is appended, never substituted, and is bounded: a runaway
/// paste must not push the built-in instruction out of the model's context.
constexpr std::size_t kMaxPromptAppendixBytes = 400;

/// Adds the user's requirements after the built-in instruction. An empty (or whitespace-only)
/// addition adds nothing at all, and the reminder at the end keeps the hierarchy explicit: the
/// addition may ask for more (tidier wording, fewer repetitions), it cannot lift the invariants
/// the guard enforces (digits and their order, the meaning words of the transcript, language of
/// the answer).
void append_user_prompt(std::string& prompt, std::string_view appendix)
{
    std::size_t first = 0;
    std::size_t last = appendix.size();
    while (first < last && is_space(appendix[first])) {
        ++first;
    }
    while (last > first && is_space(appendix[last - 1])) {
        --last;
    }
    if (first == last) {
        return;
    }
    const std::size_t length = std::min(last - first, kMaxPromptAppendixBytes);
    // Phrased as requirements on the RESULT, and deliberately silent about prompts, instructions
    // and base rules: small models answer that wording with a sentence about not being allowed to
    // change the prompt instead of cleaning the transcript, and that sentence used to reach the
    // text (Alexander, 14.10.2026). The meaning words joined the digits on 17.10.2026, when a
    // dictation came back rephrased: a model that follows a contradictory addition costs quality,
    // never correctness, because the guard refuses the answer on its own.
    prompt.append("\nТребования к результату: ");
    prompt.append(appendix.substr(first, length));
    prompt.append("\nСохрани все числа и их порядок, не заменяй смысловые слова другими "
                  "и не выбрасывай их, ответ дай на языке исходного текста.");
}

} // namespace

struct LlamaCleanupRuntime::Impl {
    LlamaCleanupOptions options;
    mutable std::mutex mutex;
    std::unique_ptr<LlmRuntime> runtime;
    std::string path;
};

LlamaCleanupRuntime::LlamaCleanupRuntime(LlamaCleanupOptions options)
    : impl_(std::make_unique<Impl>())
{
    impl_->options = std::move(options);
    if (impl_->options.max_output_tokens <= 0) {
        impl_->options.max_output_tokens = 512;
    }
    if (impl_->options.timeout.count() <= 0) {
        impl_->options.timeout = std::chrono::milliseconds(30000);
    }
}

LlamaCleanupRuntime::~LlamaCleanupRuntime() = default;

std::string_view LlamaCleanupRuntime::system_prompt() noexcept
{
    return kSystemPrompt;
}

std::string LlamaCleanupRuntime::system_prompt_for(std::string_view transcript)
{
    std::string prompt(kSystemPrompt);
    // The language the answer must stay in is decided by the transcript, not by the interface
    // language: dictation can be in any language whatever the menus say. A phrase too short to
    // decide keeps the plain instruction, and the guard refuses a wrong-language answer anyway.
    const domain::detail::CleanupScriptCounts counts =
        domain::detail::cleanup_script_counts(transcript);
    if (counts.cyrillic > 0 && counts.cyrillic * 2 >= counts.total()) {
        prompt.append(" Отвечай ТОЛЬКО на русском языке: переводить ответ на другой язык нельзя.");
    } else if (counts.latin > 0) {
        prompt.append(
            " Answer in English only: translating the answer into another language is not allowed.");
    }
    return prompt;
}

bool LlamaCleanupRuntime::load(const std::string& path, std::string& error)
{
    error.clear();
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (path.empty()) {
        error = "cleanup model path is empty";
        return false;
    }
    if (impl_->runtime != nullptr && impl_->path == path) {
        // Repeated load() of the same path is explicitly cheap: no weight is
        // read again.
        return true;
    }
    impl_->runtime.reset();
    impl_->path.clear();

    const std::filesystem::path library = impl_->options.library_path.empty()
        ? default_llm_library_path()
        : impl_->options.library_path;

    LlmOptions options;
    options.threads = impl_->options.threads;
    options.context_tokens = impl_->options.context_tokens;
    options.max_output_tokens = impl_->options.max_output_tokens;

    const CancellationToken never_cancelled;
    auto opened = LlmRuntime::open(library, std::filesystem::path(path), options, never_cancelled);
    if (opened.is_error()) {
        error = opened.error().to_string();
        return false;
    }
    impl_->runtime = std::move(opened).value();
    impl_->path = path;
    return true;
}

bool LlamaCleanupRuntime::loaded() const
{
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->runtime != nullptr;
}

std::string LlamaCleanupRuntime::loaded_path() const
{
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->path;
}

bool LlamaCleanupRuntime::clean(std::string_view text, std::string_view prompt_appendix,
    std::string& cleaned, std::string& error)
{
    error.clear();
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->runtime == nullptr) {
        error = "cleanup model is not loaded";
        return false;
    }
    const std::string source(text);
    if (trimmed(source).empty()) {
        // Nothing to clean and nothing to ask the model: an empty transcript
        // must not cost a full inference.
        cleaned.clear();
        return true;
    }

    std::string prompt = system_prompt_for(source);
    append_user_prompt(prompt, prompt_appendix);
    auto formatted = impl_->runtime->apply_chat_template(prompt, source);
    if (formatted.is_error()) {
        error = formatted.error().to_string();
        return false;
    }

    // The port has no cancellation token, so the deadline is the only bound
    // this call can honour; the runtime polls it between decode steps, so an
    // over-long transcript fails in seconds instead of running to the end.
    const CancellationToken never_cancelled;
    auto answer = impl_->runtime->generate(formatted.value(),
                                          output_budget(source.size(), impl_->options.max_output_tokens),
                                          impl_->options.timeout, never_cancelled);
    if (answer.is_error()) {
        error = answer.error().to_string();
        return false;
    }

    std::string tidied = tidy_answer(answer.value(), source);
    if (tidied.empty()) {
        error = "the model returned an empty answer";
        return false;
    }
    cleaned = std::move(tidied);
    return true;
}

void LlamaCleanupRuntime::unload()
{
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->runtime.reset();
    impl_->path.clear();
}

} // namespace voicetyper::llm
