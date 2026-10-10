// Linux-only contract test for the native engine runtimes (Parakeet, GigaAM).
//
// What this proves, and what it deliberately does not:
//
//   Proves, on Linux, without the engines installed:
//     * the probe of a library that does not exist reports
//       native_library_missing with a reason, instead of claiming the engine is
//       usable or silently substituting another one;
//     * the library that belongs to the running executable is resolved to the
//       Linux shared-object name (libparakeet.so / libtranscribe.so), so the
//       composition looks for the right file;
//     * an empty path is reported as such rather than opened.
//
//   Deliberately NOT exercised automatically: loading a real model and
//   transcribing. That needs the model files and belongs to the physical Arch
//   Linux gate (plan p_b5fbda6bfa1d, phase 5); the asr-native-smoke target does
//   it by data when a model is configured.

#include "asr/transcribe_engine.hpp"
#include "platform/api/engine_registry.hpp"
#include "platform/windows/parakeet_runtime.hpp"

#include <iostream>
#include <string>

namespace {

using namespace voicetyper;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

void check_parakeet()
{
    using namespace voicetyper::platform;

    const auto absent = probe_parakeet_runtime("/tmp/voicetyper-no-such-parakeet.so");
    check(!absent.library_loaded, "a missing parakeet library is not loaded");
    check(absent.reason == EngineAvailabilityReason::native_library_missing,
        "a missing parakeet library reports native_library_missing");
    check(!absent.detail.empty(), "the parakeet probe explains why it failed");

    const auto empty = probe_parakeet_runtime({});
    check(!empty.library_loaded, "an empty parakeet path is not loaded");
    check(empty.reason == EngineAvailabilityReason::native_library_missing,
        "an empty parakeet path reports native_library_missing");

    const auto path = parakeet_library_beside_executable();
    check(!path.empty(), "the parakeet library path is resolved on Linux");
    check(path.filename() == "libparakeet.so", "the parakeet library is the Linux shared object");
    check(path.is_absolute(), "the parakeet library path is absolute");
}

void check_transcribe()
{
    using namespace voicetyper::asr;

    const auto absent = probe_transcribe_runtime("/tmp/voicetyper-no-such-transcribe.so");
    check(!absent.library_loaded, "a missing transcribe library is not loaded");
    check(!absent.usable, "a missing transcribe library is not usable");
    check(!absent.detail.empty(), "the transcribe probe explains why it failed");
    check(absent.library_path == std::filesystem::path("/tmp/voicetyper-no-such-transcribe.so"),
        "the transcribe probe reports the path it inspected");

    const auto path = transcribe_library_beside_executable();
    check(!path.empty(), "the transcribe library path is resolved on Linux");
    check(path.filename() == "libtranscribe.so", "the transcribe library is the Linux shared object");
    check(path.is_absolute(), "the transcribe library path is absolute");
}

} // namespace

int main()
{
    check_parakeet();
    check_transcribe();

    if (failures != 0) {
        std::cerr << "linux-engines-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "linux-engines-contract: OK\n";
    return 0;
}
