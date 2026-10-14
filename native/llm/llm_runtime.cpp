// Runtime loader of the VoiceTyper local text-cleanup library.
//
// This is the only place in the product that opens voicetyper_llm.so /
// voicetyper_llm.dll. It binds a closed list of symbols from
// native/llm/voicetyper_llm.h, asserts the ABI revision and the pinned
// llama.cpp commit, and maps the flat C status onto domain::Result/Error. No
// third-party header and no llama.cpp header is included here: the library is
// reached through dlsym/GetProcAddress only, exactly like the parakeet and
// transcribe runtimes.

#include "llm_runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#if defined(_WIN32)
// Guarded because MinGW's own <os_defines.h> already defines NOMINMAX.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#ifndef VOICETYPER_LLAMA_PINNED_COMMIT
#error "VOICETYPER_LLAMA_PINNED_COMMIT must be defined by cmake/LlmRuntime.cmake"
#endif

namespace voicetyper::llm {
namespace {

#if defined(_WIN32)
using ModuleHandle = HMODULE;

std::string platform_error()
{
    return "GetLastError=" + std::to_string(static_cast<unsigned long>(::GetLastError()));
}

ModuleHandle open_module(const std::filesystem::path& path, std::string& error)
{
    ::SetLastError(0);
    HMODULE module = ::LoadLibraryW(path.wstring().c_str());
    if (module == nullptr) {
        error = "LoadLibraryW failed (" + platform_error() + ")";
    }
    return module;
}

void close_module(ModuleHandle module)
{
    if (module != nullptr) {
        ::FreeLibrary(module);
    }
}

void* resolve_symbol(ModuleHandle module, const char* name)
{
    return reinterpret_cast<void*>(::GetProcAddress(module, name));
}

std::filesystem::path executable_path()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}
#else
using ModuleHandle = void*;

std::string platform_error()
{
    const char* message = ::dlerror();
    return message == nullptr ? std::string("dlerror returned nothing") : std::string(message);
}

ModuleHandle open_module(const std::filesystem::path& path, std::string& error)
{
    ::dlerror();
    void* module = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        error = "dlopen failed (" + platform_error() + ")";
    }
    return module;
}

void close_module(ModuleHandle module)
{
    if (module != nullptr) {
        ::dlclose(module);
    }
}

void* resolve_symbol(ModuleHandle module, const char* name)
{
    return ::dlsym(module, name);
}

std::filesystem::path executable_path()
{
    std::string buffer(4096, '\0');
    const ssize_t written = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (written <= 0) {
        return {};
    }
    buffer.resize(static_cast<std::size_t>(written));
    return std::filesystem::path(buffer);
}
#endif

/// The flat C API, resolved once. Every field is non-null in a usable library.
struct Api {
    int32_t (*abi_version)() = nullptr;
    const char* (*llama_commit)() = nullptr;
    const char* (*llama_version)() = nullptr;
    const char* (*llama_ggml_version)() = nullptr;
    const char* (*llama_repository)() = nullptr;
    const char* (*llama_license)() = nullptr;
    const char* (*llama_archive_sha256)() = nullptr;
    const char* (*status_name)(int32_t) = nullptr;
    int32_t (*open)(const char*, const voicetyper_llm_options*, voicetyper_llm_context**, char**) = nullptr;
    void (*close)(voicetyper_llm_context*) = nullptr;
    int32_t (*generate)(voicetyper_llm_context*, const char*, int32_t, int32_t,
                        voicetyper_llm_cancel_fn, void*, char**, char**) = nullptr;
    int32_t (*apply_chat_template)(voicetyper_llm_context*, const char*, const char*, char**, char**) = nullptr;
    void (*string_free)(char*) = nullptr;
};

