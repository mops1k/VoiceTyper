#pragma once

// Domain port of the local text-cleanup model, implemented over the pinned
// llama.cpp runtime (native/llm/voicetyper_llm.h + llm_runtime.hpp).
//
// Why this class exists next to the loader: src/domain/cleanup_runtime.hpp is
// the only interface the port and its guards know, so the mapping from "one
// finished transcript" to "one greedy completion of an instruct model" lives
// here, in the native module - not in the domain layer and not in the loader.
// The loader stays a transport (open/generate/close), this class owns the
// prompt, the sampling parameters and the answer hygiene.
//
// Failure policy, as the port requires: nothing throws, nothing aborts, nothing
// blocks for ever. load()/clean() return false with a short reason, and the
// caller delivers what its rules produced. A model that is missing, corrupt,
// too slow or refuses to answer is therefore a normal outcome, not an error
// path in the product.
//
// Sampling: the runtime is deliberately deterministic - greedy (argmax)
// decoding, temperature 0, no top_p/top_k, no repetition penalty and no
// sampling seed. Determinism is what makes "digits are untouchable" testable:
// the same transcript always produces the same answer.
//
// Stop conditions: there is no string stop-sequence mechanism, by design. The
// only stop is the model's own end-of-generation token plus the output-token
// budget. A leading/trailing wrapper the model likes to add (surrounding quotes,
// a stray end-of-turn marker, the "assistant:" prefix) is removed by the answer
// tidy-up in the .cpp, so no text-level marker has to be configured.

#include "domain/cleanup_runtime.hpp"
#include "llm_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace voicetyper::llm {

/// Tunables of one cleaner. Defaults are the ones the product ships with.
struct LlamaCleanupOptions {
    /// Path of the runtime shared object. Empty selects
    /// default_llm_library_path() (VOICETYPER_LLM_LIBRARY, else the library
    /// beside the executable).
    std::filesystem::path library_path;
    /// Worker threads; 0 selects the runtime default (min(4, hw)).
    int threads = 0;
    /// Prompt+answer window; 0 selects the runtime default (min(4096, trained)).
    int context_tokens = 0;
    /// Hard cap on generated tokens. The per-call budget is also bounded by the
    /// length of the transcript, so a short phrase never generates 512 tokens.
    int max_output_tokens = 512;
    /// Wall-clock deadline of one clean() call. A transcript that cannot be
    /// cleaned inside it fails (the caller falls back to the rules).
    std::chrono::milliseconds timeout{30000};
};

/// The cleaner behind voicetyper::domain::CleanupRuntime.
///
/// Threading: load/unload/clean are serialised by an internal mutex, so the
/// class is safe to use from one thread and cannot corrupt itself if two are
/// used. Two clean() calls do not run concurrently (one context, one inference).
class LlamaCleanupRuntime final : public domain::CleanupRuntime {
public:
    explicit LlamaCleanupRuntime(LlamaCleanupOptions options = {});
    ~LlamaCleanupRuntime() override;

    LlamaCleanupRuntime(const LlamaCleanupRuntime&) = delete;
    LlamaCleanupRuntime& operator=(const LlamaCleanupRuntime&) = delete;

    /// Opens the runtime library and loads the GGUF at `path`.
    /// A call with the path that is already loaded returns true immediately and
    /// reloads nothing; a call with another path replaces the model.
    [[nodiscard]] bool load(const std::string& path, std::string& error) override;

    [[nodiscard]] bool loaded() const override;

    /// One complete transcript in, one cleaned transcript out. An empty input
    /// is answered with an empty output and no inference at all.
    /// `prompt_appendix` is the user's addition to the prompt (settings
    /// `cleanupPrompt`): it is appended after the built-in instruction, trimmed
    /// and capped, and can never replace that instruction.
    [[nodiscard]] bool clean(std::string_view text, std::string_view prompt_appendix,
        std::string& cleaned, std::string& error) override;

    /// Releases the model and the library. Safe when nothing is loaded.
    void unload() override;

    /// The exact system prompt every clean() call uses. Exposed so the report,
    /// the logs and a contract test can state it instead of paraphrasing it.
    [[nodiscard]] static std::string_view system_prompt() noexcept;

    /// The prompt one clean() call really uses: the same instruction with the
    /// language of the answer stated explicitly, taken from the transcript itself
    /// ("Отвечай ТОЛЬКО на русском языке" / "Answer in English only"). Needed
    /// because a small model drifts into another language on its own - observed
    /// on live dictation 14.10.2026, where Russian speech came back in English.
    /// The guard in src/domain/cleanup_guard.hpp refuses such an answer anyway;
    /// this is the cheaper first line of defence.
    [[nodiscard]] static std::string system_prompt_for(std::string_view transcript);

    /// Path of the model that is currently loaded, or "".
    [[nodiscard]] std::string loaded_path() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voicetyper::llm
