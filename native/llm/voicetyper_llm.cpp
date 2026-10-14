// VoiceTyper facade over the pinned llama.cpp. This is the only translation
// unit of the LLM runtime that includes <llama.h>; everything the product sees
// is the flat C ABI declared in voicetyper_llm.h.
//
// What happens here, and what deliberately does not:
//   * one model, one context, created by voicetyper_llm_open and reused by
//     every voicetyper_llm_generate call (no per-call model reload);
//   * greedy sampling only (deterministic, testable);
//   * the KV cache is cleared before every prompt, so two calls are independent
//     and the answer never depends on the previous prompt;
//   * the prompt is validated against the context window before a single token
//     is decoded, so "too long" is an error instead of a silent truncation;
//   * a wall-clock deadline and a caller-supplied cancel hook are polled
//     between decode steps (this is llama.cpp's abort callback - the only
//     cooperative cancellation point the library offers);
//   * no exceptions cross the ABI, no Qt, no network, no GPU, no logging on
//     stderr unless VOICETYPER_LLM_VERBOSE is set in the environment.

#include "voicetyper_llm.h"

#include <llama.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
// Only for the UTF-8 -> UTF-16 conversion of the model path in looks_like_gguf.
// Guarded because MinGW's own <os_defines.h> already defines NOMINMAX.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef VOICETYPER_LLAMA_PINNED_COMMIT
#error "VOICETYPER_LLAMA_PINNED_COMMIT must be defined by native/llm/CMakeLists.txt (see native/llm/LlamaPins.cmake)"
#endif
#ifndef VOICETYPER_LLAMA_UPSTREAM_VERSION
#error "VOICETYPER_LLAMA_UPSTREAM_VERSION must be defined by native/llm/CMakeLists.txt"
#endif
#ifndef VOICETYPER_LLAMA_GGML_VERSION
#error "VOICETYPER_LLAMA_GGML_VERSION must be defined by native/llm/CMakeLists.txt"
#endif
#ifndef VOICETYPER_LLAMA_REPOSITORY
#error "VOICETYPER_LLAMA_REPOSITORY must be defined by native/llm/CMakeLists.txt"
#endif
#ifndef VOICETYPER_LLAMA_LICENSE
#error "VOICETYPER_LLAMA_LICENSE must be defined by native/llm/CMakeLists.txt"
#endif
#ifndef VOICETYPER_LLAMA_ARCHIVE_SHA256
#error "VOICETYPER_LLAMA_ARCHIVE_SHA256 must be defined by native/llm/CMakeLists.txt"
#endif

