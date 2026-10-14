/* VoiceTyper local text-cleanup runtime: the public ABI of voicetyper_llm.
 *
 * What this library is: llama.cpp (the commit pinned in
 * native/llm/LlamaPins.cmake) plus the VoiceTyper facade in
 * native/llm/voicetyper_llm.cpp, built into ONE shared object
 * (voicetyper_llm.so / voicetyper_llm.dll) with llama.cpp's ggml linked
 * statically inside it. Nothing of llama.cpp crosses this header: it is a flat
 * C ABI, so the library can be resolved with dlopen/dlsym or LoadLibraryW,
 * exactly like the parakeet.cpp and transcribe.cpp runtimes.
 *
 * Why a separate shared object and not a static library in the application:
 * whisper.cpp is compiled into the application and vendors ggml 0.25.1, while
 * llama.cpp vendors ggml 0.26.0. Both define the same ggml_* symbols with
 * different layouts, so one set of symbols must never serve both engines. Each
 * library keeps its own copy, and only this flat ABI crosses the boundary.
 *
 * What is deliberately NOT here: no network, no Qt, no GPUs, no streaming, no
 * chat state. One model, one context, one completion per call, CPU only.
 *
 * Threading: a context serialises its own calls with an internal mutex, so
 * calling generate on one context from several threads is safe but not
 * concurrent (one inference at a time). Two contexts are two models: they do
 * not share anything and may be used from different threads. Opening and
 * closing a context is not thread safe against itself.
 *
 * Ownership: every `char**` out-parameter is either NULL or a NUL-terminated
 * UTF-8 string allocated by the library and released with
 * voicetyper_llm_string_free(). A context from voicetyper_llm_open() is
 * released with voicetyper_llm_close(). Nothing else is owned by the caller.
 *
 * Errors: every fallible entry point returns voicetyper_llm_status (0 = ok) and
 * optionally a human-readable message. No exception ever crosses this ABI; a
 * defect is reported as VOICETYPER_LLM_STATUS_INTERNAL.
 */

#ifndef VOICETYPER_LLM_H
#define VOICETYPER_LLM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever the meaning of an existing entry point changes. The binding
 * refuses a library that reports a different number. */
#define VOICETYPER_LLM_ABI_VERSION 1

#if defined(_WIN32)
#if defined(VOICETYPER_LLM_BUILD)
#define VOICETYPER_LLM_API __declspec(dllexport)
#else
#define VOICETYPER_LLM_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define VOICETYPER_LLM_API __attribute__((visibility("default")))
#else
#define VOICETYPER_LLM_API
#endif

/* Status of a call. 0 is success; anything else is a failure with a matching
 * VoiceTyper ErrorCode on the caller's side:
 *   cancelled         -> domain::ErrorCode::cancelled
 *   invalid_argument  -> invalid_argument
 *   model_not_found   -> not_found / model_not_ready
 *   model_load_failed -> corrupt_data
 *   context_failed    -> resource_exhausted
 *   prompt_too_long   -> out_of_range
 *   decode_failed     -> internal
 *   timeout           -> timeout
 *   internal          -> internal
 */
typedef enum voicetyper_llm_status {
    VOICETYPER_LLM_STATUS_OK = 0,
    VOICETYPER_LLM_STATUS_INVALID_ARGUMENT = 1,
    VOICETYPER_LLM_STATUS_MODEL_NOT_FOUND = 2,
    VOICETYPER_LLM_STATUS_MODEL_LOAD_FAILED = 3,
    VOICETYPER_LLM_STATUS_CONTEXT_FAILED = 4,
    VOICETYPER_LLM_STATUS_PROMPT_TOO_LONG = 5,
    VOICETYPER_LLM_STATUS_DECODE_FAILED = 6,
    VOICETYPER_LLM_STATUS_TIMEOUT = 7,
    VOICETYPER_LLM_STATUS_CANCELLED = 8,
    VOICETYPER_LLM_STATUS_INTERNAL = 9
} voicetyper_llm_status;

/* Context creation options. Every field accepts 0 or a negative value for "use
 * the default"; the defaults are documented next to each field, and the
 * effective values are never allowed to exceed the model's own limits. */
typedef struct voicetyper_llm_options {
    /* Worker threads. <=0: min(4, hardware_concurrency). Clamped to 1..16. */
    int32_t threads;
    /* Prompt+answer window (KV cache size). <=0: min(4096, model n_ctx_train). */
    int32_t context_tokens;
    /* Default cap on generated tokens when generate() passes <=0. <=0: 256. */
    int32_t max_output_tokens;
    /* Logical batch size for prompt ingestion. <=0: 512, clamped to the window. */
    int32_t batch_tokens;
} voicetyper_llm_options;

typedef struct voicetyper_llm_context voicetyper_llm_context;

/* Cooperative cancellation hook, polled between decode steps. Returning
 * non-zero abandons the inference with VOICETYPER_LLM_STATUS_CANCELLED. It is
 * called from the thread that runs generate(), never from a library thread. */
typedef int32_t (*voicetyper_llm_cancel_fn)(void* user_data);

/* --- identity and pins ---------------------------------------------------- */

/* ABI revision of this library; must equal VOICETYPER_LLM_ABI_VERSION. */
VOICETYPER_LLM_API int32_t voicetyper_llm_abi_version(void);

