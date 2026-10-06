# Native ASR dependency contract (whisper.cpp + parakeet.cpp)

**Task:** `t_c6a6b0ce9685` (Phase C, plan `p_312b2ec83985`)
**Machine-readable source of truth:** [`native-dependencies.json`](native-dependencies.json)
— `cmake/NativeAsr.cmake` reads it, so the build cannot drift from this document.

## What is integrated

| Engine | Mechanism | Pin | ABI | License |
|---|---|---|---|---|
| Whisper | compiled from source into the binary, **static** | `ggml-org/whisper.cpp` @ `d09f61a708f3487afa956ff578e60eae5e7a233c` (upstream 1.9.4, ggml 0.25.1) | no C ABI version; the pin is the contract | MIT |
| Parakeet | prebuilt `parakeet.dll`, loaded at runtime with `LoadLibraryW`/`GetProcAddress` | `mudler/parakeet.cpp` @ `e75de9b6b9b688fd293aa22f7e27aa724ea286f8` (`v0.5.0-1-ge75de9b`) | **6**, asserted at load time | MIT |

`mc_wasapi.dll` is recorded in the manifest with its size and SHA-256 as a Phase D
artifact, but has no C ABI contract in the repository yet and is not bound by
any code.

## The rules this integration keeps

1. **No silent engine fallback.** `ParakeetRuntime::open()` and
   `WhisperNativeContext::load()` return a `Status`/`Result` with a specific
   `ErrorCode` and a human-readable detail. Nothing in `src/asr` or
   `src/platform/windows` can reach for another engine or another model.
2. **Only the pinned ABI is bound.** Six C symbols, declared as function
   pointers in `parakeet_runtime.cpp`:
   `parakeet_capi_abi_version`, `parakeet_capi_load`, `parakeet_capi_free`,
   `parakeet_capi_transcribe_pcm_lang`, `parakeet_capi_free_string`,
   `parakeet_capi_last_error`. The context handle is an opaque `void*`; no
   `parakeet_capi.h` type crosses into portable code, and no header under
   `src/asr` or `src/platform/windows` includes `parakeet_capi.h`, `whisper.h`
   or `windows.h` (enforced by the `native-dependency-contract` CTest).
3. **Unavailability is explicit and diagnosable.**
   `parakeet.dll` missing → `native_library_missing`; present on a non-Windows
   host → `platform_unsupported`; present but a different build → `abi_mismatch`
   with the observed and required versions. Each carries the path and, on
   Windows, the Win32 error code.
4. **No network and no telemetry at recognition time.** whisper.cpp is a static
   library; parakeet.dll is loaded locally. Models are downloaded only by the
   explicit in-app model store, never by an engine, the loader, or any test.
   The manifest states this in machine form and the contract test checks it.
5. **Reproducible.** The pin, license, ABI, artifact hashes and build options
   live in one JSON file. FetchContent downloads a content-addressed archive
   (`URL_HASH`), and three files inside it are re-hashed against values taken
   from an independent `git fetch` of the same commit, so the archive is proven
   to be that commit's tree. A replaced or truncated shipped DLL is a configure
   error, not a runtime surprise.

## whisper.cpp build options (all forced in `cmake/NativeAsr.cmake`)

`BUILD_SHARED_LIBS=OFF`, `WHISPER_BUILD_TESTS=OFF`, `WHISPER_BUILD_EXAMPLES=OFF`,
`WHISPER_BUILD_SERVER=OFF`, `WHISPER_BUILD_IS_DEV=OFF`, `WHISPER_CURL=OFF`,
`WHISPER_SDL2=OFF`, `GGML_NATIVE=OFF`, `GGML_OPENMP=OFF`, `GGML_LTO=OFF`,
`GGML_CCACHE=OFF`, `GGML_BACKEND_DL=OFF`, `GGML_ACCELERATE=OFF`, `GGML_BLAS=OFF`,
`GGML_RPC=OFF`, `GGML_CUDA=OFF`, `GGML_VULKAN=OFF`, `GGML_METAL=OFF`,
`GGML_KOMPUTE=OFF`, `GGML_SYCL=OFF`, `GGML_WEBGPU=OFF`,
`GGML_WASM_SINGLE_FILE=OFF`, `GGML_CPU_ALL_VARIANTS=OFF`.

Options that do not exist at this pin, and why they are absent:
`GGML_WASM` (the WASM backend is only added under `EMSCRIPTEN`, which VoiceTyper
never enables), speech-dispatcher (no such target exists anywhere in the tree at
this pin), `WHISPER_BUILD_TOOLS` (every command-line tool of that era lives
under `examples/`, which is off).