/// Resolves `kLlmRequiredSymbols` into `api`, in the order the array lists them.
/// Returns the names that could not be resolved.
std::vector<std::string> resolve_api(ModuleHandle module, Api& api)
{
    std::vector<std::string> missing;
    auto bind = [&](std::string_view name, auto& slot) {
        slot = reinterpret_cast<std::decay_t<decltype(slot)>>(resolve_symbol(module, std::string(name).c_str()));
        if (slot == nullptr) {
            missing.emplace_back(name);
        }
    };
    bind(kLlmRequiredSymbols[0], api.abi_version);
    bind(kLlmRequiredSymbols[1], api.llama_commit);
    bind(kLlmRequiredSymbols[2], api.llama_version);
    bind(kLlmRequiredSymbols[3], api.llama_ggml_version);
    bind(kLlmRequiredSymbols[4], api.llama_repository);
    bind(kLlmRequiredSymbols[5], api.llama_license);
    bind(kLlmRequiredSymbols[6], api.llama_archive_sha256);
    bind(kLlmRequiredSymbols[7], api.status_name);
    bind(kLlmRequiredSymbols[8], api.open);
    bind(kLlmRequiredSymbols[9], api.close);
    bind(kLlmRequiredSymbols[10], api.generate);
    bind(kLlmRequiredSymbols[11], api.apply_chat_template);
    bind(kLlmRequiredSymbols[12], api.string_free);
    return missing;
}

std::string join(const std::vector<std::string>& values)
{
    std::string joined;
    for (const auto& value : values) {
        if (!joined.empty()) {
            joined.append(", ");
        }
        joined.append(value);
    }
    return joined;
}

/// Flat status -> domain error code. The mapping is the one documented in
/// native/llm/voicetyper_llm.h; nothing is invented here.
ErrorCode to_error_code(int32_t status)
{
    switch (status) {
    case VOICETYPER_LLM_STATUS_OK: return ErrorCode::ok;
    case VOICETYPER_LLM_STATUS_INVALID_ARGUMENT: return ErrorCode::invalid_argument;
    case VOICETYPER_LLM_STATUS_MODEL_NOT_FOUND: return ErrorCode::not_found;
    case VOICETYPER_LLM_STATUS_MODEL_LOAD_FAILED: return ErrorCode::corrupt_data;
    case VOICETYPER_LLM_STATUS_CONTEXT_FAILED: return ErrorCode::resource_exhausted;
    case VOICETYPER_LLM_STATUS_PROMPT_TOO_LONG: return ErrorCode::out_of_range;
    case VOICETYPER_LLM_STATUS_DECODE_FAILED: return ErrorCode::internal;
    case VOICETYPER_LLM_STATUS_TIMEOUT: return ErrorCode::timeout;
    case VOICETYPER_LLM_STATUS_CANCELLED: return ErrorCode::cancelled;
    case VOICETYPER_LLM_STATUS_INTERNAL: return ErrorCode::internal;
    default: return ErrorCode::internal;
    }
}

/// Adapter from the flat cancel hook to the domain token. The library calls it
/// between decode steps, i.e. on the calling thread.
int32_t cancel_from_token(void* user_data)
{
    const auto* token = static_cast<const CancellationToken*>(user_data);
    return token != nullptr && token->is_cancellation_requested() ? 1 : 0;
}

} // namespace

std::string_view expected_llama_commit() noexcept
{
    return VOICETYPER_LLAMA_PINNED_COMMIT;
}

std::string_view llm_library_file_name() noexcept
{
#if defined(_WIN32)
    return "voicetyper_llm.dll";
#else
    return "voicetyper_llm.so";
#endif
}

std::filesystem::path llm_library_beside_executable()
{
    const std::filesystem::path executable = executable_path();
    if (executable.empty()) {
        return {};
    }
    return executable.parent_path() / std::filesystem::path(llm_library_file_name());
}

std::filesystem::path default_llm_library_path()
{
    const char* override_path = std::getenv("VOICETYPER_LLM_LIBRARY");
    if (override_path != nullptr && *override_path != '\0') {
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(override_path)));
    }
    return llm_library_beside_executable();
}

