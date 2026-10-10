# VoiceTyper — ASR Module Specification

**Covers:** `src/asr/` (engine seam, engine host lifecycle, registry, parameters, concrete
engines, Silero segmenter) and the native libraries under `native/`.
**Baseline:** `main` @ `3cc4c5a9a602afb96c2f478a562005f5eb850068` (v2.2.1).
**Requirement keywords:** RFC 2119. Parent document: [index.spec.md](../index.spec.md).
**Prefix:** `VT-ASR-*`.

---

## 1. The port and its engines

### 1.1 `platform::Transcriber`

- **VT-ASR-101.** A transcriber MUST accept a complete 16 kHz mono PCM16 RIFF/WAVE buffer
  (`domain::WavAudio`) plus a `platform::TranscriptionRequest`, and MUST return the **full
  final** transcript. There MUST be no partial or streaming entry point
  ([src/platform/api/transcriber.hpp:11-37](../../src/platform/api/transcriber.hpp#L11-L37)).
- **VT-ASR-102.** `supports_streaming_partials` MUST stay `false` for every engine
  ([src/platform/api/transcriber.hpp:88-102](../../src/platform/api/transcriber.hpp#L88-L102)).
- **VT-ASR-103.** At most **one** engine may be loaded at a time and inference MUST be
  serialized (one mutex per engine); `transcribe()` is blocking and MUST be called from a
  worker thread.
- **VT-ASR-104.** `warmup()` MUST be idempotent and `deep_warmup(cancellation)` MUST run one
  short silence inference (4800 samples / 0.3 s) whose transcript is never delivered
  (`kWarmupSamples = 4800`, `kWarmupWavBytes = 9644`).
- **VT-ASR-105.** No engine may be substituted for another, and a transcript from a different
  model MUST never be returned (VT-RULE-001).

### 1.2 `TranscriptionRequest` and capabilities

- **VT-ASR-110.** `TranscriptionRequest` MUST carry `language` (default `ru`), `prompt`,
  `temperature` (default 0.0), `condition_on_previous_text`, `best_of` (state machine asks 3
  for the final result) and an optional `domain::SpeechMap` (empty = "detect for yourself").
- **VT-ASR-111.** `EngineCapabilities` MUST report truthfully which fields the engine honours:

| Engine | language override | prompt | temperature | previous context | best_of | streaming |
|---|---|---|---|---|---|---|
| Whisper | ✓ | ✓ | ✓ | ✗ | ✓ | ✗ |
| Parakeet v3 | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| GigaAM v3 | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |

### 1.3 Request normalization

- **VT-ASR-120.** `normalize(request)` MUST be the only place where the settings `double`
  temperature becomes a `float`, and it MUST reject a negative temperature
  (`out_of_range "temperature is below the engine minimum"`) and `best_of` outside `1..8`.
  A temperature above 1.0 MUST be **clamped** to 1.0 rather than rejected
  ([src/asr/engine_parameters.cpp:99-129](../../src/asr/engine_parameters.cpp#L99-L129)).
- **VT-ASR-121.** A whitespace-only prompt (Unicode-aware) MUST be dropped; otherwise it MUST
  be kept and `carry_initial_prompt` set. `no_context` MUST be
  `!condition_on_previous_text`.
- **VT-ASR-122.** The language code MUST be `"auto"` for `automatic` (never an empty string,
  which in whisper.cpp would mean "model default"), `"ru"` or `"en"`.
- **VT-ASR-123.** `normalize_parakeet` MUST return the default request
  (`decoder = 0` = default TDT head, `sample_rate = 16000`, empty `target_language`) and MUST
  ignore every request field: the C API has no prompt, no vocabulary and no language override.
- **VT-ASR-124.** Whisper decode parameters are **frozen** to the .NET values and MUST NOT be
  tuned silently: `no_speech_thold 0.6`, `temperature_inc 0`, `entropy_thold -1`,
  `logprob_thold -1`, greedy strategy, `greedy.best_of = max(1, best_of)`, `no_context` per
  VT-ASR-121, `detect_language = false`, `use_gpu = false`, `flash_attn = false`, all printing
  and progress callbacks null, and no `whisper_free_params` for the by-value parameter struct
  (freed-params is a heap-corruption trap, covered by `whisper-native-contract`).
- **VT-ASR-125.** Cancellation MUST be cooperative through whisper.cpp's `abort_callback`
  polling the token, so an in-flight decode actually stops.

---

## 2. Engine host (`VT-ASR-2xx`)

Contract: [src/asr/engine_host.hpp](../../src/asr/engine_host.hpp); test `asr-lifecycle-contract`
(11 scenarios).

- **VT-ASR-200.** `EngineHost` MUST implement `domain::TranscriptionPort` and own exactly one
  loaded engine plus a readiness state.
- **VT-ASR-201.** `EngineReadiness` MUST be `idle | unavailable | model_missing | loading |
  ready | warming | failed | shutting_down`, a separate axis from
  `platform::EngineAvailability`.
- **VT-ASR-202.** `transcribe()` MUST NEVER lazily load a model: a cold engine MUST report
  `model_not_ready` immediately instead of stalling the hotkey path.
- **VT-ASR-203.** `select(engine, model_path)` MUST bump the epoch and return immediately; the
  load and warm-up MUST run on a dedicated executor thread, NOT on the recording worker (a
  multi-second warm-up must not stall the VAD loop).
- **VT-ASR-204.** `EngineExecutor` MUST be a single-threaded LIFO-drop worker: a task posted
  with an epoch older than the newest MUST be dropped, and queued entries of an older epoch MUST
  be evicted (a superseded model switch must not download-and-load in the wrong order).
- **VT-ASR-205.** A background load that finishes after a newer `select()` MUST publish nothing
  and its engine MUST be destroyed.
- **VT-ASR-206.** A retired engine MUST be released only once nothing is in flight
  (`in_flight == 0`).
- **VT-ASR-207.** A load failure MUST map to a specific readiness: `not_found` → `model_missing`,
  `engine_unavailable` → `unavailable`, anything else → `failed`, always with the availability
  reason or the error text preserved.
- **VT-ASR-208.** An unavailable or ABI-mismatched engine MUST surface as
  `engine_unavailable` (or `model_not_ready` where appropriate) with a reason — never a Whisper
  fallback and never an empty transcript.
- **VT-ASR-209.** A late result arriving after cancellation MUST be discarded as `cancelled`.
- **VT-ASR-210.** `shutdown(deadline)`: on deadline expiry while a non-interruptible inference
  is running the call MUST return `timeout` and MUST **deliberately leak** the live context
  rather than free it under inference; a repeated `shutdown()` MUST still complete the release.
  The destructor MUST call `shutdown(2000 ms)`.
- **VT-ASR-211.** Readiness MUST be observable without blocking on the inference mutex, and the
  composition MUST poll it (every 500 ms) to log each transition once and post a terminal state
  to the status overlay's error state.

---

## 3. Engine registry (`VT-ASR-3xx`)

Contract: [src/asr/native_engine_registry.hpp](../../src/asr/native_engine_registry.hpp);
test `native-engine-registry-contract`.

- **VT-ASR-301.** The registry MUST be constructed with injected factories and probes so the
  "never substitute an engine" rule is testable on every host.
- **VT-ASR-302.** `availability()` MUST answer: a missing or empty registered path →
  `model_missing`; whisper with no factory or without the build flag → `platform_unsupported`;
  parakeet/gigaam → the probe's verdict, which is authoritative ("the registry never guesses").
- **VT-ASR-303.** `create()` MUST fail with `not_found` for a non-regular model file and
  `engine_unavailable "engine unavailable: <reason>"` when the engine is not available, and MUST
  pass the DLL path and the model path to the factory.
- **VT-ASR-304.** `set_model_path()` / `model_path()` MUST be guarded by one mutex and keyed by
  `TranscriptionEngine`.
- **VT-ASR-305.** The registry constants `kParakeetAbiVersion = 6` and
  `kParakeetPinnedCommit = e75de9b6b9b688fd293aa22f7e27aa724ea286f8` MUST agree with the
  dependency manifest and the vendored header; the agreement is checked at every configure and
  by `native-dependency-contract`.

---

## 4. Whisper (`VT-ASR-4xx`)

Contract: [src/asr/whisper_native.hpp](../../src/asr/whisper_native.hpp); tests
`whisper-native-contract`, `asr-native-smoke`.

- **VT-ASR-401.** Whisper MUST be compiled from the pinned source
  `ggml-org/whisper.cpp` @ `d09f61a708f3487afa956ff578e60eae5e7a233c` (upstream 1.9.4,
  ggml 0.25.1, MIT) and linked **statically**; `whisper.h` MUST be included by exactly one
  translation unit.
- **VT-ASR-402.** Threads MUST be clamped to `1..16`; `0` means "unspecified" and resolves to
  `1`. The physical-core derivation is not wired for Whisper, and the honest default is
  documented rather than guessed.
- **VT-ASR-403.** Loading MUST use `use_gpu = false`, `flash_attn = false`,
  `gpu_device = 0`, `dtw_token_timestamps = false`; a missing or non-regular file MUST be
  `not_found` naming the path; a null context MUST be `corrupt_data`; a cancel right after init
  MUST free the context and return `cancelled`.
- **VT-ASR-404.** The transcript MUST be the concatenation of all segment texts, trimmed; an
  empty buffer MUST be `invalid_argument`; a non-zero `whisper_full` MUST be `cancelled` when
  the token fired and `internal` (with the code) otherwise.
- **VT-ASR-405.** `WhisperTranscriber::warmup()` MUST be a no-op because construction is the
  warm-up (one context, no second load).

**Models** (q8 only, catalog frozen in `src/platform/model_catalog.cpp:27-33`):

| Size | File | Bytes |
|---|---|---|
| Tiny | `ggml-tiny-q8_0.bin` | 43,537,433 |
| Base | `ggml-base-q8_0.bin` | 81,768,585 |
| Small (default) | `ggml-small-q8_0.bin` | 264,464,607 |
| Medium | `ggml-medium-q8_0.bin` | 823,369,779 |
| Large turbo | `ggml-large-v3-turbo-q8_0.bin` | 874,188,075 |

- **VT-ASR-406.** The shipped binary baseline MUST be the generic x86-64 AVX2 class
  (`GGML_NATIVE=OFF`; SSE4.2/AVX/AVX2/FMA/F16C/BMI2 forced on, all AVX-512/VNNI off). The
  product MUST NOT claim to run on a CPU without that baseline (⚠ gap G-8: no runtime ISA check).

---

## 5. Parakeet v3 (`VT-ASR-5xx`)

Contract: [src/platform/windows/parakeet_runtime.hpp](../../src/platform/windows/parakeet_runtime.hpp).
See also [platform.spec.md](platform.spec.md) §VT-PLT-1xx for the port.

- **VT-ASR-501.** Parakeet MUST be loaded from the shipped `parakeet.dll` (5,682,591 bytes,
  SHA-256 `85c7c65bfd8d467799d6cb43bd3cfac884e27dce27680189b56edc5594ec932b`, ABI **6**) via
  `LoadLibraryW` + `GetProcAddress`, binding **exactly six** symbols: `parakeet_capi_abi_version`,
  `parakeet_capi_load`, `parakeet_capi_free`, `parakeet_capi_transcribe_pcm_lang`,
  `parakeet_capi_free_string`, `parakeet_capi_last_error`.
- **VT-ASR-502.** An ABI other than 6, a missing symbol, or a missing library MUST produce a
  specific `EngineAvailabilityReason` (`abi_mismatch`, `abi_mismatch`/`native_library_missing`,
  `native_library_missing`) with the observed and required values and the path — never a
  fallback. Off Windows the reason MUST be `platform_unsupported`.
- **VT-ASR-503.** The probe MUST free the module after inspecting it; the runtime MUST keep it
  for its lifetime.
- **VT-ASR-504.** `load_model()` MUST reject a second model in one runtime with
  `invalid_state "a model is already loaded in this runtime"`, and MUST fail with
  `corrupt_data` plus the native last error when the load returns null.
- **VT-ASR-505.** **Parakeet inference is not interruptible (decision D1).** Cancellation MUST
  be observed before and once after the native call; the maximum cancellation latency is one
  full inference and MUST be documented rather than "fixed" by killing the native context.
- **VT-ASR-506.** `sample_count > INT32_MAX` MUST be `out_of_range`; the returned malloc'd
  string MUST be copied and then freed with `free_string`; the result MUST be trimmed.
- **VT-ASR-507.** The engine MUST apply `normalize(request)` so an out-of-range temperature or
  `best_of` is rejected identically to Whisper, even though no field is applied
  (VT-ASR-111).

**Models** (`tdt-0.6b-v3`, language auto, base URL `mudler/parakeet-cpp-gguf`):

| Quant | File | Bytes |
|---|---|---|
| q4_k | `tdt-0.6b-v3-q4_k.gguf` | 675,200,864 |
| q5_k | `tdt-0.6b-v3-q5_k.gguf` | 741,867,360 |
| q6_k | `tdt-0.6b-v3-q6_k.gguf` | 812,700,512 |
| q8_0 (default) | `tdt-0.6b-v3-q8_0.gguf` | 940,663,680 |

---

## 6. GigaAM v3 (`VT-ASR-6xx`)

Contract: [src/asr/transcribe_engine.hpp](../../src/asr/transcribe_engine.hpp),
[src/asr/gigaam_transcriber.hpp](../../src/asr/gigaam_transcriber.hpp);
tests `gigaam-engine-contract`, `native-dependency-contract`.

- **VT-ASR-601.** GigaAM MUST be loaded from the pinned `transcribe.cpp` build: version exactly
  `"0.3.1"` (commit `3f32fbcc7bb3246851a0234263438bc3c0fa1cac`), library
  `transcribe/libtranscribe.dll` (2,679,873 bytes) in its **own directory** beside the
  executable, together with `ggml.dll`, `ggml-base.dll`, `ggml-cpu.dll` (different MinGW
  generation from the application, so its runtime siblings must not be confused with the app's).
- **VT-ASR-602.** The loader MUST bind exactly the 14 frozen symbols and MUST
  `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)` so
  sibling DLLs resolve inside the library's own directory and never from `PATH`.
- **VT-ASR-603.** The module MUST be loaded once per process and MUST NOT be unloaded: ggml
  installs a global `std::terminate` handler from a static initializer, and a second load at the
  same base address trips `GGML_ASSERT(prev != ggml_uncaught_exception)` (measured: exit code 3).
- **VT-ASR-604.** Readiness MUST require the exact version string and the `struct_size` of the
  four public structs the binding passes (`transcribe_model_load_params`,
  `transcribe_session_params`, `transcribe_run_params`, `transcribe_capabilities`) to match the
  vendored header. A mismatch is `unsupported`, never a downgrade; the four `*_init` symbols MUST
  be called because the library rejects a struct whose `struct_size` was not stamped by its own
  init.
- **VT-ASR-605.** The backend MUST be pinned to `TRANSCRIBE_BACKEND_CPU`: an accidental
  GPU/Vulkan pick on a machine with a driver is not acceptable for an offline CPU product.
- **VT-ASR-606.** `run_params` MUST set `task = TRANSCRIBE_TASK_TRANSCRIBE`,
  `timestamps = TRANSCRIBE_TIMESTAMPS_NONE`, no language hint (GigaAM v3 is Russian-only) and no
  vocabulary. An abort callback MUST be installed for the call and cleared afterwards;
  `transcribe_was_aborted` MUST be honoured.
- **VT-ASR-607.** `TRANSCRIBE_ERR_OUTPUT_TRUNCATED` and `OUTPUT_REPETITION` MUST map to
  `resource_exhausted`: a partial decode MUST never be treated as a complete dictation.
- **VT-ASR-608.** The model window MUST be read from the model's own capabilities
  (`transcribe_capabilities::max_audio_ms / 1000`), never hard-coded; `0` means "no practical
  limit". The window is a **soft** window for the GigaAM family: longer audio is accepted by the
  library with a warning and possibly lower accuracy, so the product cuts at pauses instead.
- **VT-ASR-609.** A dictation longer than the window MUST be cut at a pause and the parts
  joined: the chunk budget MUST be `max(1.0, window - 1.0)` seconds (one second of margin), the
  planner MUST be `domain::plan_speech_chunks`, and the parts MUST be joined with
  `domain::join_transcripts`.
- **VT-ASR-610.** If a prepared `SpeechMap` is present in the request the transcriber MUST reuse
  it and MUST NOT run the detector again; otherwise it MUST run the bound segmenter once
  (`reset()` + `detect_speech_no_reset()`) and take `last_frame_probabilities()` /
  `probability_frame_seconds()`.
- **VT-ASR-611.** With no speech spans for an over-long dictation the engine MUST refuse with
  `out_of_range "the dictation is longer than the model window (N s) and no speech segmenter is
  bound to cut it at a pause"` — an honest refusal, never an arbitrary cut.
- **VT-ASR-612.** Every chunk MUST be decoded separately (including the single-chunk case, so a
  long dictation is also freed of non-speech); cancellation MUST be checked between chunks; a
  failed chunk MUST fail the whole dictation; a pre-cancelled token MUST run no inference; an
  empty WAV MUST be `invalid_argument`.
- **VT-ASR-613.** `warmup()` MUST load the model (so a missing model is a readiness state, not a
  creation failure); a null engine MUST be `engine_unavailable`; `warmup()` MUST be idempotent
  when ready.
- **VT-ASR-614.** `max_audio_seconds()` MUST return the model-declared window (0 = unbounded),
  and `last_chunk_count()` MUST be exposed for diagnostics.

**Models** (`gigaam-v3-e2e-rnnt-gguf`, Russian only, punctuation and casing from the model):

| Quant | File | Bytes |
|---|---|---|
| q4_k_m | `gigaam-v3-e2e-rnnt-Q4_K_M.gguf` | 183,948,704 |
| q5_k_m | `gigaam-v3-e2e-rnnt-Q5_K_M.gguf` | 206,392,736 |
| q6_k | `gigaam-v3-e2e-rnnt-Q6_K.gguf` | 227,953,952 |
| q8_0 (default) | `gigaam-v3-e2e-rnnt-Q8_0.gguf` | 273,724,832 |

- **VT-ASR-615.** The q8_0 file shipped for validation is 273,724,832 bytes with SHA-256
  `78d63b47723b7f8d78c6113a6ef983b5a86e2a86f6c273e1f5cb6967b1c4467a`
  (`native/transcribe/BUILD.txt`).
- **VT-ASR-616.** GigaAM is a **C++-only extension** beyond the .NET reference: the legacy
  serializer cannot resolve `transcriptionEngine: "gigaam"` and its loader falls back to a
  whole-document default, so a settings backup MUST be taken before the user switches engines
  (parity decision 2026-10-06).

---

## 7. Silero VAD segmenter (`VT-ASR-7xx`)

Contract: [src/asr/silero_segmenter.hpp](../../src/asr/silero_segmenter.hpp);
test `silero-vad-contract`.

- **VT-ASR-701.** The detector MUST be Silero through the already pinned whisper.cpp
  (`whisper_vad_*`): no second runtime and no second ggml. The model is exactly
  `ggml-silero-v6.2.0.bin` (885,098 bytes) — the file the models page installs.
- **VT-ASR-702.** `open(model_path)` MUST reject an empty path with `invalid_argument`, and a
  missing file or a directory with `not_found`; a null context MUST be `io_failure`
  "could not load the model". `use_gpu` MUST be false and `n_threads` MUST be
  `clamp(hardware_concurrency, 1, 16)`.
- **VT-ASR-703.** `speech_pad_ms` MUST be **150** (upstream default 30) so a soft onset or a
  fading tail falls inside the span (Alexander's requirement of 2026-10-07). The other
  documented defaults MUST be kept: threshold 0.50, minimum speech 250 ms, minimum silence
  100 ms, and no forced max-speech split (chunk policy is the caller's).
- **VT-ASR-704.** One Silero frame is 512 samples at 16 kHz (31.9 ms);
  `probability_frame_seconds()` MUST report exactly `512/16000`.
- **VT-ASR-705.** The context MUST be guarded by one mutex (the machine streams from a worker
  while the trimming decorator runs after stop) and `reset()` MUST call
  `whisper_vad_reset_state`.
- **VT-ASR-706.** Segments MUST be built from the probability frames
  (`whisper_vad_segments_from_probs`), converted from centiseconds to seconds, and zero-length
  spans dropped; probabilities MUST be copied out before the segments are freed.
- **VT-ASR-707.** If the Silero model is missing or unloadable the application MUST log a warning
  and fall back to `domain::EnergySpeechSegmenter`; the fallback MUST be explicitly logged, and
  the energy heuristic MUST NOT be presented as Silero (it can be fooled by steady noise and has
  no speaker notion).
- **VT-ASR-708.** `VOICETYPER_VAD_MODEL` (test-only) MUST gate the model-dependent checks of
  `silero-vad-contract`; without it the test MUST print an explicit SKIP line rather than pass
  silently.

---

## 8. Native dependencies (`VT-ASR-8xx`)

Machine-readable source of truth: `docs/migration/cpp/native-dependencies.json`, read by
`cmake/NativeAsr.cmake`.

| Dependency | Kind | Pin | ABI | License | Bytes | SHA-256 |
|---|---|---|---|---|---|---|
| whisper.cpp + ggml | source-built, static | `d09f61a708f3487afa956ff578e60eae5e7a233c` (1.9.4 / ggml 0.25.1) | pin is the contract | MIT | archive 9,519,792 | archive `750655899a32d5bb5b04bbde9afa430f851d46718f53812a4ec086537fee3ef8` |
| `native/parakeet.dll` | prebuilt, LoadLibraryW | `e75de9b6b9b688fd293aa22f7e27aa724ea286f8` (v0.5.0-1) | **6** | MIT | 5,682,591 | `85c7c65bfd8d467799d6cb43bd3cfac884e27dce27680189b56edc5594ec932b` |
| `native/transcribe/libtranscribe.dll` | prebuilt, LoadLibraryExW | commit `3f32fbcc…`, tag v0.3.1 | version string `0.3.1` | MIT | 2,679,873 | `e1f74843382e2a36398c22a82d46d208eca2ac34ea49b372121ee3cac1e8e4e0` |
| `native/transcribe/ggml.dll` | dependency | same build | — | MIT | 112,861 | `da0989f0cbe0d144e9ca5f8d89b7ed623db31a116bc3416fb9783d3c952e2b64` |
| `native/transcribe/ggml-base.dll` | dependency | same build | — | MIT | 1,010,572 | `d51879f16e423fecdde3441dd325f91a6a32a6c25ab5be031c018609e3ef3807` |
| `native/transcribe/ggml-cpu.dll` | dependency (dynamically loaded) | same build | — | MIT | 1,489,298 | `ea49b1ca3e89787158fa619ab4bea32464db62b225eb4f6b2fb0fa929440b913` |
| `native/mc_wasapi.dll` | prebuilt, not bound | no ABI contract | — | — | 114,213 | `4f97ea7fed7670e91a99cc7ba233efc86f04de9e8cfb9e8f8da5bc08d16944e0` |
| `native/transcribe/transcribe.h` | vendored header | v0.3.1 | — | MIT | 141,687 | `d3617a0638387157af4f99596322d4c6bade2463aae88e0e76506fcd2130a082` |

- **VT-ASR-801.** Dependencies MUST be **pinned, not "latest"**: the pin lives in the manifest,
  `FetchContent` downloads a `URL_HASH`-verified archive of exactly that commit, and three files
  inside it are re-hashed against values taken from an independent `git fetch` of the same
  commit. A moved tag, a re-cut archive, a replaced or truncated DLL MUST be a configure error.
- **VT-ASR-802.** Only the pinned ABI may be bound: no header under `src/asr/*.hpp` or
  `src/platform/windows/*.hpp` may include `parakeet_capi.h`, `whisper.h` or `windows.h`
  (enforced by `native-dependency-contract`).
- **VT-ASR-803.** Unavailability MUST be explicit and diagnosable: `native_library_missing`,
  `platform_unsupported` or `abi_mismatch` with observed vs required values, plus the path and
  the Win32 error code where applicable.
- **VT-ASR-804.** **No network and no telemetry at recognition time, on any platform**: no
  native ASR component may open a connection, send telemetry/analytics/crash reports or download
  anything; models are downloaded only by the explicit in-app model store, never by an engine or
  a test. The manifest states this in machine-readable form and `native-dependency-contract`
  checks it (VT-SYS-001).
- **VT-ASR-805.** Offline contract builds MUST stay offline: `VOICETYPER_BUILD_ASR` defaults to
  `VOICETYPER_BUILD_GUI`, so a GUI-off portable build fetches nothing and still runs
  `native-dependency-contract`; `VOICETYPER_WHISPER_PREFETCHED_DIR` MUST verify the same content
  pins on a pre-fetched tree.
- **VT-ASR-806.** Two toolchain deviations are documented and MUST NOT be silently removed:
  `_WIN32_WINNT=0x0601` for the vendored sub-build (MinGW-w64 13.1 lacks
  `THREAD_POWER_THROTTLING_STATE`; only ggml's thread power-throttling optimisation is skipped)
  and using the commit archive instead of a git clone (CMake documents `GIT_SHALLOW` as
  incompatible with a hash `GIT_TAG`).
- **VT-ASR-807.** The unrelated upstream C++ target named `parakeet` that the whisper.cpp repo
  builds (`include/parakeet.h`) MUST NEVER be bound; it is a build side effect and is linked by
  nothing.

### Linux shared libraries (`VT-ASR-82x`)

Added 2026-10-10 with the multiplatform work (VT-SYS-010). The Linux build uses the
same three engines; Parakeet and GigaAM are shared libraries loaded with
`dlopen`/`dlsym` instead of `LoadLibraryW`.

| Dependency | Kind | Pin | ABI | License |
|---|---|---|---|---|
| whisper.cpp + ggml | source-built, static | `d09f61a708f3487afa956ff578e60eae5e7a233c` (1.9.4 / ggml 0.25.1) | pin is the contract | MIT |
| `libparakeet.so` | built from source, `dlopen` | `e75de9b6b9b688fd293aa22f7e27aa724ea286f8` (v0.5.0-1), ggml **static** | **6** | MIT |
| `libtranscribe.so` + `libggml*.so` (0.25.3) | built from source, `dlopen` | commit `3f32fbcc…`, tag v0.3.1 | version string `0.3.1` | MIT |

- **VT-ASR-820.** Parakeet and GigaAM MUST bind exactly the symbol lists and ABI of
  VT-ASR-5xx/6xx through `dlopen`/`dlsym` with `RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE`
  (transcribe needs its symbols to stay global for the ggml backend)
  ([src/platform/windows/parakeet_runtime.cpp](../../src/platform/windows/parakeet_runtime.cpp),
  [src/platform/windows/transcribe_runtime.cpp](../../src/platform/windows/transcribe_runtime.cpp),
  `#elif defined(__linux__)` branches).
- **VT-ASR-821.** The library MUST be looked for first next to the running executable and then in
  `<exe_dir>/engine-libs`, so a development build runs without copying files.
- **VT-ASR-822.** Unavailability MUST be `native_library_missing` with the loader's own message
  (including the undefined symbol), never `platform_unsupported`: Linux is a supported platform
  now (VT-ASR-803).
- **VT-ASR-823.** `libparakeet.so` MUST link its ggml statically: parakeet.cpp pins ggml 0.13 and
  transcribe.cpp pins ggml 0.25 while both shared libraries carry the same `libggml*.so.0`
  soname, so two shared copies cannot coexist in one process (VT-PLT-1604).
- **VT-ASR-824.** Whisper on Linux MUST use the same pinned whisper.cpp archive and the CPU
  backend as on Windows; no engine MAY be silently substituted when another one is unavailable.
- **VT-ASR-825.** The engine libraries MUST be built by the `voicetyper_parakeet_cpp` /
  `voicetyper_transcribe_cpp` ExternalProject targets and exposed through the
  `voicetyper-linux-engine-libs` target ([build-release.spec.md](build-release.spec.md) §VT-BLD-7xx).

---

## 9. Data model on the ASR boundary

| Type | Role | Fields |
|---|---|---|
| `domain::SampleBuffer` | accumulated capture | `samples()`, `size()`, `capacity()`, `append*`; default capacity 960000 samples, 512 MiB bound |
| `domain::WavAudio` | the currency at the engine boundary | `format` (16 kHz mono PCM16), `data`, `bytes`, `file_view()`, `payload_view()`, `payload_bytes()` |
| `domain::SpeechSegment` | one detected speech span | `start_seconds`, `end_seconds` |
| `domain::SpeechMap` | a detector's answer carried with the audio | `segments`, `probabilities`, `frame_seconds` |
| `platform::TranscriptionRequest` | per-session engine request | `language`, `prompt`, `temperature`, `condition_on_previous_text`, `best_of`, `speech_map` |
| `platform::EngineCapabilities` | what the engine honours | six booleans (VT-ASR-111) |
| `asr::NormalizedRequest` | engine-ready fields | `language_code`, `initial_prompt`, `temperature`, `no_context`, `carry_initial_prompt`, `best_of` |
| `asr::ParakeetRequest` | frozen Parakeet request | `decoder = 0`, `sample_rate = 16000`, empty `target_language` |
| `asr::WhisperDecodeOptions` | one field per `whisper_full_params` field | `language`, `prompt`, `carry_initial_prompt`, `temperature`, `best_of`, `no_context` |
| `asr::TranscribeProbe` | library inspection result | `library_loaded`, `version`, `usable`, `detail`, `missing_symbols`, `library_path` |
| `platform::ParakeetProbe` | library inspection result | `library_loaded`, `abi_version`, `usable`, `reason`, `detail`, `missing_symbols`, `library_path` |
| `asr::EngineReadiness` | host state | `idle … shutting_down` (8 values) |
| `asr::EngineStateSnapshot` | observable host state | `readiness`, `engine`, `model_path`, `abi_version`, `reason`, `last_error` |
| `platform::ModelDescriptor` | frozen catalog entry | `kind`, `size`, `parakeet_size`, `gigaam_size`, `file_name`, `download_url`, `expected_bytes` |

- **VT-ASR-901.** There is deliberately **no** `AudioChunk`, `TranscriptionResult`, `Segment`,
  `EngineParams` or `ModelInfo` type; a specification asserting them would be wrong.
- **VT-ASR-902.** Data crossing the native boundary:

| Boundary | Representation |
|---|---|
| host → transcriber | complete RIFF/WAVE PCM16 mono 16 kHz (`WavAudio`) + request |
| → whisper.cpp | `const float*`, `size_t`, `whisper_context*`, `whisper_full_params` |
| → parakeet.dll | `const float*`, `int` (rejects > `INT32_MAX`), `int sample_rate = 16000`, `int decoder = 0`, `const char* target_lang = ""` |
| → libtranscribe.dll | `const float*`, `int`, `*_params` structs stamped by `*_init`, borrowed `const char*` text, abort callback |
| → whisper.cpp VAD | `const float*`, `int`, probability frames, segments in centiseconds |

---

## 10. Known gaps

- **G-1 (closed 2026-10-10).** Real engines used to exist on Windows only (Parakeet reported
  `platform_unsupported`, GigaAM `unsupported`). All three now run on Linux: Whisper through the
  same pinned whisper.cpp, Parakeet and GigaAM through `libparakeet.so` / `libtranscribe.so`
  (VT-ASR-820…825); measured 2026-10-08 on Arch: Whisper tiny load 51 ms / warmup 2460 /
  transcribe 1212, GigaAM q8 58/29/137, Parakeet q8 `state=ready`.
- **G-2.** Parakeet cancellation latency is bounded only by one full inference (D1).
- **G-3.** Silero is optional at runtime; with no model file the product silently degrades to the
  energy heuristic (logged).
- **G-4.** `EngineHost::retire_locked()` is a no-op and `TranscribeProbe::version_commit` is
  never populated (`transcribe_version_commit` is not bound); `EngineExecutor::wait_idle()`
  waits for an empty queue, not for the running task.
- **G-5.** Model acceptance at load time is existence-only (`is_regular_file`); the catalog's
  exact byte sizes are not enforced when loading a model (risk R11 in `release-gates.md`).
- **G-6.** No real-speech golden corpus exists, so the release gate "WER/CER must not worsen by
  more than 0.02 absolute" has no corpus to observe; every checked-in WAV fixture is a sine or
  silence.
- **G-7.** Documentation drift: `docs/migration/cpp/windows-smoke.md` still says VAD uses the
  energy heuristic; `parity-ledger.md` says "22 properties" while the schema has 23;
  `tools/compare-contract-json.py` does not know `gigaam` as an engine value.
- **G-8.** No runtime ISA check: on a CPU below the AVX2 baseline Whisper may fail or run very
  slowly.
- **G-9.** `mc_wasapi.dll` is shipped and hash-verified but bound by no code (no C header, no ABI
  contract); the MME fallback does not exist in the C++ build.