### x86-64 instruction-set baseline (pinned 2026-10-01, `t_2d453cc0fd84`)

`GGML_SSE42`, `GGML_AVX`, `GGML_AVX2`, `GGML_FMA`, `GGML_F16C` and `GGML_BMI2`
are forced **ON** for `CMAKE_SYSTEM_PROCESSOR` matching `x86_64|amd64`;
`GGML_AVX512`, `GGML_AVX_VNNI`, `GGML_AVX512_VBMI`, `GGML_AVX512_VNNI` and
`GGML_AVX512_BF16` are forced OFF, and `GGML_NATIVE` stays OFF. This is the
generic AVX2 baseline, not host tuning; ggml's own runtime dispatch still picks
the kernels, so the binary needs a CPU with SSE4.2/AVX/AVX2/FMA/F16C/BMI2 —
exactly what upstream builds by default on a non-cross build.

Why it is pinned rather than inherited: ggml enables those options only when
`GGML_NATIVE_DEFAULT` is ON, and upstream sets it OFF as soon as
`CMAKE_CROSSCOMPILING` is true. **CMake sets `CMAKE_CROSSCOMPILING=TRUE` for
every build that uses a toolchain file**, and VoiceTyper's MinGW toolchain is
one, so on a native Windows host with `CMAKE_HOST_SYSTEM_NAME ==
CMAKE_SYSTEM_NAME == "Windows"` the flag was still TRUE and the vendored ggml was
compiled for plain x86-64 SSE2. Measured on the target machine with the
installed `ggml-small-q8_0.bin` (read-only) and the 2.000 s fixture
`wav-silence-speech-silence.wav`:

| Build | load | deep warm-up | transcribe |
|---|---|---|---|
| Windows MinGW13, SSE2 (before the pin) | 577 ms | 145 637 ms | 62 946 ms |
| Windows MinGW13, pinned baseline (after) | 544 ms | 29 371 ms | 14 601 ms |
| Arch, pinned baseline (same source) | 1 801 ms | 24 045 ms | 15 046 ms |
| .NET baseline on the same machine (`phase-0-baseline.md:90`) | — | 19 434 ms | — |

The transcript was `*звук*` in every run and the model file was byte-identical
before and after (264 464 607 bytes). The configure step now prints
`whisper.cpp: pinned the generic x86-64 baseline (...)` together with the
observed `CMAKE_CROSSCOMPILING` value, so a future toolchain change that drops
the kernels again is visible in the configure log instead of only in the timings.

**Caution:** the whisper.cpp repository at this commit also declares an
unconditional C++ target literally named `parakeet` (`include/parakeet.h`, no ABI
versioning). It is unrelated to the shipped `parakeet.dll`, and VoiceTyper must
never bind it. It is built as a side effect of the sub-build and linked by
nothing.

## Frozen Whisper decode parameters (parity with `WhisperEngine.cs`)

`n_threads` clamped to 1..16, greedy search, `no_speech_thold = 0.6`,
`temperature_inc = 0`, `entropy_thold = -1`, `logprob_thold = -1` (no
temperature/entropy fallback), `best_of` from the caller (3 for the final
result), `no_context` when previous context is not requested, prompt with
`carry_initial_prompt`, `language` `"ru"`/`"en"` or `nullptr`/`""`/`"auto"` for
detection, CPU only, no progress or realtime printing. Cancellation is polled
from whisper.cpp's `abort_callback`, so a cancel stops the compute instead of
waiting for the whole buffer. The contract test compares these against
upstream's own defaults, so an upstream change that would silently alter
decoding fails the build.

## Build commands

CMake in this WSL workspace is at `/home/mops1k/.local/tools/cmake/bin`
(3.31.6); it is not on `PATH` by default.

### Arch Linux, GUI off, native ASR dependency ON (network once)

```sh
export PATH="/home/mops1k/.local/tools/cmake/bin:$PATH"
cmake --preset linux-arch-native-gui-off
cmake --build --preset linux-arch-native-gui-off -j"$(nproc)"
ctest --preset linux-arch-native-gui-off
```

### Arch Linux, fully offline portable contract build (no network at all)

`VOICETYPER_BUILD_ASR` defaults to `VOICETYPER_BUILD_GUI`, so turning the GUI off
turns the whisper.cpp fetch off with it:

```sh
cmake -S . -B build/contract-gui-off -DVOICETYPER_BUILD_GUI=OFF -DVOICETYPER_BUILD_ASR=OFF
cmake --build build/contract-gui-off -j"$(nproc)"
ctest --test-dir build/contract-gui-off
```

