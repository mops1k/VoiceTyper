// Compile/link contract for the pinned whisper.cpp dependency.
//
// This test exists to fail loudly if the native dependency stops being real:
// it includes the upstream <whisper.h> from the pinned FetchContent checkout,
// calls real C API entry points, and links against the static whisper/ggml
// archives that cmake/NativeAsr.cmake builds. If the pin, the headers or the
// static link break, this target does not compile or does not link.
//
// What it deliberately does NOT do: download or load a model, and run
// inference. Recognition quality, latency and RAM are the CPU-dispatch task
// (t_2d453cc0fd84) and the ASR gate (t_14a24cb19c23), not this one. The
// only model paths used here are deliberately wrong ones, to prove the engine
// reports a precise error instead of guessing.
//
// Printed greppable line: "whisper-native-contract: OK".

#include "asr/whisper_native.hpp"
#include "domain/cancellation.hpp"

#include <whisper.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
namespace asr = voicetyper::asr;
using voicetyper::domain::CancellationSource;
using voicetyper::domain::ErrorCode;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

bool is_hex_of_length(std::string_view text, std::size_t length)
{
    if (text.size() != length) {
        return false;
    }
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

void check_pin()
{
    const std::string_view commit = asr::whisper_pinned_commit();
    check(is_hex_of_length(commit, 40), "the compiled-in whisper.cpp pin is a full 40-hex commit");
    check(commit == VOICETYPER_WHISPER_EXPECTED_COMMIT,
        "the compiled-in whisper.cpp pin equals the manifest pin (" + std::string(commit) + ")");
    check(is_hex_of_length(asr::whisper_archive_sha256(), 64),
        "the compiled-in archive SHA-256 is recorded");
    check(asr::whisper_archive_sha256() == VOICETYPER_WHISPER_EXPECTED_ARCHIVE_SHA256,
        "the archive SHA-256 compiled into the build equals the manifest value");
    check(!asr::whisper_upstream_version().empty(), "the upstream version is reported");
    check(asr::whisper_license() == "MIT", "whisper.cpp is MIT licensed");
    check(asr::whisper_repository().find("github.com/ggml-org/whisper.cpp") != std::string_view::npos,
        "the repository is reported");
}

/// Real calls into the pinned static library. No model is involved: these
/// only exercise the parameter constructors and the matching free function,
/// which is enough to prove the header, the archive and the linker agree.
void check_upstream_api_surface()
{
    whisper_context_params context_params = whisper_context_default_params();
    check(context_params.use_gpu == true, "upstream defaults to a GPU-capable context");
    context_params.use_gpu = false;
    context_params.flash_attn = false;
    check(context_params.use_gpu == false && context_params.flash_attn == false,
        "the wrapper builds the offline CPU-only context this product requires");
    whisper_context_params* heap_context_params = whisper_context_default_params_by_ref();
    check(heap_context_params != nullptr, "whisper_context_default_params_by_ref links");
    if (heap_context_params != nullptr) {
        whisper_free_context_params(heap_context_params);
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    check(params.strategy == WHISPER_SAMPLING_GREEDY, "greedy sampling is the compiled default");
    check(params.n_threads > 0, "upstream picks a positive default thread count");

    // The frozen .NET decoder constants are compared against upstream's own
    // defaults, so a change in whisper.cpp that would silently alter the
    // product's decoding behaviour fails here.
    check(params.no_speech_thold == asr::kWhisperNoSpeechThreshold,
        "upstream no_speech_thold default is the frozen 0.6 the .NET engine uses");
    check(params.logprob_thold == asr::kWhisperLogProbThreshold,
        "upstream logprob_thold default is the frozen -1 (no logprob fallback)");
    check(params.no_context == true, "upstream defaults to no previous-text context, like .NET");
    check(params.temperature_inc != asr::kWhisperTemperatureIncrement,
        "the .NET temperature increment 0 is an explicit override of upstream, not an accident");
    check(params.entropy_thold != asr::kWhisperEntropyThreshold,
        "the .NET entropy threshold -1 is an explicit override of upstream, not an accident");

    // The fields the wrapper actually writes must exist in this pinned header.
    params.language = "ru";
    params.initial_prompt = "API, CPU";
    params.carry_initial_prompt = true;
    params.temperature = 0.0f;
    params.temperature_inc = asr::kWhisperTemperatureIncrement;
    params.entropy_thold = asr::kWhisperEntropyThreshold;
    params.logprob_thold = asr::kWhisperLogProbThreshold;
    params.no_speech_thold = asr::kWhisperNoSpeechThreshold;
    params.greedy.best_of = 3;
    params.no_context = true;
    params.n_threads = 4;
    params.print_progress = false;
    params.print_realtime = false;
    params.abort_callback = nullptr;
    params.abort_callback_user_data = nullptr;
    check(params.greedy.best_of == 3 && params.n_threads == 4,
        "the wrapper's decode parameters are writable in this pinned header");
    check(params.language != nullptr, "language assignment sticks");

    // whisper_free_params() does `delete` on its argument, so it is only legal
    // for the pointer returned by ..._by_ref(). Calling it on the by-value
    // struct corrupts the heap; that trap is worth documenting in the test.
    whisper_full_params* heap_params = whisper_full_default_params_by_ref(WHISPER_SAMPLING_GREEDY);
    check(heap_params != nullptr, "whisper_full_default_params_by_ref links");
    if (heap_params != nullptr) {
        whisper_free_params(heap_params);
    }
}

void check_thread_clamp()
{
    check(asr::clamp_whisper_threads(0) == 1, "unspecified thread count clamps to 1");
    check(asr::clamp_whisper_threads(-4) == 1, "a negative thread count clamps to 1");
    check(asr::clamp_whisper_threads(1) == 1, "the lower bound is 1");
    check(asr::clamp_whisper_threads(8) == 8, "a valid thread count is kept");
    check(asr::clamp_whisper_threads(16) == 16, "the upper bound is 16");
    check(asr::clamp_whisper_threads(64) == 16, "an oversized thread count clamps to 16");
    check(asr::kWhisperMinThreads == 1 && asr::kWhisperMaxThreads == 16,
        "the .NET 1..16 inference clamp is frozen in the wrapper");
}

/// Model loading must report a precise error for a wrong path and never
/// pretend to be ready. No real model is touched, so this test can run on a
/// machine that has no models at all.
void check_load_failures(const fs::path& scratch_dir)
{
    const auto absent = asr::WhisperNativeContext::load(
        scratch_dir / "definitely-not-a-model.bin", {}, {});
    check(absent.is_error(), "loading a missing model fails");
    if (absent.is_error()) {
        check(absent.code() == ErrorCode::not_found, "a missing model file reports not_found");
        check(absent.error().message().find("definitely-not-a-model.bin") != std::string::npos,
            "the not_found message names the model path");
    }

    // A file that exists but is not a ggml model must be rejected as corrupt,
    // not accepted and not silently substituted.
    const fs::path bogus = scratch_dir / "not-a-ggml-model.bin";
    {
        std::ofstream out(bogus, std::ios::binary);
        out << "this is not a ggml model, it is a test fixture that stays small on purpose";
    }
    const auto corrupt = asr::WhisperNativeContext::load(bogus, {}, {});
    check(corrupt.is_error(), "loading a file that is not a model fails");
    if (corrupt.is_error()) {
        check(corrupt.code() == ErrorCode::corrupt_data, "a non-model file reports corrupt_data");
    }
    std::error_code ec;
    fs::remove(bogus, ec);

    const auto empty_path = asr::WhisperNativeContext::load({}, {}, {});
    check(empty_path.is_error(), "loading with an empty path fails");
    if (empty_path.is_error()) {
        check(empty_path.code() == ErrorCode::invalid_argument,
            "an empty model path reports invalid_argument");
    }

    // A request that is already cancelled must not start any work.
    CancellationSource cancelled;
    cancelled.request_cancellation();
    const auto aborted = asr::WhisperNativeContext::load(bogus, {}, cancelled.token());
    check(aborted.is_error(), "loading with a cancelled token fails");
    if (aborted.is_error()) {
        check(aborted.code() == ErrorCode::cancelled, "a cancelled load reports cancelled");
    }
}

} // namespace

int main()
{
    check_pin();
    check_upstream_api_surface();
    check_thread_clamp();

    std::error_code ec;
    const fs::path scratch_dir = fs::temp_directory_path(ec) / "voicetyper-whisper-native-contract";
    fs::create_directories(scratch_dir, ec);
    check_load_failures(scratch_dir);

    std::cout << "whisper.cpp pin " << asr::whisper_pinned_commit()
              << " (upstream " << asr::whisper_upstream_version()
              << ", " << asr::whisper_license() << ", static, offline)\n";

    if (failures != 0) {
        std::cerr << "whisper-native-contract: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "whisper-native-contract: OK\n";
    return 0;
}