namespace {

using SteadyClock = std::chrono::steady_clock;

/// Defaults of voicetyper_llm_options, applied when a field is <= 0 or the
/// options pointer is NULL. The thread bound mirrors whisper_native.hpp's
/// clamp_whisper_threads: the product is a CPU-only desktop application and
/// flooding a small machine with workers costs more than it gains.
constexpr int32_t kDefaultMaxOutputTokens = 256;
constexpr int32_t kDefaultBatchTokens = 512;
constexpr int32_t kMaxThreads = 16;

int32_t default_threads() noexcept
{
    const auto hardware = static_cast<int32_t>(std::thread::hardware_concurrency());
    const int32_t bounded = hardware <= 0 ? 4 : hardware;
    return std::min<int32_t>(4, std::min<int32_t>(bounded, kMaxThreads));
}

/// Process-wide llama.cpp backend. llama_backend_init() must run once before
/// the first model load and llama_backend_free() after the last unload; the
/// counter makes that hold no matter how many contexts the caller opens, and
/// the mutex makes the counting itself safe.
std::mutex g_backend_mutex;
int g_backend_users = 0;
bool g_logger_installed = false;

void silent_log(ggml_log_level, const char*, void*) {}

void backend_acquire()
{
    const std::lock_guard<std::mutex> lock(g_backend_mutex);
    if (g_backend_users == 0) {
        llama_backend_init();
    }
    ++g_backend_users;
    if (!g_logger_installed) {
        // A library must not print to the application's stderr: llama.cpp is
        // extremely chatty about device discovery and graph allocation. The
        // escape hatch exists because those lines are the only diagnostic when
        // a model refuses to load on a new machine.
        const char* verbose = std::getenv("VOICETYPER_LLM_VERBOSE");
        if (verbose == nullptr || *verbose == '\0' || std::strcmp(verbose, "0") == 0) {
            llama_log_set(silent_log, nullptr);
        }
        g_logger_installed = true;
    }
}

void backend_release()
{
    const std::lock_guard<std::mutex> lock(g_backend_mutex);
    if (g_backend_users > 0) {
        --g_backend_users;
    }
    if (g_backend_users == 0) {
        llama_backend_free();
    }
}

char* dup_string(const std::string& text)
{
    char* copy = static_cast<char*>(std::malloc(text.size() + 1));
    if (copy == nullptr) {
        return nullptr;
    }
    std::memcpy(copy, text.c_str(), text.size() + 1);
    return copy;
}

void set_error(char** out_error, const std::string& message)
{
    if (out_error != nullptr) {
        *out_error = dup_string(message);
    }
}

bool verbose_requested()
{
    const char* verbose = std::getenv("VOICETYPER_LLM_VERBOSE");
    return verbose != nullptr && *verbose != '\0' && std::strcmp(verbose, "0") != 0;
}

/// Cooperative abort state shared with llama.cpp's abort callback for the
/// duration of one decode. `reason` records *why* the callback fired, because
/// llama_decode only says "aborted" and the caller has to tell a timeout from a
/// cancellation.
struct AbortState {
    voicetyper_llm_cancel_fn cancel = nullptr;
    void* cancel_user_data = nullptr;
    SteadyClock::time_point deadline{};
    bool has_deadline = false;
    int reason = 0; // 0 = none, 1 = cancel hook, 2 = deadline
};

bool abort_callback(void* user_data)
{
    auto* state = static_cast<AbortState*>(user_data);
    if (state == nullptr) {
        return false;
    }
    if (state->cancel != nullptr && state->cancel(state->cancel_user_data) != 0) {
        state->reason = 1;
        return true;
    }
    if (state->has_deadline && SteadyClock::now() >= state->deadline) {
        state->reason = 2;
        return true;
    }
    return false;
}

/// True when the file starts with the GGUF magic. Checked before llama.cpp is
/// handed the path: a truncated download or an HTML error page would otherwise
/// be reported as a generic load failure after a long read.
bool looks_like_gguf(const std::string& utf8_path)
{
    std::FILE* file = nullptr;
#if defined(_WIN32)
    // std::fopen interprets the name in the ANSI code page; the model store
    // hands us UTF-8. Convert explicitly so a user directory with non-ASCII
    // characters does not turn into "model not found".
    std::wstring wide;
    {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(),
                                               static_cast<int>(utf8_path.size()), nullptr, 0);
        if (needed > 0) {
            wide.resize(static_cast<std::size_t>(needed));
            MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), static_cast<int>(utf8_path.size()),
                                wide.data(), needed);
        }
    }
    if (!wide.empty()) {
        file = _wfopen(wide.c_str(), L"rb");
    }
#else
    file = std::fopen(utf8_path.c_str(), "rb");
#endif
    if (file == nullptr) {
        return false;
    }
    char magic[4] = {};
    const std::size_t read = std::fread(magic, 1, sizeof(magic), file);
    std::fclose(file);
    return read == sizeof(magic) && std::memcmp(magic, "GGUF", 4) == 0;
}

int32_t fail(std::int32_t status, char** out_error, const std::string& message)
{
    set_error(out_error, message);
    return status;
}