LlmRuntimeProbe probe_llm_runtime(const std::filesystem::path& library_path)
{
    LlmRuntimeProbe probe;
    probe.library_path = library_path;
    if (library_path.empty()) {
        probe.detail = "no runtime library path was given";
        return probe;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(library_path, error)) {
        probe.detail = "runtime library is missing: " + library_path.string();
        return probe;
    }

    std::string open_error;
    ModuleHandle module = open_module(library_path, open_error);
    if (module == nullptr) {
        probe.detail = open_error + " for " + library_path.string();
        return probe;
    }
    probe.library_loaded = true;

    Api api;
    probe.missing_symbols = resolve_api(module, api);
    if (!probe.missing_symbols.empty()) {
        probe.detail = "runtime library does not export " + join(probe.missing_symbols)
            + " (not a VoiceTyper LLM runtime?)";
        close_module(module);
        return probe;
    }

    probe.abi_version = api.abi_version();
    probe.pinned_commit = api.llama_commit() == nullptr ? std::string() : std::string(api.llama_commit());
    probe.llama_version = api.llama_version() == nullptr ? std::string() : std::string(api.llama_version());
    close_module(module);

    if (probe.abi_version != kLlmRuntimeAbiVersion) {
        probe.detail = "ABI mismatch: the library reports " + std::to_string(probe.abi_version)
            + ", this build expects " + std::to_string(kLlmRuntimeAbiVersion);
        return probe;
    }
    if (probe.pinned_commit != expected_llama_commit()) {
        probe.detail = "llama.cpp pin mismatch: the library contains " + probe.pinned_commit
            + ", this build expects " + std::string(expected_llama_commit());
        return probe;
    }
    probe.usable = true;
    return probe;
}

struct LlmRuntime::Impl {
    ModuleHandle module = nullptr;
    Api api;
    voicetyper_llm_context* context = nullptr;
    LlmRuntimeProbe probe;
    std::string last_error;

    ~Impl()
    {
        if (context != nullptr && api.close != nullptr) {
            api.close(context);
            context = nullptr;
        }
        close_module(module);
        module = nullptr;
    }
};

LlmRuntime::LlmRuntime() : impl_(std::make_unique<Impl>()) {}

LlmRuntime::~LlmRuntime() = default;

Result<std::unique_ptr<LlmRuntime>> LlmRuntime::open(const std::filesystem::path& library_path,
                                                     const std::filesystem::path& model_path,
                                                     const LlmOptions& options,
                                                     const CancellationToken& cancellation)
{
    if (cancellation.is_cancellation_requested()) {
        return Result<std::unique_ptr<LlmRuntime>>::failure(ErrorCode::cancelled,
                                                            "opening the cleanup runtime was cancelled");
    }
    // An empty path means "the library the product ships": the environment
    // override first, then the file beside the executable. Passing the path
    // explicitly stays possible for tests and diagnostics.
    const std::filesystem::path library = library_path.empty() ? default_llm_library_path()
                                                               : library_path;
    LlmRuntimeProbe probe = probe_llm_runtime(library);
    if (!probe.usable) {
        return Result<std::unique_ptr<LlmRuntime>>::failure(ErrorCode::unavailable, probe.detail);
    }

    std::string open_error;
    ModuleHandle module = open_module(library, open_error);
    if (module == nullptr) {
        return Result<std::unique_ptr<LlmRuntime>>::failure(
            ErrorCode::unavailable, open_error + " for " + library.string());
    }

    std::unique_ptr<LlmRuntime> runtime(new LlmRuntime());
    runtime->impl_->module = module;
    runtime->impl_->api = Api{};
    const auto missing = resolve_api(module, runtime->impl_->api);
    if (!missing.empty()) {
        // The probe resolved the same list a moment ago; this is a defect in the
        // loader, not a user condition.
        return Result<std::unique_ptr<LlmRuntime>>::failure(
            ErrorCode::internal, "required symbols disappeared: " + join(missing));
    }
    runtime->impl_->probe = std::move(probe);

    voicetyper_llm_options native_options{};
    native_options.threads = options.threads;
    native_options.context_tokens = options.context_tokens;
    native_options.max_output_tokens = options.max_output_tokens;
    native_options.batch_tokens = options.batch_tokens;

    const std::string model_utf8 = model_path.u8string().empty()
        ? std::string()
        : std::string(reinterpret_cast<const char*>(model_path.u8string().c_str()));
    char* error_message = nullptr;
    voicetyper_llm_context* context = nullptr;
    const int32_t status = runtime->impl_->api.open(model_utf8.c_str(), &native_options, &context,
                                                    &error_message);
    if (status != VOICETYPER_LLM_STATUS_OK || context == nullptr) {
        const std::string detail = error_message != nullptr ? std::string(error_message)
                                                            : std::string("voicetyper_llm_open failed");
        if (error_message != nullptr) {
            runtime->impl_->api.string_free(error_message);
        }
        runtime->impl_->last_error = detail;
        return Result<std::unique_ptr<LlmRuntime>>::failure(to_error_code(status), detail);
    }
    runtime->impl_->context = context;
    return Result<std::unique_ptr<LlmRuntime>>(std::move(runtime));
}

