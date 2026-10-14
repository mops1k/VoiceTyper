#pragma once

// VoiceTyper local text-cleanup runtime loader.
//
// The runtime is a separate shared object (voicetyper_llm.so / voicetyper_llm.dll)
// that contains the pinned llama.cpp with its ggml linked statically inside.
// This class is the only place in the product that resolves it, and it resolves
// it exactly the way the parakeet and transcribe runtimes are resolved:
// dlopen/RTLD_LOCAL on Linux, LoadLibraryW on Windows, a closed list of
// symbols, and an explicit error when the library is absent, foreign or of a
// different ABI. There is no link-time dependency and no silent substitution.
//
// Why a runtime loader and not a link-time dependency: the cleanup model is
// optional and 0.4-2 GB. A machine without it must start, dictate and behave
// exactly as before, and the library's absence has to be an answer ("off"),
// not a startup failure.
//
// Contract:
//   * one library, one model, one context (llm_runtime does not pool);
//   * the caller owns the timeout and cancellation: generate() takes a deadline
//     and a CancellationToken, both polled inside the native decode loop;
//   * no exception crosses this boundary; every failure is a domain::Result;
//   * the library is kept loaded for the lifetime of the object, because
//     unloading llama.cpp after a model has been freed is not worth the risk of
//     a second load at the same base address.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "voicetyper_llm.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::llm {

using domain::CancellationToken;
using domain::Error;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// ABI revision of native/llm/voicetyper_llm.h this build was compiled against.
inline constexpr int kLlmRuntimeAbiVersion = VOICETYPER_LLM_ABI_VERSION;

/// The complete, closed list of symbols this product may bind. Resolving
/// anything else is a bug; a missing one is an explicit error.
inline constexpr std::array<std::string_view, 13> kLlmRequiredSymbols = {
    "voicetyper_llm_abi_version",
    "voicetyper_llm_llama_commit",
    "voicetyper_llm_llama_version",
    "voicetyper_llm_llama_ggml_version",
    "voicetyper_llm_llama_repository",
    "voicetyper_llm_llama_license",
    "voicetyper_llm_llama_archive_sha256",
    "voicetyper_llm_status_name",
    "voicetyper_llm_open",
    "voicetyper_llm_close",
    "voicetyper_llm_generate",
    "voicetyper_llm_apply_chat_template",
    "voicetyper_llm_string_free",
};

/// Options of one context. Zero means "the library's default".
struct LlmOptions {
    /// Worker threads; 0 selects min(4, hardware concurrency).
    int threads = 0;
    /// Prompt+answer window; 0 selects min(4096, the model's trained window).
    int context_tokens = 0;
    /// Default cap on generated tokens; 0 selects 256.
    int max_output_tokens = 256;
    /// Prompt ingestion batch; 0 selects 512.
    int batch_tokens = 0;
};

/// What inspecting the library found. `detail` carries the exact cause (path,
/// OS error, observed ABI, missing symbols) so a failure is never reported as a
/// bare "unavailable".
struct LlmRuntimeProbe {
    /// True when the library was opened and every required symbol resolved.
    bool library_loaded = false;
    /// Value returned by voicetyper_llm_abi_version(); 0 when it was not called.
    int abi_version = 0;
    /// True only for a usable library with the required ABI and pinned commit.
    bool usable = false;
    /// Commit the library reports (empty when the library does not answer).
    std::string pinned_commit;
    /// Version the library reports for the llama.cpp it contains.
    std::string llama_version;
    /// Human-readable cause of `usable == false`.
    std::string detail;
    std::vector<std::string> missing_symbols;
    std::filesystem::path library_path;
};

/// The commit native/llm/LlamaPins.cmake pins, compiled into the loader. A
/// library that reports another commit is a stale or foreign build and is
/// refused, not used.
[[nodiscard]] std::string_view expected_llama_commit() noexcept;

/// The file name the runtime is shipped under on this platform
/// ("voicetyper_llm.dll" on Windows, "voicetyper_llm.so" elsewhere).
[[nodiscard]] std::string_view llm_library_file_name() noexcept;

/// `<directory of the running executable>/<llm_library_file_name()>`. Empty when
/// the executable's own path cannot be determined.
[[nodiscard]] std::filesystem::path llm_library_beside_executable();

/// Cheap probe used by diagnostics and by the settings/about page: opens the
/// library, resolves the closed symbol list, asserts the ABI version and the
/// pinned commit, then unloads it again. Never throws, never loads a model.
/// A missing library is not an exception but an answer: `usable == false` with
/// the cause in `detail`.
[[nodiscard]] LlmRuntimeProbe probe_llm_runtime(const std::filesystem::path& library_path);

/// Library path resolution order used by the composition root:
///   1. the VOICETYPER_LLM_LIBRARY environment variable (tests, diagnostics),
///   2. the library beside the executable.
[[nodiscard]] std::filesystem::path default_llm_library_path();

/// An opened runtime with one loaded model.
///
/// Threading: the native context serialises its own calls, so one instance is
/// safe to use from several threads, though only one inference runs at a time.
/// Cancellation and timeouts are checked inside the native decode loop, so a
/// cancelled or timed-out call stops within one token rather than after the
/// whole answer.
class LlmRuntime {
public:
    ~LlmRuntime();

    LlmRuntime(const LlmRuntime&) = delete;
    LlmRuntime& operator=(const LlmRuntime&) = delete;
    LlmRuntime(LlmRuntime&&) = delete;
    LlmRuntime& operator=(LlmRuntime&&) = delete;

    /// Opens the library and loads `model_path` once. The file is not read
    /// again by later calls.
    ///
    /// Failure: unavailable (library missing, unloadable, wrong ABI or foreign
    /// pin), not_found (model file missing), corrupt_data (not a GGUF file or
    /// llama.cpp refused it), resource_exhausted (no context), cancelled.
    [[nodiscard]] static Result<std::unique_ptr<LlmRuntime>> open(
        const std::filesystem::path& library_path,
        const std::filesystem::path& model_path,
        const LlmOptions& options,
        const CancellationToken& cancellation);

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] const LlmRuntimeProbe& probe() const noexcept;

    /// One greedy completion of `prompt`. `max_output_tokens` <= 0 means the
    /// context default; `timeout` <= 0 means no deadline. The prompt is passed
    /// to the model as-is (see apply_chat_template for instruct models).
    ///
    /// Failure: not_ready (not open), invalid_argument (empty prompt),
    /// out_of_range (prompt does not fit the window), timeout, cancelled,
    /// internal (decode failure).
    [[nodiscard]] Result<std::string> generate(
        std::string_view prompt,
        int max_output_tokens,
        std::chrono::milliseconds timeout,
        const CancellationToken& cancellation);

    /// Formats system+user with the model's own chat template. Cheap, runs no
    /// inference.
    [[nodiscard]] Result<std::string> apply_chat_template(
        std::string_view system_prompt,
        std::string_view user_text);

    /// Human-readable last native error, or "" when there is none.
    [[nodiscard]] std::string last_error() const;

private:
    LlmRuntime();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voicetyper::llm