/// One llama_decode, mapped onto the flat status codes.
int32_t decode(llama_context* ctx, llama_batch batch, const AbortState& state)
{
    const int32_t result = llama_decode(ctx, batch);
    if (result == 0) {
        return VOICETYPER_LLM_STATUS_OK;
    }
    if (result == 2) {
        return state.reason == 2 ? VOICETYPER_LLM_STATUS_TIMEOUT : VOICETYPER_LLM_STATUS_CANCELLED;
    }
    return VOICETYPER_LLM_STATUS_DECODE_FAILED;
}

} // namespace

/// Opaque context of the C ABI: the loaded model, its one inference context and
/// the options the calls default to.
struct voicetyper_llm_context {
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    const llama_vocab* vocab = nullptr;
    /// Serialises generate() on this context. Greedy decoding mutates the KV
    /// cache and the sampler, so one context is one inference at a time.
    std::mutex mutex;
    int32_t default_max_output_tokens = kDefaultMaxOutputTokens;
    int32_t window_tokens = 0;
};

extern "C" {

int32_t voicetyper_llm_abi_version(void)
{
    return VOICETYPER_LLM_ABI_VERSION;
}

const char* voicetyper_llm_llama_commit(void) { return VOICETYPER_LLAMA_PINNED_COMMIT; }
const char* voicetyper_llm_llama_version(void) { return VOICETYPER_LLAMA_UPSTREAM_VERSION; }
const char* voicetyper_llm_llama_ggml_version(void) { return VOICETYPER_LLAMA_GGML_VERSION; }
const char* voicetyper_llm_llama_repository(void) { return VOICETYPER_LLAMA_REPOSITORY; }
const char* voicetyper_llm_llama_license(void) { return VOICETYPER_LLAMA_LICENSE; }
const char* voicetyper_llm_llama_archive_sha256(void) { return VOICETYPER_LLAMA_ARCHIVE_SHA256; }

const char* voicetyper_llm_status_name(int32_t status)
{
    switch (status) {
    case VOICETYPER_LLM_STATUS_OK: return "ok";
    case VOICETYPER_LLM_STATUS_INVALID_ARGUMENT: return "invalid_argument";
    case VOICETYPER_LLM_STATUS_MODEL_NOT_FOUND: return "model_not_found";
    case VOICETYPER_LLM_STATUS_MODEL_LOAD_FAILED: return "model_load_failed";
    case VOICETYPER_LLM_STATUS_CONTEXT_FAILED: return "context_failed";
    case VOICETYPER_LLM_STATUS_PROMPT_TOO_LONG: return "prompt_too_long";
    case VOICETYPER_LLM_STATUS_DECODE_FAILED: return "decode_failed";
    case VOICETYPER_LLM_STATUS_TIMEOUT: return "timeout";
    case VOICETYPER_LLM_STATUS_CANCELLED: return "cancelled";
    case VOICETYPER_LLM_STATUS_INTERNAL: return "internal";
    default: return "unknown";
    }
}

int32_t voicetyper_llm_open(const char* model_path,
                            const voicetyper_llm_options* options,
                            voicetyper_llm_context** out_context,
                            char** out_error)
{
    if (out_error != nullptr) {
        *out_error = nullptr;
    }
    if (out_context == nullptr) {
        return fail(VOICETYPER_LLM_STATUS_INVALID_ARGUMENT, out_error,
                    "voicetyper_llm_open: out_context is null");
    }
    *out_context = nullptr;
    if (model_path == nullptr || *model_path == '\0') {
        return fail(VOICETYPER_LLM_STATUS_INVALID_ARGUMENT, out_error,
                    "voicetyper_llm_open: model path is empty");
    }

    const std::string path_utf8(model_path);
    try {
        const std::filesystem::path path(std::u8string(
            reinterpret_cast<const char8_t*>(path_utf8.data()), path_utf8.size()));
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            return fail(VOICETYPER_LLM_STATUS_MODEL_NOT_FOUND, out_error,
                        "model file is missing: " + path_utf8);
        }
        if (!looks_like_gguf(path_utf8)) {
            return fail(VOICETYPER_LLM_STATUS_MODEL_LOAD_FAILED, out_error,
                        "model file is unreadable or does not start with the GGUF magic: " + path_utf8);
        }

        voicetyper_llm_options effective{};
        if (options != nullptr) {
            effective = *options;
        }
        if (effective.threads <= 0) {
            effective.threads = default_threads();
        }
        effective.threads = std::min(std::max(effective.threads, 1), kMaxThreads);
        if (effective.max_output_tokens <= 0) {
            effective.max_output_tokens = kDefaultMaxOutputTokens;
        }
        if (effective.batch_tokens <= 0) {
            effective.batch_tokens = kDefaultBatchTokens;
        }

        backend_acquire();

        llama_model_params model_params = llama_model_default_params();
        // CPU only: this runtime is the offline cleanup model of a desktop
        // product, and a GPU backend would add a driver dependency the product
        // does not ship.
        model_params.n_gpu_layers = 0;

        llama_model* model = llama_model_load_from_file(path_utf8.c_str(), model_params);
        if (model == nullptr) {
            backend_release();
            return fail(VOICETYPER_LLM_STATUS_MODEL_LOAD_FAILED, out_error,
                        "llama.cpp could not load the model (unsupported architecture, quantisation or"
                        " a truncated GGUF file): " + path_utf8);
        }

        const int32_t trained_window = llama_model_n_ctx_train(model);
        int32_t window = effective.context_tokens;
        if (window <= 0) {
            window = 4096;
        }
        if (trained_window > 0) {
            window = std::min(window, trained_window);
        }
        window = std::max(window, 64);
        const int32_t batch = std::min(std::max(effective.batch_tokens, 1), window);

        llama_context_params context_params = llama_context_default_params();
        context_params.n_ctx = static_cast<uint32_t>(window);
        context_params.n_batch = static_cast<uint32_t>(batch);
        context_params.n_ubatch = static_cast<uint32_t>(batch);
        context_params.n_threads = effective.threads;
        context_params.n_threads_batch = effective.threads;
        context_params.no_perf = true;

        llama_context* context = llama_init_from_model(model, context_params);
        if (context == nullptr) {
            llama_model_free(model);
            backend_release();
            return fail(VOICETYPER_LLM_STATUS_CONTEXT_FAILED, out_error,
                        "llama.cpp could not create a context of " + std::to_string(window) + " tokens");
        }

        auto* handle = new (std::nothrow) voicetyper_llm_context();
        if (handle == nullptr) {
            llama_free(context);
            llama_model_free(model);
            backend_release();
            return fail(VOICETYPER_LLM_STATUS_INTERNAL, out_error, "out of memory allocating the context");
        }
        handle->model = model;
        handle->context = context;
        handle->vocab = llama_model_get_vocab(model);
        handle->default_max_output_tokens = effective.max_output_tokens;
        handle->window_tokens = static_cast<int32_t>(llama_n_ctx(context));
        *out_context = handle;

        if (verbose_requested()) {
            std::fprintf(stderr,
                         "voicetyper_llm: loaded %s (llama.cpp %s, ggml %s, commit %s),"
                         " window %d tokens, %d threads\n",
                         path_utf8.c_str(), VOICETYPER_LLAMA_UPSTREAM_VERSION, VOICETYPER_LLAMA_GGML_VERSION,
                         VOICETYPER_LLAMA_PINNED_COMMIT, handle->window_tokens, effective.threads);
        }
        return VOICETYPER_LLM_STATUS_OK;
    } catch (const std::exception& error) {
        return fail(VOICETYPER_LLM_STATUS_INTERNAL, out_error,
                    std::string("voicetyper_llm_open failed: ") + error.what());
    } catch (...) {
        return fail(VOICETYPER_LLM_STATUS_INTERNAL, out_error, "voicetyper_llm_open failed");
    }
}

