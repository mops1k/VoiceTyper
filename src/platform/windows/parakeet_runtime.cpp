// Parakeet DLL binding. The only VoiceTyper file that includes <windows.h> for
// this engine, and the only one that names the parakeet_capi_* symbols.
// Portable code sees parakeet_runtime.hpp instead, which is standard-C++20 and
// carries none of the native types.

#include "platform/windows/parakeet_runtime.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#if defined(__linux__)
#  include <dlfcn.h>
#  include <unistd.h>
#endif

namespace voicetyper::platform {
namespace {

#if defined(__linux__)
/// The shared object the Linux build loads. It is the same C ABI as the shipped
/// Windows DLL (mudler/parakeet.cpp, pinned commit), just built for this
/// platform, so the binding and the ABI assertion are unchanged.
inline constexpr std::string_view kParakeetSharedLibraryName = "libparakeet.so";
#endif

/// Opaque native context. Declared as void* on purpose: no parakeet typedef
/// (parakeet_ctx) is reproduced here, so nothing from the C header can leak
/// into portable code or into a signature.
using parakeet_context = void;

using AbiVersionFn = int (*)();
using LoadFn = parakeet_context* (*)(const char* gguf_path);
using FreeFn = void (*)(parakeet_context*);
using TranscribePcmLangFn = char* (*)(parakeet_context*, const float*, int, int, int, const char*);
using FreeStringFn = void (*)(char*);
using LastErrorFn = const char* (*)(parakeet_context*);

/// The six bound entry points, in the order of kParakeetRequiredSymbols.
struct ParakeetSymbols {
    AbiVersionFn abi_version = nullptr;
    LoadFn load = nullptr;
    FreeFn free_context = nullptr;
    TranscribePcmLangFn transcribe_pcm_lang = nullptr;
    FreeStringFn free_string = nullptr;
    LastErrorFn last_error = nullptr;