bool LlmRuntime::is_open() const noexcept
{
    return impl_ != nullptr && impl_->context != nullptr;
}

const LlmRuntimeProbe& LlmRuntime::probe() const noexcept
{
    return impl_->probe;
}

std::string LlmRuntime::last_error() const
{
    return impl_ == nullptr ? std::string() : impl_->last_error;
}

Result<std::string> LlmRuntime::generate(std::string_view prompt,
                                         int max_output_tokens,
                                         std::chrono::milliseconds timeout,
                                         const CancellationToken& cancellation)
{
    if (impl_ == nullptr || impl_->context == nullptr) {
        return Result<std::string>::failure(ErrorCode::not_ready, "the cleanup runtime is not open");
    }
    if (prompt.empty()) {
        return Result<std::string>::failure(ErrorCode::invalid_argument,
                                            "the prompt must not be empty");
    }
    if (cancellation.is_cancellation_requested()) {
        return Result<std::string>::failure(ErrorCode::cancelled, "inference was cancelled");
    }

    const std::string prompt_text(prompt);
    int32_t timeout_ms = 0;
    if (timeout.count() > 0) {
        timeout_ms = static_cast<int32_t>(std::min<std::int64_t>(
            timeout.count(), static_cast<std::int64_t>(std::numeric_limits<int32_t>::max())));
    }

    char* text = nullptr;
    char* error_message = nullptr;
    const int32_t status = impl_->api.generate(impl_->context, prompt_text.c_str(), max_output_tokens,
                                               timeout_ms, cancel_from_token,
                                               const_cast<CancellationToken*>(&cancellation), &text,
                                               &error_message);
    if (status != VOICETYPER_LLM_STATUS_OK || text == nullptr) {
        std::string detail = error_message != nullptr ? std::string(error_message) : std::string();
        if (error_message != nullptr) {
            impl_->api.string_free(error_message);
        }
        if (detail.empty()) {
            detail = impl_->api.status_name != nullptr ? std::string(impl_->api.status_name(status))
                                                       : std::string("cleanup inference failed");
        }
        impl_->last_error = detail;
        return Result<std::string>::failure(to_error_code(status), detail);
    }

    std::string answer(text);
    impl_->api.string_free(text);
    impl_->last_error.clear();
    return Result<std::string>(std::move(answer));
}

Result<std::string> LlmRuntime::apply_chat_template(std::string_view system_prompt,
                                                    std::string_view user_text)
{
    if (impl_ == nullptr || impl_->context == nullptr) {
        return Result<std::string>::failure(ErrorCode::not_ready, "the cleanup runtime is not open");
    }
    const std::string system_text(system_prompt);
    const std::string user(user_text);
    char* text = nullptr;
    char* error_message = nullptr;
    const int32_t status = impl_->api.apply_chat_template(
        impl_->context, system_text.empty() ? nullptr : system_text.c_str(), user.c_str(), &text,
        &error_message);
    if (status != VOICETYPER_LLM_STATUS_OK || text == nullptr) {
        std::string detail = error_message != nullptr ? std::string(error_message) : std::string();
        if (error_message != nullptr) {
            impl_->api.string_free(error_message);
        }
        if (detail.empty()) {
            detail = "the model's chat template could not be applied";
        }
        return Result<std::string>::failure(to_error_code(status), detail);
    }
    std::string formatted(text);
    impl_->api.string_free(text);
    return Result<std::string>(std::move(formatted));
}

} // namespace voicetyper::llm