void voicetyper_llm_close(voicetyper_llm_context* context)
{
    if (context == nullptr) {
        return;
    }
    {
        // Hold the mutex so a concurrent generate() on another thread cannot be
        // running while the context it uses disappears.
        const std::lock_guard<std::mutex> lock(context->mutex);
        if (context->context != nullptr) {
            llama_free(context->context);
            context->context = nullptr;
        }
        if (context->model != nullptr) {
            llama_model_free(context->model);
            context->model = nullptr;
        }
        context->vocab = nullptr;
    }
    backend_release();
    delete context;
}

int32_t voicetyper_llm_generate(voicetyper_llm_context* context,
                               const char* prompt,
                               int32_t max_output_tokens,
                               int32_t timeout_ms,
                               voicetyper_llm_cancel_fn cancel,
                               void* cancel_user_data,
                               char** out_text,
                               char** out_error)
{
    if (out_error != nullptr) {
        *out_error = nullptr;
    }
    if (out_text != nullptr) {
        *out_text = nullptr;
    }
    if (context == nullptr || context->context == nullptr || prompt == nullptr || out_text == nullptr) {
        return fail(VOICETYPER_LLM_STATUS_INVALID_ARGUMENT, out_error,
                    "voicetyper_llm_generate: null context, prompt or out_text");
    }

    try {
        const std::lock_guard<std::mutex> lock(context->mutex);
        llama_context* ctx = context->context;
        const llama_vocab* vocab = context->vocab;
        const int32_t window = context->window_tokens;

        // Tokenise first: the whole "does this prompt fit" decision is made
        // before any compute, so an oversized prompt costs nothing.
        const int32_t prompt_bytes = static_cast<int32_t>(std::strlen(prompt));
        std::vector<llama_token> tokens(static_cast<std::size_t>(window) + 1);
        int32_t prompt_tokens = llama_tokenize(vocab, prompt, prompt_bytes, tokens.data(),
                                               static_cast<int32_t>(tokens.size()),
                                               /*add_special=*/false, /*parse_special=*/true);
        if (prompt_tokens == std::numeric_limits<int32_t>::min()) {
            return fail(VOICETYPER_LLM_STATUS_PROMPT_TOO_LONG, out_error,
                        "prompt is far larger than the context window");
        }
        if (prompt_tokens < 0) {
            const int32_t needed = -prompt_tokens;
            if (needed >= window) {
                return fail(VOICETYPER_LLM_STATUS_PROMPT_TOO_LONG, out_error,
                            "prompt needs " + std::to_string(needed) + " tokens but the context window is "
                                + std::to_string(window));
            }
            tokens.resize(static_cast<std::size_t>(needed));
            prompt_tokens = llama_tokenize(vocab, prompt, prompt_bytes, tokens.data(),
                                           static_cast<int32_t>(tokens.size()),
                                           /*add_special=*/false, /*parse_special=*/true);
        }
        if (prompt_tokens <= 0) {
            return fail(VOICETYPER_LLM_STATUS_INVALID_ARGUMENT, out_error,
                        "prompt produced no tokens");
        }

        int32_t budget = max_output_tokens > 0 ? max_output_tokens : context->default_max_output_tokens;
        if (budget <= 0) {
            budget = kDefaultMaxOutputTokens;
        }
        // The window is the hard bound; a caller may ask for more than is left,
        // and the answer is simply shorter rather than an overflow.
        budget = std::min(budget, window - prompt_tokens);
        if (budget <= 0) {
            return fail(VOICETYPER_LLM_STATUS_PROMPT_TOO_LONG, out_error,
                        "prompt leaves no room in the " + std::to_string(window)
                            + "-token context window");
        }

        // Two generate() calls on one context must be independent: without this
        // the second answer would attend to the first prompt.
        llama_memory_clear(llama_get_memory(ctx), true);

        AbortState abort_state;
        abort_state.cancel = cancel;
        abort_state.cancel_user_data = cancel_user_data;
        if (timeout_ms > 0) {
            abort_state.deadline = SteadyClock::now() + std::chrono::milliseconds(timeout_ms);
            abort_state.has_deadline = true;
        }
        llama_set_abort_callback(ctx, abort_callback, &abort_state);

        const int32_t batch_capacity = std::min(context->window_tokens, std::max(1, std::min(
            static_cast<int32_t>(llama_n_batch(ctx)), prompt_tokens)));
        llama_batch batch = llama_batch_init(batch_capacity, 0, 1);
        for (int32_t offset = 0; offset < prompt_tokens;) {
            const int32_t chunk = std::min(batch_capacity, prompt_tokens - offset);
            for (int32_t i = 0; i < chunk; ++i) {
                const int32_t position = offset + i;
                batch.token[i] = tokens[static_cast<std::size_t>(position)];
                batch.pos[i] = position;
                batch.n_seq_id[i] = 1;
                batch.seq_id[i][0] = 0;
                // Only the last prompt token needs logits: that is the
                // distribution the first generated token is sampled from.
                batch.logits[i] = (position == prompt_tokens - 1) ? 1 : 0;
            }
            batch.n_tokens = chunk;
            const int32_t status = decode(ctx, batch, abort_state);
            if (status != VOICETYPER_LLM_STATUS_OK) {
                llama_batch_free(batch);
                return fail(status, out_error,
                            status == VOICETYPER_LLM_STATUS_TIMEOUT
                                ? "inference timed out while reading the prompt"
                                : (status == VOICETYPER_LLM_STATUS_CANCELLED
                                       ? "inference cancelled while reading the prompt"
                                       : "llama_decode failed on the prompt"));
            }
            offset += chunk;
        }
        llama_batch_free(batch);

        llama_sampler* sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

        llama_batch single = llama_batch_init(1, 0, 1);
        std::string answer;
        int32_t produced = 0;
        int32_t status = VOICETYPER_LLM_STATUS_OK;
        while (produced < budget) {
            const llama_token token = llama_sampler_sample(sampler, ctx, -1);
            if (llama_vocab_is_eog(vocab, token)) {
                break;
            }
            char piece[256];
            const int32_t piece_length = llama_token_to_piece(vocab, token, piece,
                                                              static_cast<int32_t>(sizeof(piece)), 0, false);
            if (piece_length > 0) {
                answer.append(piece, static_cast<std::size_t>(piece_length));
            }
            ++produced;
            if (produced >= budget) {
                break;
            }
            single.token[0] = token;
            single.pos[0] = prompt_tokens + produced - 1;
            single.n_seq_id[0] = 1;
            single.seq_id[0][0] = 0;
            single.logits[0] = 1;
            single.n_tokens = 1;
            status = decode(ctx, single, abort_state);
            if (status != VOICETYPER_LLM_STATUS_OK) {
                break;
            }
        }
        llama_batch_free(single);
        llama_sampler_free(sampler);
        // The callback captures a stack object: detach it before returning.
        llama_set_abort_callback(ctx, nullptr, nullptr);

        if (status != VOICETYPER_LLM_STATUS_OK) {
            if (status == VOICETYPER_LLM_STATUS_TIMEOUT) {
                return fail(status, out_error, "inference timed out after "
                                                   + std::to_string(timeout_ms) + " ms");
            }
            if (status == VOICETYPER_LLM_STATUS_CANCELLED) {
                return fail(status, out_error, "inference cancelled");
            }
            return fail(status, out_error, "llama_decode failed while generating");
        }

        char* copy = dup_string(answer);
        if (copy == nullptr) {
            return fail(VOICETYPER_LLM_STATUS_INTERNAL, out_error, "out of memory copying the answer");
        }
        *out_text = copy;
        return VOICETYPER_LLM_STATUS_OK;
    } catch (const std::exception& error) {
        return fail(VOICETYPER_LLM_STATUS_INTERNAL, out_error,
                    std::string("voicetyper_llm_generate failed: ") + error.what());
    } catch (...) {
        return fail(VOICETYPER_LLM_STATUS_INTERNAL, out_error, "voicetyper_llm_generate failed");
    }
}