    [[nodiscard]] bool complete() const noexcept
    {
        return abi_version != nullptr && load != nullptr && free_context != nullptr
            && transcribe_pcm_lang != nullptr && free_string != nullptr && last_error != nullptr;
    }
};

std::string trimmed(std::string text)
{
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto first = std::find_if_not(text.begin(), text.end(), is_space);
    if (first == text.end()) {
        return {};
    }
    const auto last = std::find_if_not(text.rbegin(), text.rend(), is_space).base();
    return std::string(first, last);
}

[[maybe_unused]] std::string win32_error_text(unsigned long code)
{
    return "Win32 error " + std::to_string(code);
}

/// Resolves exactly kParakeetRequiredSymbols and reports which ones are
/// missing. Any other export of the DLL is deliberately left untouched.
/// Windows-only by nature, so it is marked maybe_unused for the builds that
/// prove the unavailable path instead of the loaded path.
[[maybe_unused]] std::vector<std::string> resolve_required_symbols(void* module, ParakeetSymbols& out)
{
    std::vector<std::string> missing;
#if defined(_WIN32)
    if (module == nullptr) {
        missing.emplace_back("library handle");
        return missing;
    }
    const auto resolve = [&module](std::string_view name) -> void* {
        return reinterpret_cast<void*>(
            ::GetProcAddress(static_cast<HMODULE>(module), std::string(name).c_str()));
    };
    out.abi_version = reinterpret_cast<AbiVersionFn>(resolve(kParakeetRequiredSymbols[0]));
    out.load = reinterpret_cast<LoadFn>(resolve(kParakeetRequiredSymbols[1]));
    out.free_context = reinterpret_cast<FreeFn>(resolve(kParakeetRequiredSymbols[2]));
    out.transcribe_pcm_lang = reinterpret_cast<TranscribePcmLangFn>(resolve(kParakeetRequiredSymbols[3]));
    out.free_string = reinterpret_cast<FreeStringFn>(resolve(kParakeetRequiredSymbols[4]));
    out.last_error = reinterpret_cast<LastErrorFn>(resolve(kParakeetRequiredSymbols[5]));

    if (!out.complete()) {
        const std::array<void*, 6> resolved = {
            reinterpret_cast<void*>(out.abi_version),
            reinterpret_cast<void*>(out.load),
            reinterpret_cast<void*>(out.free_context),
            reinterpret_cast<void*>(out.transcribe_pcm_lang),
            reinterpret_cast<void*>(out.free_string),
            reinterpret_cast<void*>(out.last_error),
        };
        for (std::size_t index = 0; index < kParakeetRequiredSymbols.size(); ++index) {
            if (resolved[index] == nullptr) {
                missing.emplace_back(kParakeetRequiredSymbols[index]);
            }
        }
    }
#elif defined(__linux__)
    if (module == nullptr) {
        missing.emplace_back("library handle");
        return missing;
    }
    const auto resolve = [&module](std::string_view name) -> void* {
        return ::dlsym(module, std::string(name).c_str());
    };
    out.abi_version = reinterpret_cast<AbiVersionFn>(resolve(kParakeetRequiredSymbols[0]));
    out.load = reinterpret_cast<LoadFn>(resolve(kParakeetRequiredSymbols[1]));
    out.free_context = reinterpret_cast<FreeFn>(resolve(kParakeetRequiredSymbols[2]));
    out.transcribe_pcm_lang = reinterpret_cast<TranscribePcmLangFn>(resolve(kParakeetRequiredSymbols[3]));
    out.free_string = reinterpret_cast<FreeStringFn>(resolve(kParakeetRequiredSymbols[4]));
    out.last_error = reinterpret_cast<LastErrorFn>(resolve(kParakeetRequiredSymbols[5]));

    if (!out.complete()) {
        const std::array<void*, 6> resolved = {
            reinterpret_cast<void*>(out.abi_version),
            reinterpret_cast<void*>(out.load),
            reinterpret_cast<void*>(out.free_context),
            reinterpret_cast<void*>(out.transcribe_pcm_lang),
            reinterpret_cast<void*>(out.free_string),
            reinterpret_cast<void*>(out.last_error),
        };
        for (std::size_t index = 0; index < kParakeetRequiredSymbols.size(); ++index) {
            if (resolved[index] == nullptr) {
                missing.emplace_back(kParakeetRequiredSymbols[index]);
            }
        }
    }
#else
    (void)module;
    (void)out;
    missing.emplace_back("LoadLibraryW");
    missing.emplace_back("GetProcAddress");
#endif
    return missing;
}

/// Builds the probe result from facts already collected, so the Windows and the
/// non-Windows path cannot drift apart in wording or ordering.
ParakeetProbe describe_probe(
    const std::filesystem::path& dll_path,
    const std::vector<std::string>& missing_symbols,
    bool module_opened,
    int abi,
    const std::string& load_failure,
    EngineAvailabilityReason unavailable_reason = EngineAvailabilityReason::native_library_missing)
{
    ParakeetProbe probe;
    probe.library_path = dll_path;
    probe.missing_symbols = missing_symbols;
    probe.abi_version = abi;
    probe.library_loaded = module_opened && missing_symbols.empty();

    if (!module_opened) {
        probe.reason = unavailable_reason;
        probe.detail = "parakeet runtime unavailable: " + load_failure;
        return probe;
    }
    if (!missing_symbols.empty()) {
        probe.reason = EngineAvailabilityReason::abi_mismatch;
        std::string detail = "parakeet.dll is present but does not export the pinned ABI "
            + std::to_string(kParakeetAbiVersion) + " entry points:";
        for (const std::string& symbol : missing_symbols) {
            detail += " " + symbol;
        }
        probe.detail = std::move(detail);
        return probe;
    }
    if (abi != kParakeetAbiVersion) {
        probe.reason = EngineAvailabilityReason::abi_mismatch;
        probe.detail = "parakeet.dll reports ABI version " + std::to_string(abi)
            + ", this build requires " + std::to_string(kParakeetAbiVersion);
        return probe;
    }

    probe.reason = EngineAvailabilityReason::available;
    probe.usable = true;
    probe.detail = "parakeet.dll " + dll_path.string() + " loaded, ABI version "
        + std::to_string(abi) + ", " + std::to_string(kParakeetRequiredSymbols.size())
        + " pinned symbols bound";
    return probe;
}

} // namespace

struct ParakeetRuntime::Impl {
#if defined(_WIN32)
    HMODULE module = nullptr;
#elif defined(__linux__)
    void* module = nullptr;
#endif
    ParakeetSymbols symbols;
    parakeet_context* model = nullptr;
    std::filesystem::path model_path;
    int abi = 0;
    ParakeetProbe probe;
};

ParakeetProbe probe_parakeet_runtime(const std::filesystem::path& dll_path)
{
    std::error_code ec;
    const bool exists = !dll_path.empty() && std::filesystem::is_regular_file(dll_path, ec);
    if (!exists) {
        ParakeetProbe probe;
        probe.library_path = dll_path;
        probe.reason = EngineAvailabilityReason::native_library_missing;
        probe.detail = dll_path.empty()
            ? "parakeet runtime unavailable: no library path was given"
            : "parakeet runtime unavailable: no such library file: " + dll_path.string();
        return probe;
    }

#if defined(_WIN32)
    HMODULE module = ::LoadLibraryW(dll_path.wstring().c_str());
    if (module == nullptr) {
        return describe_probe(
            dll_path, {}, false, 0,
            "LoadLibraryW failed for " + dll_path.string() + " ("
                + win32_error_text(::GetLastError()) + ")");
    }

    ParakeetSymbols symbols;
    const std::vector<std::string> missing = resolve_required_symbols(module, symbols);
    if (!missing.empty()) {
        ::FreeLibrary(module);
        return describe_probe(dll_path, missing, true, 0, {});
    }

    const int abi = symbols.abi_version != nullptr ? symbols.abi_version() : 0;
    ParakeetProbe probe = describe_probe(dll_path, {}, true, abi, {});
    ::FreeLibrary(module);
    return probe;
#elif defined(__linux__)
    void* module = ::dlopen(dll_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        const char* reason = ::dlerror();
        return describe_probe(
            dll_path, {}, false, 0,
            "dlopen failed for " + dll_path.string() + " ("
                + (reason != nullptr ? std::string(reason) : std::string("unknown dynamic loader error"))
                + ")");
    }

    ParakeetSymbols symbols;
    const std::vector<std::string> missing = resolve_required_symbols(module, symbols);
    if (!missing.empty()) {
        ::dlclose(module);
        return describe_probe(dll_path, missing, true, 0, {});
    }

    const int abi = symbols.abi_version != nullptr ? symbols.abi_version() : 0;
    ParakeetProbe probe = describe_probe(dll_path, {}, true, abi, {});
    ::dlclose(module);
    return probe;
#else
    return describe_probe(
        dll_path, {}, false, 0,
        dll_path.string() + " is a Windows DLL and this build is not Windows; Parakeet is "
        "explicitly unavailable here, and no other engine is substituted",
        EngineAvailabilityReason::platform_unsupported);
#endif
}

std::filesystem::path parakeet_library_beside_executable()
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
    return exe_path.parent_path() / std::filesystem::path(kParakeetLibraryName);
#elif defined(__linux__)
    // /proc/self/exe names the running image, so the library is looked for next
    // to the real binary even when the process was started through PATH.
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
                exe_path.parent_path() / std::filesystem::path(std::string(kParakeetSharedLibraryName));
            std::error_code beside_error;
            if (std::filesystem::exists(beside, beside_error) && !beside_error) {
                return beside;
            }
            // A development build keeps the engine libraries in its own
            // subdirectory (engine-libs), so the loader looks there before giving
            // up: the deployed layout and the build layout both work.
            const std::filesystem::path build_layout = exe_path.parent_path()
                / "engine-libs" / std::filesystem::path(std::string(kParakeetSharedLibraryName));
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

// --- runtime ----------------------------------------------------------------

ParakeetRuntime::ParakeetRuntime()
    : impl_(std::make_unique<Impl>())
{
}

ParakeetRuntime::~ParakeetRuntime()
{
    free_model();
#if defined(_WIN32)
    if (impl_ != nullptr && impl_->module != nullptr) {
        ::FreeLibrary(impl_->module);
        impl_->module = nullptr;
    }
#elif defined(__linux__)
    if (impl_ != nullptr && impl_->module != nullptr) {
        ::dlclose(impl_->module);
        impl_->module = nullptr;
    }
#endif
}

bool ParakeetRuntime::is_open() const noexcept
{
    return impl_ != nullptr && impl_->probe.usable;
}

bool ParakeetRuntime::is_ready() const noexcept
{
    return is_open() && impl_->model != nullptr;
}

int ParakeetRuntime::abi_version() const noexcept
{
    return impl_ != nullptr ? impl_->abi : 0;
}

const ParakeetProbe& ParakeetRuntime::probe() const noexcept
{
    return impl_->probe;
}

Result<std::unique_ptr<ParakeetRuntime>> ParakeetRuntime::open(
    const std::filesystem::path& dll_path, const CancellationToken& cancellation)
{
    if (auto cancelled = domain::check_cancelled(cancellation); cancelled.is_error()) {
        return Result<std::unique_ptr<ParakeetRuntime>>(cancelled.error());
    }

#if defined(_WIN32)
    if (dll_path.empty()) {
        return Result<std::unique_ptr<ParakeetRuntime>>::failure(
            ErrorCode::invalid_argument, "parakeet: no library path was given");
    }
    HMODULE module = ::LoadLibraryW(dll_path.wstring().c_str());
    if (module == nullptr) {
        return Result<std::unique_ptr<ParakeetRuntime>>::failure(
            ErrorCode::engine_unavailable,
            "parakeet: LoadLibraryW failed for " + dll_path.string() + " ("
                + win32_error_text(::GetLastError()) + ")");
    }

    std::unique_ptr<ParakeetRuntime> owned(new ParakeetRuntime());
    owned->impl_->module = module;
    const std::vector<std::string> missing = resolve_required_symbols(module, owned->impl_->symbols);
    if (!missing.empty()) {
        std::string detail = "parakeet: " + dll_path.string()
            + " does not export the pinned ABI " + std::to_string(kParakeetAbiVersion)
            + " entry points:";
        for (const std::string& symbol : missing) {
            detail += " " + symbol;
        }
        ::FreeLibrary(module);
        owned->impl_->module = nullptr;
        return Result<std::unique_ptr<ParakeetRuntime>>::failure(
            ErrorCode::engine_unavailable, std::move(detail));
    }

    owned->impl_->abi = owned->impl_->symbols.abi_version();
    owned->impl_->probe = describe_probe(dll_path, {}, true, owned->impl_->abi, {});
    if (owned->impl_->abi != kParakeetAbiVersion) {
        const int observed = owned->impl_->abi;
        ::FreeLibrary(module);
        owned->impl_->module = nullptr;
        return Result<std::unique_ptr<ParakeetRuntime>>::failure(
            ErrorCode::engine_unavailable,
            "parakeet: " + dll_path.string() + " reports ABI version " + std::to_string(observed)
                + ", this build requires " + std::to_string(kParakeetAbiVersion));
    }
    return owned;
#elif defined(__linux__)
    if (dll_path.empty()) {
        return Result<std::unique_ptr<ParakeetRuntime>>::failure(
            ErrorCode::invalid_argument, "parakeet: no library path was given");
    }
    void* module = ::dlopen(dll_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        const char* reason = ::dlerror();
        return Result<std::unique_ptr<ParakeetRuntime>>::failure(
            ErrorCode::engine_unavailable,
            "parakeet: dlopen failed for " + dll_path.string() + " ("
                + (reason != nullptr ? std::string(reason) : std::string("unknown dynamic loader error"))
                + ")");
    }

    std::unique_ptr<ParakeetRuntime> owned(new ParakeetRuntime());
    owned->impl_->module = module;
    const std::vector<std::string> missing = resolve_required_symbols(module, owned->impl_->symbols);
    if (!missing.empty()) {
        std::string detail = "parakeet: " + dll_path.string()
            + " does not export the pinned ABI " + std::to_string(kParakeetAbiVersion)
            + " entry points:";
        for (const std::string& symbol : missing) {
            detail += " " + symbol;
        }
        ::dlclose(module);
        owned->impl_->module = nullptr;
        return Result<std::unique_ptr<ParakeetRuntime>>::failure(
            ErrorCode::engine_unavailable, std::move(detail));
    }

    owned->impl_->abi = owned->impl_->symbols.abi_version();
    owned->impl_->probe = describe_probe(dll_path, {}, true, owned->impl_->abi, {});
    if (owned->impl_->abi != kParakeetAbiVersion) {
        const int observed = owned->impl_->abi;
        ::dlclose(module);
        owned->impl_->module = nullptr;
        return Result<std::unique_ptr<ParakeetRuntime>>::failure(
            ErrorCode::engine_unavailable,
            "parakeet: " + dll_path.string() + " reports ABI version " + std::to_string(observed)
                + ", this build requires " + std::to_string(kParakeetAbiVersion));
    }
    return owned;
#else
    (void)dll_path;
    return Result<std::unique_ptr<ParakeetRuntime>>::failure(
        ErrorCode::engine_unavailable,
        "parakeet: the shipped parakeet.dll is a Windows library and this build targets "
        "another platform, so Parakeet is unavailable here; no other engine is substituted");
#endif
}

Status ParakeetRuntime::load_model(
    const std::filesystem::path& gguf_path, const CancellationToken& cancellation)
{
    if (!is_open()) {
        return Status::failure(ErrorCode::engine_unavailable, "parakeet: runtime is not open");
    }
    if (auto cancelled = domain::check_cancelled(cancellation); cancelled.is_error()) {
        return cancelled;
    }
    if (gguf_path.empty()) {
        return Status::failure(ErrorCode::invalid_argument, "parakeet: empty model path");
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(gguf_path, ec)) {
        return Status::failure(
            ErrorCode::not_found, "parakeet: model file not found: " + gguf_path.string());
    }
    if (impl_->model != nullptr) {
        // One model per runtime; the engine registry owns that policy.
        return Status::failure(
            ErrorCode::invalid_state, "parakeet: a model is already loaded in this runtime");
    }

    const std::string path_text = gguf_path.string();
    parakeet_context* model = impl_->symbols.load(path_text.c_str());
    if (model == nullptr) {
        const std::string detail = last_error();
        return Status::failure(ErrorCode::corrupt_data,
            "parakeet: model could not be loaded: " + path_text
                + (detail.empty() ? std::string{} : " (" + detail + ")"));
    }
    impl_->model = model;
    impl_->model_path = gguf_path;
    return Status::success();
}

Result<std::string> ParakeetRuntime::transcribe_pcm(
    const float* samples,
    std::size_t sample_count,
    int sample_rate,
    int decoder,
    const std::string& target_lang,
    const CancellationToken& cancellation)
{
    if (!is_ready()) {
        return Result<std::string>::failure(ErrorCode::model_not_ready, "parakeet: no model loaded");
    }
    if (samples == nullptr || sample_count == 0) {
        return Result<std::string>::failure(
            ErrorCode::invalid_argument, "parakeet: empty PCM buffer");
    }
    if (sample_count > static_cast<std::size_t>(INT32_MAX)) {
        return Result<std::string>::failure(ErrorCode::out_of_range,
            "parakeet: PCM buffer is longer than the ABI int parameter allows");
    }
    // D1: the native call itself is not interruptible, so cancellation is
    // observed here and once more after the call returns.
    if (auto cancelled = domain::check_cancelled(cancellation); cancelled.is_error()) {
        return Result<std::string>(cancelled.error());
    }

    char* raw = impl_->symbols.transcribe_pcm_lang(
        impl_->model,
        samples,
        static_cast<int>(sample_count),
        sample_rate,
        decoder,
        target_lang.c_str());
    const bool was_cancelled = cancellation.is_cancellation_requested();
    if (raw == nullptr) {
        const std::string detail = last_error();
        if (was_cancelled) {
            return Result<std::string>::failure(
                ErrorCode::cancelled, "parakeet: transcription cancelled");
        }
        return Result<std::string>::failure(ErrorCode::engine_unavailable,
            "parakeet: transcription failed" + (detail.empty() ? std::string{} : ": " + detail));
    }

    std::string text(raw);
    impl_->symbols.free_string(raw);
    if (was_cancelled) {
        return Result<std::string>::failure(
            ErrorCode::cancelled, "parakeet: transcription cancelled");
    }
    return trimmed(std::move(text));
}

void ParakeetRuntime::free_model() noexcept
{
    if (impl_ != nullptr && impl_->model != nullptr && impl_->symbols.free_context != nullptr) {
        impl_->symbols.free_context(impl_->model);
        impl_->model = nullptr;
    }
}

std::string ParakeetRuntime::last_error() const
{
    if (impl_ == nullptr || impl_->symbols.last_error == nullptr) {
        return {};
    }
    const char* text = impl_->symbols.last_error(impl_->model);
    return text != nullptr ? std::string(text) : std::string{};
}

} // namespace voicetyper::platform
