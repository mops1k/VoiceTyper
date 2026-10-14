// Real-runtime contract test of the VoiceTyper local text-cleanup library.
//
// It is opt-in by data, not by build: without VOICETYPER_LLM_MODEL it prints a
// skip reason and passes, so a machine (or CI) without a 0.5 GB GGUF stays
// green, while a developer who has one gets real evidence.
//
// What it proves on that machine:
//   * the shared object next to the executable exports exactly the ABI this
//     build was compiled against, and contains the pinned llama.cpp commit
//     (probe_llm_runtime);
//   * a GGUF model loads once and answers one short prompt;
//   * greedy decoding is deterministic: the same prompt twice gives the same
//     text (the KV cache is cleared between calls, so the second answer does
//     not attend to the first);
//   * a pre-cancelled token never reaches the model and reports cancelled;
//   * a 1 ms deadline on a real prompt reports timeout instead of hanging;
//   * an oversized prompt is rejected as out_of_range before any compute.
//
// What it does NOT prove: cleanup quality. Quality is measured on the 24-phrase
// protocol in docs/migration/cpp/filler-words-measurement.md (Phase E).

#include "llama_cleanup_runtime.hpp"
#include "llm_runtime.hpp"
#include "voicetyper_llm.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

using voicetyper::domain::CancellationSource;
using voicetyper::domain::CancellationToken;
using voicetyper::domain::ErrorCode;
using voicetyper::llm::LlmOptions;
using voicetyper::llm::LlmRuntime;
using voicetyper::llm::probe_llm_runtime;

std::string environment(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

int fail(const std::string& message)
{
    std::cout << "llm-runtime-contract: FAIL - " << message << "\n";
    return 1;
}

} // namespace