/* Compile-time pin of the llama.cpp this build contains. Never NULL, never
 * empty. The loader compares the commit against the pin it was built with, so a
 * stale library next to the executable is an explicit error. */
VOICETYPER_LLM_API const char* voicetyper_llm_llama_commit(void);
/* Upstream version string of the pinned commit ("0.6.0"). */
VOICETYPER_LLM_API const char* voicetyper_llm_llama_version(void);
/* Vendored ggml version of the pinned commit ("0.26.0"). */
VOICETYPER_LLM_API const char* voicetyper_llm_llama_ggml_version(void);
VOICETYPER_LLM_API const char* voicetyper_llm_llama_repository(void);
/* SPDX-ish license id of llama.cpp and of the vendored ggml ("MIT"). */
VOICETYPER_LLM_API const char* voicetyper_llm_llama_license(void);
/* SHA-256 of the source archive this library was built from. */
VOICETYPER_LLM_API const char* voicetyper_llm_llama_archive_sha256(void);
/* Human-readable status name, for diagnostics and logs. Never NULL. */
VOICETYPER_LLM_API const char* voicetyper_llm_status_name(int32_t status);

/* --- lifecycle ------------------------------------------------------------ */

/* Loads a GGUF model from `model_path` and creates exactly one context.
 *
 * The file must exist, be readable, be a regular file and start with the GGUF
 * magic; anything else fails with MODEL_NOT_FOUND or MODEL_LOAD_FAILED before
 * llama.cpp is asked to allocate anything.
 *
 * `options` may be NULL (all defaults). On success `*out_context` is a new
 * context and `*out_error` is NULL. On failure `*out_context` is NULL and
 * `*out_error` carries the cause when `out_error` is not NULL.
 *
 * Failure: INVALID_ARGUMENT, MODEL_NOT_FOUND, MODEL_LOAD_FAILED,
 * CONTEXT_FAILED, INTERNAL.
 */
VOICETYPER_LLM_API int32_t voicetyper_llm_open(const char* model_path,
                                              const voicetyper_llm_options* options,
                                              voicetyper_llm_context** out_context,
                                              char** out_error);

/* Releases the context, its KV cache and the model. NULL is accepted and does
 * nothing. Idempotent; the process-wide llama.cpp backend stays initialised
 * until the last context is closed. */
VOICETYPER_LLM_API void voicetyper_llm_close(voicetyper_llm_context* context);

/* --- inference ------------------------------------------------------------ */

/* One greedy completion of `prompt`.
 *
 * Deterministic by construction: the sampler chain is plain greedy (argmax), so
 * the same prompt on the same model produces the same answer. That is what
 * makes the "digits are untouchable" guard in the domain layer testable.
 *
 * The prompt is *not* wrapped in a chat template here - the caller decides what
 * the model sees (use voicetyper_llm_apply_chat_template for instruct models).
 * Special tokens written in the prompt (e.g. <|im_start|>) are parsed.
 *
 * Length: the prompt must fit the context window together with at least one
 * output token, otherwise the call fails with PROMPT_TOO_LONG. The number of
 * generated tokens is min(max_output_tokens (or the context default when <=0),
 * window left after the prompt), and the call returns as soon as the model
 * emits its end-of-generation token.
 *
 * Time: `timeout_ms` > 0 is a wall-clock deadline for the whole call; <=0 means
 * no deadline. `cancel` is polled between decode steps (it may be NULL).
 * Both surface as TIMEOUT or CANCELLED - never as a partial answer that the
 * caller cannot tell apart from a complete one.
 *
 * On success `*out_text` is the generated UTF-8 text (possibly empty when the
 * model stops immediately) and `*out_error` is NULL. On failure `*out_text` is
 * NULL and `*out_error` carries the cause.
 *
 * Failure: INVALID_ARGUMENT, PROMPT_TOO_LONG, DECODE_FAILED, TIMEOUT,
 * CANCELLED, INTERNAL.
 */
VOICETYPER_LLM_API int32_t voicetyper_llm_generate(voicetyper_llm_context* context,
                                                   const char* prompt,
                                                   int32_t max_output_tokens,
                                                   int32_t timeout_ms,
                                                   voicetyper_llm_cancel_fn cancel,
                                                   void* cancel_user_data,
                                                   char** out_text,
                                                   char** out_error);

/* Formats `system_prompt` (may be NULL or empty) and `user_text` with the chat
 * template the loaded model carries in its GGUF metadata - the same template
 * llama.cpp's own tools use, so instruct models get their real special tokens.
 * A model without a template falls back to a plain "role: content" block and
 * the call still succeeds.
 *
 * This only formats text: it runs no inference, so it is cheap and safe to call
 * before a generate(). Failure: INVALID_ARGUMENT (no context/out params),
 * CONTEXT_FAILED (template application failed), INTERNAL.
 */
VOICETYPER_LLM_API int32_t voicetyper_llm_apply_chat_template(voicetyper_llm_context* context,
                                                              const char* system_prompt,
                                                              const char* user_text,
                                                              char** out_text,
                                                              char** out_error);

/* Frees a string returned by this library. NULL is accepted. */
VOICETYPER_LLM_API void voicetyper_llm_string_free(char* text);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VOICETYPER_LLM_H */