int32_t voicetyper_llm_apply_chat_template(voicetyper_llm_context* context,
                                           const char* system_prompt,
                                           const char* user_text,
                                           char** out_text,
                                           char** out_error)
{
    if (out_error != nullptr) {
        *out_error = nullptr;
    }
    if (out_text != nullptr) {
        *out_text = nullptr;
    }
    if (context == nullptr || context->model == nullptr || out_text == nullptr) {
        return fail(VOICETYPER_LLM_STATUS_INVALID_ARGUMENT, out_error,
                    "voicetyper_llm_apply_chat_template: null context or out_text");
    }

    try {
        const std::lock_guard<std::mutex> lock(context->mutex);
        const bool has_system = system_prompt != nullptr && *system_prompt != '\0';
        const std::string user = user_text == nullptr ? std::string() : std::string(user_text);

        const char* tmpl = llama_model_chat_template(context->model, nullptr);
        if (tmpl == nullptr) {
            // The GGUF carries no template (a bare base model). A plain block is
            // honest about that; a wrong guess of a real template would inject
            // tokens the model was never trained with.
            std::string plain;
            if (has_system) {
                plain.append("system: ").append(system_prompt).append("\n");
            }
            plain.append("user: ").append(user).append("\nassistant:");
            char* copy = dup_string(plain);
            if (copy == nullptr) {
                return fail(VOICETYPER_LLM_STATUS_INTERNAL, out_error, "out of memory");
            }
            *out_text = copy;
            return VOICETYPER_LLM_STATUS_OK;
        }

        llama_chat_message messages[2];
        std::size_t count = 0;
        if (has_system) {
            messages[count].role = "system";
            messages[count].content = system_prompt;
            ++count;
        }
        messages[count].role = "user";
        messages[count].content = user.c_str();
        ++count;

        std::vector<char> buffer(std::max<std::size_t>(512, (user.size() + (has_system ? std::strlen(system_prompt) : 0)) * 2 + 128));
        int32_t written = llama_chat_apply_template(tmpl, messages, count, /*add_ass=*/true,
                                                    buffer.data(), static_cast<int32_t>(buffer.size()));
        if (written > static_cast<int32_t>(buffer.size())) {
            buffer.assign(static_cast<std::size_t>(written) + 1, '\0');
            written = llama_chat_apply_template(tmpl, messages, count, /*add_ass=*/true,
                                                buffer.data(), static_cast<int32_t>(buffer.size()));
        }
        if (written < 0 || written > static_cast<int32_t>(buffer.size())) {
            return fail(VOICETYPER_LLM_STATUS_CONTEXT_FAILED, out_error,
                        "llama_chat_apply_template failed on the model's template");
        }
        char* copy = dup_string(std::string(buffer.data(), static_cast<std::size_t>(written)));
        if (copy == nullptr) {
            return fail(VOICETYPER_LLM_STATUS_INTERNAL, out_error, "out of memory");
        }
        *out_text = copy;
        return VOICETYPER_LLM_STATUS_OK;
    } catch (const std::exception& error) {
        return fail(VOICETYPER_LLM_STATUS_INTERNAL, out_error,
                    std::string("voicetyper_llm_apply_chat_template failed: ") + error.what());
    } catch (...) {
        return fail(VOICETYPER_LLM_STATUS_INTERNAL, out_error,
                    "voicetyper_llm_apply_chat_template failed");
    }
}

void voicetyper_llm_string_free(char* text)
{
    std::free(text);
}

} // extern "C"