int main()
{
    using Clock = std::chrono::steady_clock;

    const std::filesystem::path library_path =
        environment("VOICETYPER_LLM_LIBRARY").empty()
            ? voicetyper::llm::llm_library_beside_executable()
            : std::filesystem::path(environment("VOICETYPER_LLM_LIBRARY"));

    const auto probe = probe_llm_runtime(library_path);
    if (!probe.usable) {
        return fail("runtime library is not usable: " + probe.detail
                    + "\n  expected file: " + library_path.string());
    }
    std::cout << "  library: " << probe.library_path.string() << "\n"
              << "  abi: " << probe.abi_version << " (expected " << VOICETYPER_LLM_ABI_VERSION << ")\n"
              << "  llama.cpp commit: " << probe.pinned_commit << "\n"
              << "  llama.cpp version: " << probe.llama_version << "\n";

    const std::string model_argument = environment("VOICETYPER_LLM_MODEL");
    if (model_argument.empty() || !std::filesystem::is_regular_file(model_argument)) {
        std::cout << "llm-runtime-contract: SKIP (set VOICETYPER_LLM_MODEL to a small GGUF model, "
                     "for example the Qwen2.5-0.5B-Instruct Q4_K_M file)\n";
        return 0;
    }
    const std::filesystem::path model_path(model_argument);
    std::cout << "  model: " << model_path.string() << " ("
              << std::filesystem::file_size(model_path) << " bytes)\n";

    const CancellationToken never_cancelled;
    LlmOptions options;
    options.max_output_tokens = 48;

    const auto load_started = Clock::now();
    auto opened = LlmRuntime::open(library_path, model_path, options, never_cancelled);
    if (opened.is_error()) {
        return fail("open failed: " + std::string(opened.error().message()));
    }
    auto runtime = std::move(opened).value();
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - load_started).count();
    std::cout << "  load: " << load_ms << " ms\n";

    // --- one short inference through the model's own chat template -----------
    const std::string system_prompt =
        "Ты убираешь слова-паразиты. Верни только очищенный текст, без пояснений.";
    const std::string user_text = "Ну, это, как бы, тестовая фраза, вот.";

    auto templated = runtime->apply_chat_template(system_prompt, user_text);
    if (templated.is_error()) {
        return fail("apply_chat_template failed: " + std::string(templated.error().message()));
    }
    std::cout << "  templated prompt: " << templated.value().size() << " bytes\n";

    const auto inference_started = Clock::now();
    auto first = runtime->generate(templated.value(), 32, std::chrono::seconds(120), never_cancelled);
    if (first.is_error()) {
        return fail("generate failed: " + std::string(first.error().message()));
    }
    const auto inference_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - inference_started).count();
    std::cout << "  inference: " << inference_ms << " ms, answer: \""
              << first.value().substr(0, 160) << "\"\n";
    if (first.value().empty()) {
        return fail("the model produced an empty answer for a non-empty prompt");
    }

    // --- determinism ---------------------------------------------------------
    auto second = runtime->generate(templated.value(), 32, std::chrono::seconds(120), never_cancelled);
    if (second.is_error()) {
        return fail("second generate failed: " + std::string(second.error().message()));
    }
    if (second.value() != first.value()) {
        return fail("greedy decoding is not deterministic: \"" + first.value() + "\" != \""
                    + second.value() + "\"");
    }
    std::cout << "  determinism: two calls produced identical text\n";

    // --- cancellation --------------------------------------------------------
    CancellationSource cancelled_source;
    cancelled_source.request_cancellation();
    const CancellationToken cancelled = cancelled_source.token();
    auto cancelled_call = runtime->generate(templated.value(), 32, std::chrono::seconds(120), cancelled);
    if (!cancelled_call.is_error() || cancelled_call.error().code() != ErrorCode::cancelled) {
        return fail("a cancelled token did not report cancelled");
    }
    std::cout << "  cancellation: reported cancelled without running the model\n";

    // --- deadline ------------------------------------------------------------
    auto timed_out = runtime->generate(templated.value(), 64, std::chrono::milliseconds(1), never_cancelled);
    if (!timed_out.is_error() || timed_out.error().code() != ErrorCode::timeout) {
        return fail("a 1 ms deadline did not report timeout");
    }
    std::cout << "  timeout: reported timeout\n";

    // --- prompt length guard -------------------------------------------------
    const std::string oversized(200000, 'x');
    auto too_long = runtime->generate(oversized, 32, std::chrono::seconds(120), never_cancelled);
    if (!too_long.is_error() || too_long.error().code() != ErrorCode::out_of_range) {
        return fail("an oversized prompt did not report out_of_range");
    }
    std::cout << "  length guard: oversized prompt rejected\n";

    // --- the domain port implementation (LlamaCleanupRuntime) ----------------
    // Released first: the cleaner opens its own context, and there is no reason
    // to keep two loaded models while it runs.
    runtime.reset();

    voicetyper::llm::LlamaCleanupOptions cleanup_options;
    cleanup_options.library_path = library_path;
    cleanup_options.timeout = std::chrono::seconds(120);
    voicetyper::llm::LlamaCleanupRuntime cleaner(cleanup_options);
    std::cout << "  CleanupRuntime system prompt: "
              << voicetyper::llm::LlamaCleanupRuntime::system_prompt().size() << " bytes\n";

    // The language of the answer is stated in the prompt and taken from the transcript: live
    // dictation showed the small model answering Russian speech in English (14.10.2026).
    const std::string ru_prompt =
        voicetyper::llm::LlamaCleanupRuntime::system_prompt_for("Ну, как бы, посмотри код.");
    const std::string en_prompt =
        voicetyper::llm::LlamaCleanupRuntime::system_prompt_for("Um, like, look at the code.");
    if (ru_prompt.find("русском") == std::string::npos) {
        return fail("a Cyrillic transcript must ask for a Russian answer");
    }
    if (en_prompt.find("English") == std::string::npos) {
        return fail("a Latin transcript must ask for an English answer");
    }
    std::cout << "  language instruction: ru " << ru_prompt.size() << " bytes, en "
              << en_prompt.size() << " bytes\n";

    std::string error;
    if (!cleaner.load(model_path.string(), error)) {
        return fail("CleanupRuntime::load failed: " + error);
    }
    if (!cleaner.loaded()) {
        return fail("CleanupRuntime::loaded() is false right after a successful load");
    }
    const auto reload_started = Clock::now();
    if (!cleaner.load(model_path.string(), error)) {
        return fail("CleanupRuntime::load with the same path failed: " + error);
    }
    const auto reload_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - reload_started).count();
    std::cout << "  CleanupRuntime reload of the same path: " << reload_ms << " ms (no weight reload)\n";

    std::string cleaned;
    const auto clean_started = Clock::now();
    if (!cleaner.clean("Ну, это, как бы, тестовая фраза, вот.", "", cleaned, error)) {
        return fail("CleanupRuntime::clean failed: " + error);
    }
    const auto clean_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - clean_started).count();
    if (cleaned.empty()) {
        return fail("CleanupRuntime::clean returned an empty answer");
    }
    std::cout << "  CleanupRuntime answer: \"" << cleaned.substr(0, 160) << "\" (" << clean_ms << " ms)\n";

    // An empty transcript must not cost an inference.
    std::string empty_answer = "sentinel";
    if (!cleaner.clean("   ", "", empty_answer, error)) {
        return fail("CleanupRuntime::clean on an empty transcript failed: " + error);
    }
    if (!empty_answer.empty()) {
        return fail("an empty transcript must produce an empty answer");
    }
    std::cout << "  CleanupRuntime: empty transcript answered without inference\n";

    cleaner.unload();
    if (cleaner.loaded()) {
        return fail("CleanupRuntime::unload left the runtime loaded");
    }

    std::cout << "llm-runtime-contract: OK\n";
    return 0;
}
