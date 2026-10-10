#include "asr/transcribe_engine.hpp"

// The vendored public header of the pin
// (native/transcribe/transcribe.h, v0.3.1 - versioned next to the DLLs it describes). It is the only
// place in the product that includes it, exactly like whisper.h in
// whisper_native.cpp: the seam stays portable, the ABI stays in one file.

#include "transcribe.h"

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

#if defined(__linux__)
#    include <dlfcn.h>
#    include <unistd.h>
#endif

namespace voicetyper::asr {
namespace {

#if defined(_WIN32) || defined(__linux__)

#if defined(_WIN32)
using ModuleHandle = HMODULE;
using LibraryKey = std::wstring;
#else
/// The dynamic loader's handle type and the cache key: the same runtime code
/// serves both platforms, only the loader calls differ.
using ModuleHandle = void*;
using LibraryKey = std::string;

/// The shared object the Linux build loads: the same pinned transcribe.cpp
/// (v0.3.1) as the Windows DLL, built for this platform.
inline constexpr std::string_view kTranscribeSharedLibraryName = "libtranscribe.so";
#endif

std::string narrow(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#else
    return path.string();
#endif
}

std::string trimmed(std::string text)
{
    const auto is_space = [](unsigned char character) {
        return character == ' ' || character == '\t' || character == '\n' || character == '\r';
    };
    while (!text.empty() && is_space(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_space(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

/// Local status naming: transcribe_status_string() is deliberately not bound, so
/// the diagnostic text does not depend on a symbol the product does not use.
const char* status_name(transcribe_status status)
{
    switch (status) {
    case TRANSCRIBE_OK: return "ok";
    case TRANSCRIBE_ERR_INVALID_ARG: return "invalid_argument";
    case TRANSCRIBE_ERR_NOT_IMPLEMENTED: return "not_implemented";
    case TRANSCRIBE_ERR_FILE_NOT_FOUND: return "file_not_found";
    case TRANSCRIBE_ERR_GGUF: return "gguf";
    case TRANSCRIBE_ERR_UNSUPPORTED_ARCH: return "unsupported_arch";
    case TRANSCRIBE_ERR_UNSUPPORTED_VARIANT: return "unsupported_variant";
    case TRANSCRIBE_ERR_OOM: return "oom";
    case TRANSCRIBE_ERR_BACKEND: return "backend";
    case TRANSCRIBE_ERR_SAMPLE_RATE: return "sample_rate";
    case TRANSCRIBE_ERR_UNSUPPORTED_LANGUAGE: return "unsupported_language";
    case TRANSCRIBE_ERR_UNSUPPORTED_TASK: return "unsupported_task";
    case TRANSCRIBE_ERR_UNSUPPORTED_TIMESTAMPS: return "unsupported_timestamps";
    case TRANSCRIBE_ERR_ABORTED: return "aborted";
    case TRANSCRIBE_ERR_BAD_STRUCT_SIZE: return "bad_struct_size";
    case TRANSCRIBE_ERR_UNSUPPORTED_PNC: return "unsupported_pnc";
    case TRANSCRIBE_ERR_UNSUPPORTED_ITN: return "unsupported_itn";
    case TRANSCRIBE_ERR_INPUT_TOO_LONG: return "input_too_long";
    case TRANSCRIBE_ERR_OUTPUT_TRUNCATED: return "output_truncated";
    case TRANSCRIBE_ERR_OUTPUT_REPETITION: return "output_repetition";
    }
    return "unknown";
}

domain::ErrorCode map_status(transcribe_status status)
{
    switch (status) {
    case TRANSCRIBE_OK: return domain::ErrorCode::ok;
    case TRANSCRIBE_ERR_INVALID_ARG: return domain::ErrorCode::invalid_argument;
    case TRANSCRIBE_ERR_FILE_NOT_FOUND: return domain::ErrorCode::not_found;
    case TRANSCRIBE_ERR_GGUF: return domain::ErrorCode::corrupt_data;
    case TRANSCRIBE_ERR_OOM: return domain::ErrorCode::resource_exhausted;
    case TRANSCRIBE_ERR_ABORTED: return domain::ErrorCode::cancelled;
    case TRANSCRIBE_ERR_BAD_STRUCT_SIZE: return domain::ErrorCode::internal;
    case TRANSCRIBE_ERR_INPUT_TOO_LONG: return domain::ErrorCode::out_of_range;
    // A truncated transcript is a real failure: the caller must not treat a
    // partial decode as a complete dictation (the library's own rule).
    case TRANSCRIBE_ERR_OUTPUT_TRUNCATED:
    case TRANSCRIBE_ERR_OUTPUT_REPETITION: return domain::ErrorCode::resource_exhausted;
    case TRANSCRIBE_ERR_UNSUPPORTED_ARCH:
    case TRANSCRIBE_ERR_UNSUPPORTED_VARIANT:
    case TRANSCRIBE_ERR_UNSUPPORTED_LANGUAGE:
    case TRANSCRIBE_ERR_UNSUPPORTED_TASK:
    case TRANSCRIBE_ERR_UNSUPPORTED_TIMESTAMPS:
    case TRANSCRIBE_ERR_UNSUPPORTED_PNC:
    case TRANSCRIBE_ERR_UNSUPPORTED_ITN:
    case TRANSCRIBE_ERR_NOT_IMPLEMENTED: return domain::ErrorCode::unsupported;
    case TRANSCRIBE_ERR_BACKEND:
    case TRANSCRIBE_ERR_SAMPLE_RATE: return domain::ErrorCode::unavailable;
    }
    return domain::ErrorCode::unavailable;
}

/// The token is polled by the library between decode steps, on the run thread,
/// which is exactly the cancellation contract of the other two engines.
bool on_abort(void* user_data)
{
    const auto* cancellation = static_cast<const domain::CancellationToken*>(user_data);
    return cancellation != nullptr && cancellation->is_cancellation_requested();
}

struct Symbols {
    decltype(&transcribe_version) version = nullptr;
    decltype(&transcribe_abi_struct_size) abi_struct_size = nullptr;
    decltype(&transcribe_model_load_params_init) model_load_params_init = nullptr;
    decltype(&transcribe_session_params_init) session_params_init = nullptr;
    decltype(&transcribe_run_params_init) run_params_init = nullptr;
    decltype(&transcribe_capabilities_init) capabilities_init = nullptr;
    decltype(&transcribe_open) open = nullptr;
    decltype(&transcribe_session_free) session_free = nullptr;
    decltype(&transcribe_get_model) get_model = nullptr;
    decltype(&transcribe_model_get_capabilities) capabilities = nullptr;
    decltype(&transcribe_set_abort_callback) set_abort_callback = nullptr;
    decltype(&transcribe_was_aborted) was_aborted = nullptr;
    decltype(&transcribe_run) run = nullptr;
    decltype(&transcribe_full_text) full_text = nullptr;
};

void* resolve(ModuleHandle module, std::string_view name)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(module, std::string(name).c_str()));
#else
    return ::dlsym(module, std::string(name).c_str());
#endif
}

bool resolve_symbols(ModuleHandle module, Symbols& symbols, TranscribeProbe& probe)
{
    const auto bind = [&](std::string_view name, auto& target) {
        target = reinterpret_cast<std::decay_t<decltype(target)>>(resolve(module, name));
        if (target == nullptr) {
            probe.missing_symbols.emplace_back(name);
        }
    };
    bind("transcribe_version", symbols.version);
    bind("transcribe_abi_struct_size", symbols.abi_struct_size);
    bind("transcribe_model_load_params_init", symbols.model_load_params_init);
    bind("transcribe_session_params_init", symbols.session_params_init);
    bind("transcribe_run_params_init", symbols.run_params_init);
    bind("transcribe_capabilities_init", symbols.capabilities_init);
    bind("transcribe_open", symbols.open);
    bind("transcribe_session_free", symbols.session_free);
    bind("transcribe_get_model", symbols.get_model);
    bind("transcribe_model_get_capabilities", symbols.capabilities);
    bind("transcribe_set_abort_callback", symbols.set_abort_callback);
    bind("transcribe_was_aborted", symbols.was_aborted);
    bind("transcribe_run", symbols.run);
    bind("transcribe_full_text", symbols.full_text);
    return probe.missing_symbols.empty();
}

/// The library's own view of the structs the binding passes, compared with the
/// header this translation unit compiled against. A size mismatch means the DLL
/// is not the build the header describes, even if the version string matches -
/// which below 1.0 it may, since the ABI is explicitly allowed to break between
/// minor releases.
std::string struct_size_problem(const Symbols& symbols)
{
    struct Entry {
        const char* name;
        transcribe_abi_struct which;
        std::size_t expected;
    };
    const Entry entries[] = {
        {"transcribe_model_load_params", TRANSCRIBE_ABI_MODEL_LOAD_PARAMS, sizeof(struct transcribe_model_load_params)},
        {"transcribe_session_params", TRANSCRIBE_ABI_SESSION_PARAMS, sizeof(struct transcribe_session_params)},
        {"transcribe_run_params", TRANSCRIBE_ABI_RUN_PARAMS, sizeof(struct transcribe_run_params)},
        {"transcribe_capabilities", TRANSCRIBE_ABI_CAPABILITIES, sizeof(struct transcribe_capabilities)},
    };
    for (const auto& entry : entries) {
        const auto actual = symbols.abi_struct_size(entry.which);
        if (actual != entry.expected) {
            return std::string(entry.name) + ": DLL reports " + std::to_string(actual)
                + " bytes, the pinned header declares " + std::to_string(entry.expected);
        }
    }
    return {};
}

/// One loaded library, kept for the life of the PROCESS.
///
/// Why it is never unloaded (measured 2026-10-07: exit code 3 with
/// "GGML_ASSERT(prev != ggml_uncaught_exception) failed"): ggml installs a global
/// std::terminate handler from a static initializer at load time. Freeing the
/// module leaves that handler pointing into unloaded code, and loading the library
/// again at the same base address makes the initializer compare itself with its
/// own previous address and trip its own assert. The probe used to load and free
/// the library, so the engine's later load aborted the process. Keeping one module
/// (and one resolved symbol table) alive until process exit is both the safe and
/// the cheaper option: one load, one resolution.
struct LoadedLibrary {
    ModuleHandle module = nullptr;
    Symbols symbols;
    TranscribeProbe probe;
};

const LoadedLibrary& load_library_once(const std::filesystem::path& dll_path)
{
    static std::mutex mutex;
    static std::map<LibraryKey, LoadedLibrary> cache;

    const LibraryKey key =
#if defined(_WIN32)
        dll_path.wstring();
#else
        dll_path.string();
#endif
    std::lock_guard<std::mutex> lock(mutex);
    const auto existing = cache.find(key);
    if (existing != cache.end()) {
        return existing->second;
    }

    LoadedLibrary entry;
    entry.probe.library_path = dll_path;
    auto& probe = entry.probe;
    if (dll_path.empty()) {
        probe.detail = "no library path was configured";
        return cache.emplace(key, std::move(entry)).first->second;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(dll_path, ec) || ec) {
        probe.detail = "library not found: " + dll_path.string();
        return cache.emplace(key, std::move(entry)).first->second;
    }

    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR keeps the runtime's own directory as the
    // dependency search path, so its ggml/standard-library siblings are resolved
    // there and never from an unrelated directory that happens to be on PATH.
#if defined(_WIN32)
    entry.module = ::LoadLibraryExW(
        dll_path.wstring().c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (entry.module == nullptr) {
        probe.detail = "LoadLibraryExW failed for " + dll_path.string() + " (error "
            + std::to_string(::GetLastError()) + ")";
        return cache.emplace(key, std::move(entry)).first->second;
    }
#else
    // RTLD_NOW resolves the whole symbol set at load time, RTLD_GLOBAL makes the
    // library's own ggml symbols visible to its siblings, and RTLD_NODELETE keeps
    // it mapped for the process lifetime - the same reason the Windows build
    // never frees it (ggml installs a terminate handler at load time).
    entry.module = ::dlopen(dll_path.c_str(), RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
    if (entry.module == nullptr) {
        const char* reason = ::dlerror();
        probe.detail = "dlopen failed for " + dll_path.string() + " ("
            + (reason != nullptr ? std::string(reason) : std::string("unknown dynamic loader error"))
            + ")";
        return cache.emplace(key, std::move(entry)).first->second;
    }
#endif
    if (!resolve_symbols(entry.module, entry.symbols, probe)) {
        probe.detail = "the library is not the pinned transcribe.cpp build: missing symbols";
        return cache.emplace(key, std::move(entry)).first->second;
    }
    probe.library_loaded = true;

    const char* version = entry.symbols.version();
    probe.version = version == nullptr ? std::string() : std::string(version);
    if (probe.version != kTranscribeExpectedVersion) {
        probe.detail = "version mismatch: the library reports '" + probe.version + "', the pin expects '"
            + std::string(kTranscribeExpectedVersion) + "'";
        return cache.emplace(key, std::move(entry)).first->second;
    }
    const std::string problem = struct_size_problem(entry.symbols);
    if (!problem.empty()) {
        probe.detail = "struct size mismatch against the pinned header - " + problem;
        return cache.emplace(key, std::move(entry)).first->second;
    }
    probe.usable = true;
    probe.detail = "ok";
    return cache.emplace(key, std::move(entry)).first->second;
}

class DllTranscribeEngine final : public TranscribeEngine {
public:
    DllTranscribeEngine(ModuleHandle module, Symbols symbols, int threads)
        : module_(module)
        , symbols_(symbols)
        , threads_(threads)
    {
    }

    ~DllTranscribeEngine() override
    {
        free_model();
        // The module is deliberately NOT freed: see load_library_once(). The
        // handle is owned by the process-lifetime cache.
        static_cast<void>(module_);
    }

    [[nodiscard]] bool is_ready() const noexcept override { return session_ != nullptr; }
    [[nodiscard]] double max_audio_seconds() const override { return max_audio_seconds_; }
    [[nodiscard]] std::string last_error() const override { return last_error_; }

    domain::Status load_model(const std::filesystem::path& gguf_path, const CancellationToken& cancellation) override
    {
        if (auto cancelled = domain::check_cancelled(cancellation); cancelled.is_error()) {
            return cancelled;
        }
        if (!gguf_path.empty()) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(gguf_path, ec) || ec) {
                return domain::Status::failure(
                    ErrorCode::not_found, "gigaam: model file not found: " + gguf_path.string());
            }
        }
        free_model();

        struct transcribe_model_load_params load_params;
        symbols_.model_load_params_init(&load_params);
        // Offline CPU recognition only (parity row PRIV-01): the backend is
        // pinned instead of left on "auto", so a machine with a Vulkan driver
        // present cannot silently change where inference runs.
        load_params.backend = TRANSCRIBE_BACKEND_CPU;

        struct transcribe_session_params session_params;
        symbols_.session_params_init(&session_params);
        if (threads_ > 0) {
            session_params.n_threads = threads_;
        }

        const std::string path = narrow(gguf_path);
        transcribe_status status = symbols_.open(path.c_str(), &load_params, &session_params, &session_);
        if (status != TRANSCRIBE_OK || session_ == nullptr) {
            session_ = nullptr;
            last_error_ = std::string("gigaam: model load failed: ") + status_name(status);
            return domain::Status::failure(map_status(status), last_error_);
        }

        const struct transcribe_model* model = symbols_.get_model(session_);
        struct transcribe_capabilities capabilities;
        symbols_.capabilities_init(&capabilities);
        if (model != nullptr && symbols_.capabilities(model, &capabilities) == TRANSCRIBE_OK) {
            max_audio_seconds_ = capabilities.max_audio_ms > 0
                ? static_cast<double>(capabilities.max_audio_ms) / 1000.0
                : 0.0;
        }
        return domain::Status::success();
    }

    domain::Result<std::string> transcribe(
        const std::vector<float>& samples, const CancellationToken& cancellation) override
    {
        if (session_ == nullptr) {
            return domain::Result<std::string>::failure(
                ErrorCode::model_not_ready, "gigaam: model is not loaded");
        }
        if (samples.empty()) {
            return std::string();
        }
        if (auto cancelled = domain::check_cancelled(cancellation); cancelled.is_error()) {
            return domain::Result<std::string>::failure(
                cancelled.error().code(), cancelled.error().message());
        }

        struct transcribe_run_params run_params;
        symbols_.run_params_init(&run_params);
        run_params.task = TRANSCRIBE_TASK_TRANSCRIBE;
        run_params.timestamps = TRANSCRIBE_TIMESTAMPS_NONE;
        // No language hint: GigaAM-v3 is a Russian-only model and the engine
        // declares no language override (platform::EngineCapabilities).

        symbols_.set_abort_callback(session_, on_abort, const_cast<CancellationToken*>(&cancellation));
        const transcribe_status status = symbols_.run(
            session_, samples.data(), static_cast<int>(samples.size()), &run_params);
        const bool aborted = symbols_.was_aborted(session_);
        symbols_.set_abort_callback(session_, nullptr, nullptr);

        if (aborted || status == TRANSCRIBE_ERR_ABORTED) {
            last_error_.clear();
            return domain::Result<std::string>::failure(
                ErrorCode::cancelled, "gigaam: transcription cancelled");
        }
        if (status != TRANSCRIBE_OK) {
            last_error_ = std::string("gigaam: inference failed: ") + status_name(status);
            return domain::Result<std::string>::failure(map_status(status), last_error_);
        }

        const char* text = symbols_.full_text(session_);
        last_error_.clear();
        return trimmed(text == nullptr ? std::string() : std::string(text));
    }

    void free_model() noexcept override
    {
        if (session_ != nullptr) {
            symbols_.session_free(session_);
            session_ = nullptr;
        }
        max_audio_seconds_ = 0.0;
    }

private:
    ModuleHandle module_ = nullptr;
    Symbols symbols_;
    int threads_ = 0;
    transcribe_session* session_ = nullptr;
    double max_audio_seconds_ = 0.0;
    std::string last_error_;
};

#endif // _WIN32

} // namespace

TranscribeProbe probe_transcribe_runtime(const std::filesystem::path& dll_path)
{
#if defined(_WIN32) || defined(__linux__)
    return load_library_once(dll_path).probe;
#else
    TranscribeProbe probe;
    probe.library_path = dll_path;
    probe.detail = "the transcribe.cpp runtime is Windows-only in this product";
    return probe;
#endif
}

std::filesystem::path transcribe_library_beside_executable()
{
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
    const std::filesystem::path exe_path(buffer);
    return exe_path.parent_path() / std::filesystem::path(kTranscribeLibraryDirectory)
        / std::filesystem::path(kTranscribeLibraryName);
#elif defined(__linux__)
    std::vector<char> buffer(1024);
    while (buffer.size() <= 64 * 1024) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (length < 0) {
            return {};
        }
        if (static_cast<std::size_t>(length) < buffer.size() - 1) {
            buffer[static_cast<std::size_t>(length)] = '\0';
            const std::filesystem::path exe_path(buffer.data());
            const std::filesystem::path beside =
                exe_path.parent_path() / std::filesystem::path(std::string(kTranscribeSharedLibraryName));
            std::error_code beside_error;
            if (std::filesystem::exists(beside, beside_error) && !beside_error) {
                return beside;
            }
            // A development build keeps the engine libraries in its own
            // subdirectory (engine-libs), so the loader looks there before giving
            // up: the deployed layout and the build layout both work.
            const std::filesystem::path build_layout = exe_path.parent_path()
                / "engine-libs" / std::filesystem::path(std::string(kTranscribeSharedLibraryName));
            std::error_code build_error;
            if (std::filesystem::exists(build_layout, build_error) && !build_error) {
                return build_layout;
            }
            return beside;
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
#else
    return {};
#endif
}

Result<std::unique_ptr<TranscribeEngine>> open_transcribe_engine(
    const std::filesystem::path& dll_path,
    int threads,
    const CancellationToken& cancellation)
{
    if (auto cancelled = domain::check_cancelled(cancellation); cancelled.is_error()) {
        return Result<std::unique_ptr<TranscribeEngine>>::failure(
            cancelled.error().code(), cancelled.error().message());
    }

#if defined(_WIN32) || defined(__linux__)
    const LoadedLibrary& library = load_library_once(dll_path);
    if (!library.probe.usable) {
        const auto code = library.probe.library_loaded ? ErrorCode::unsupported : ErrorCode::unavailable;
        return Result<std::unique_ptr<TranscribeEngine>>::failure(code, "gigaam: " + library.probe.detail);
    }
    std::unique_ptr<TranscribeEngine> engine(new DllTranscribeEngine(library.module, library.symbols, threads));
    return engine;
#else
    static_cast<void>(threads);
    static_cast<void>(dll_path);
    return Result<std::unique_ptr<TranscribeEngine>>::failure(
        ErrorCode::unsupported, "gigaam: the transcribe.cpp runtime is Windows-only in this product");
#endif
}

} // namespace voicetyper::asr