The `native-dependency-contract` CTest still runs there: the manifest, the ABI
constant, the DLL hashes and the "Parakeet is explicitly unavailable" path are
all verifiable without whisper.cpp and without a model.

### Windows, MinGW13 + Qt 6.11.2 (GUI on, so ASR on)

MinGW13 **must** come before WinLibs on `PATH`.

```powershell
$env:VOICETYPER_MINGW_ROOT = 'C:\Users\Kvintilyanov\tools\Qt\Tools\Tools\mingw1310_64'
$env:VOICETYPER_QT_ROOT    = 'C:\Users\Kvintilyanov\tools\Qt\6.11.2-mingw-retry\6.11.2\mingw_64'
$env:PATH = "$env:VOICETYPER_MINGW_ROOT\bin;C:\Users\Kvintilyanov\tools\mingw64\bin;" + $env:PATH
cmake --preset windows-mingw-release --fresh
cmake --build --preset windows-mingw-release
ctest --preset windows-mingw-release
```

### Offline escape hatch for build machines without network access

`VOICETYPER_WHISPER_PREFETCHED_DIR=<path>` makes the build use a pre-fetched
whisper.cpp tree instead of downloading. This is **not** a weaker contract: the
three manifest content pins are still verified against that tree, so it is
accepted only when it is byte-identical to the pinned commit.

```sh
cmake -S . -B build/x -DVOICETYPER_BUILD_ASR=ON -DVOICETYPER_WHISPER_PREFETCHED_DIR=/path/to/whisper.cpp
```

## Two documented toolchain deviations

1. **Windows API level of the vendored sub-build (`_WIN32_WINNT=0x0601`).**
   MinGW-w64 13.1 does not provide `THREAD_POWER_THROTTLING_STATE`, which
   `ggml-cpu.c` uses inside `#if _WIN32_WINNT >= 0x0602`; with the MinGW13
   default of `0xa00` the pinned whisper.cpp therefore does not compile. The
   vendored `ggml`/`whisper` targets are pinned to the Windows 7 API level, which
   compiles that block out. Consequence: only ggml's "ask Windows not to throttle
   this thread" optimisation is skipped — a possible throughput difference on
   Windows 11 with more than four threads, not a correctness change. Override
   with `-DVOICETYPER_WHISPER_WINDOWS_API_LEVEL=default` to build without it.
2. **`FetchContent` uses the commit archive, not a git clone.** CMake documents
   that `GIT_SHALLOW` does not work with a commit hash as `GIT_TAG`
   ("works only with branch names and tags"), and a full 48 MB history clone took
   2m34s on this WSL side. The archive URL embeds the commit and `URL_HASH` pins
   the bytes exactly, so the build is content-addressed and needs no git.

## Validation evidence (2026-09-25, current tree)

| Run | Configure | Build | CTest | Notes |
|---|---|---|---|---|
| Arch Release, GUI off, `VOICETYPER_BUILD_ASR=ON` | 0 (50.7 s) | 0 (2m56 s) | 0, **19/19** | whisper fetched, `libwhisper.a` 914,442 B + `libggml-cpu.a` 1,482,374 B static, zero warnings |
| Arch Release, GUI off, `VOICETYPER_BUILD_ASR=OFF` (offline) | 0 | 0 (my targets) | 0, 1/1 for `native-dependency-contract` | no network touched |
| Windows MinGW13/Qt 6.11.2 Release, `VOICETYPER_BUILD_ASR=ON` | 0 (9.3 s) | 0 (33.9 s) | 0, **19/19** | `g++ 13.1.0 x86_64-w64-mingw32` first on PATH; **parakeet ABI probe returned 6** with all six symbols bound; zero build warnings |

Direct runs: `voicetyper-whisper-native-contract` 0/0/0 on Arch, 0 on Windows;
`voicetyper-native-dependency-contract` 0 on both. The manifest digest printed by
the contract test is
`6e299586f7290ebad789aad62bc396fb01d3ea89bc1d03ef36ef7c908dff7983`.

## What is NOT claimed here

- No recognition quality, latency or RAM measurement: no model was loaded and no
  inference was run (CPU dispatch and performance parity are `t_2d453cc0fd84`).
- No .NET differential transcript comparison (that is the ASR gate
  `t_14a24cb19c23`).
- The `WhisperNativeContext::transcribe` "model_not_ready" branch is defensive
  only: no caller can hold an unloaded instance until the lifecycle task
  (`t_925b0c0e4f93`) exists.
- Parakeet inference is not interruptible (contract decision D1): cancellation is
  observed before and after the native call, so the maximum cancellation latency
  is one inference, and the native context is never killed from another thread.
