# VoiceTyper C++ parity ledger

**Baseline:** C# `main` @ `fc7d69c1a32be71a19634c16904d0a29a24135b0` (`v1.1.3`)  
**Policy:** intent parity. The stated user-facing functionality and this ledger are normative; current C# no-op/partial gaps are not reproduced as bugs.  
**Endpoint:** native C++/Qt app builds, launches and passes Windows smoke scenarios.

A row is not `done` merely because a C++ file exists. It requires implementation, contract/differential evidence, and a Windows launch or physical scenario.

| ID | Feature / user intent | C# evidence | C++ contract/test target | Windows launch scenario | Status |
|---|---|---|---|---|---|
| SHELL-01 | QApplication composition root, main window, tray-resident lifecycle, clean shutdown | `VoiceTyper.App/App.axaml.cs:71-91` | `voicetyper_app` + shell tests | start/visible/hidden/close-to-tray/quit | planned |
| SHELL-02 | Cross-version single instance, second instance handled without corrupting settings | `App.axaml.cs:27,112-117,214-230` | `single_instance` contract | second launch while first is running | planned |
| SET-01 | All settings pages/controls and exact persistence | `MainWindow.axaml:150-787`, `SettingsService.cs:20-77` | settings JSON golden + UI contract | open/edit/restart each page | planned |
| SET-02 | 700 ms autosave and immediate apply | `SettingsViewModel.cs:629-651` | presentation autosave test | change setting, close, reopen | planned |
| REC-01 | Push-to-Talk, Toggle and VAD modes | `RecordingStateMachine.cs:27-37` | `recording_state_machine` tests | each mode with real/recorded audio | planned |
| REC-02 | Full final transcription, no streaming preview regression | `RecordingStateMachine.cs:35-37` | differential transcript tests | record → final clipboard text | planned |
| REC-03 | Cancel during recording/processing is idempotent and loses no unrelated state | `RecordingStateMachine.cs:135-143` | cancellation/TSan tests | cancel during recording and inference | planned |
| AUD-01 | Selected/default microphone, hotplug and fallback diagnostics | `MicrophoneService.cs:15-29`, `AudioRecorder.cs:232-311` | `microphone` contract | Intel Smart Sound + ordinary mic | planned |
| AUD-02 | Native WASAPI path and fallback chain | `AudioRecorder.cs:92-119,194-339` | `wasapi` component tests | record from each available backend | planned |
| AUD-03 | 16 kHz mono PCM16 WAV, resampling, downmix and no lost tail | `WavBuilder.cs:15-56` | `audio_dsp` differential tests | known WAV fixture end-to-end | planned |
| AUD-04 | Noise reduction and silence trimming preserve quiet speech | `NoiseSuppressor.cs`, `SilenceTrimmer.cs` | `audio_dsp` tests | quiet/long-pause recordings | planned |
| VAD-01 | Silero threshold/auto-stop/no-speech behavior | `SileroSpeechSegmenter.cs`, `SilenceAutoStopDetector.cs` | VAD fixtures/TSan | VAD mode real microphone | planned |
| ASR-01 | Whisper model lifecycle, language, prompt, temperature, bestOf, warmup | `WhisperEngine.cs`, `App.axaml.cs:243-307` | engine contract + CPU parity | model switch/warmup/first dictation | planned |
| ASR-02 | Parakeet native ABI, GGUF catalog, explicit unavailable/error state | `ParakeetEngine.cs`, `ParakeetNative.cs` | ABI/model contract | Parakeet first dictation/error path | planned |
| ASR-03 | Model download/progress/cancel/delete and no corrupt partial install | `ModelManager.cs:43-298` | model-store fault tests | download, cancel, retry, delete | planned |
| OUT-01 | Clipboard first, 80 ms delay, optional auto-paste, UAC diagnostics | `TextOutputService.cs:15-45` | output contract | clipboard-only and paste into editor | planned |
| KEY-01 | Global hotkeys, Win capture, conflict handling and release semantics | `HotkeyService.cs`, `HotkeyCaptureHook.cs` | hotkey contract | configure/use Alt+Win+Space and cancel | planned |
| PAD-01 | XInput/DirectInput record/cancel/release and device loss recovery | `GamepadInputService.cs` | gamepad parser/matcher + component tests | physical controller record/cancel | planned |
| LIFE-01 | Autostart, start-minimized, tray actions, overlay and theme | `StartupManager.cs`, `TrayIcon.cs`, overlay | lifecycle contract + `status_overlay` UI test (`ui-status-overlay-test`) | toggle autostart, restart app, tray actions, overlay during dictation | planned |
| UPD-01 | Update check, SHA, cancel, handoff and rollback | `UpdateService.cs`, `UpdateLauncher.cs` | update contract | update failure/cancel/rollback scenario | planned |
| LOG-01 | Paths, rotation, redacted diagnostics, no audio/text leakage | `FileLogger.cs`, Phase 0 contracts | logger contract | inspect log after launch/failure | planned |
| PRIV-01 | Offline CPU recognition, no telemetry/network audio path | `WhisperEngine`, `ParakeetEngine` | static/network audit | disconnect network, dictation remains local | planned |

## Contract decisions D1–D9

| Decision | Agreed C++ contract | Required evidence |
|---|---|---|
| D1 Parakeet cancellation | `interruptible=false`; measure and document maximum cancellation latency; never kill native context | engine contract + latency test |
| D2 VAD resampling | continuous session resampler; no independent phase reset per drained chunk | boundary/tail golden + differential test |
| D3 VAD segment time | port current absolute/relative normalization heuristic unchanged | VAD golden scenarios |
| D4 Selected microphone | native and fallback backends receive `microphoneDeviceId` | device selection/hotplug test |
| D5 Update SHA | missing SHA blocks auto-install; update may still be shown and manually opened | update contract + launch failure scenario |
| D6 Asset architecture | prefer x64/arch-matched asset; fallback first match with explicit warning | update-release fixture |
| D7 Recording buffer | 512 MiB bound; overflow stops capture with `kResourceBusy` and diagnostic | bounded-buffer/fault test |
| D8 Numeric enums | accept numeric enum values for compatibility, normalize, and log warning | settings JSON fixture |
| D9 Startup log | clear current `voiceTyper.log` at startup like C#; keep archives `1..5` | logger rotation test |

Phase B storage read/save decisions (null strings, duplicate keys, save without
the UI clamp, atomic replace) are recorded in
`docs/migration/cpp/compatibility-contracts.md` §1 "C++ Phase B read/save decisions"
and are covered by the `settings-json-contract` test.

## Intent-parity decisions

- C# no-op/partial gaps (`HideOnFocusLoss`, tray theme/recording glyph, `LastText`, selected microphone restore, `conditionOnPreviousText`) are implemented according to the stated user intent, not copied as silent no-ops.
- Every such implementation has a named contract test and a Windows launch scenario before its row can be marked `done`.
- Any intentional deviation from the C# contract requires a separate user decision and a recorded regression test.

## Update rule

Update this ledger together with the Phase 2–7 plan status. A missing evidence cell is a blocker, not an implicit pass.

## Phase C native dependency evidence (t_c6a6b0ce9685)

Recorded here so the ASR rows are not read as "done": the dependency boundary
exists and is verified, the engines themselves are not yet implemented, and no
model has been transcribed.

| Row | What exists now | What is still missing for `done` |
|---|---|---|
| ASR-01 | `src/asr/whisper_native.*` (context load/free, full-buffer transcribe, language/prompt/temperature/bestOf/threads, abort-callback cancellation) against pinned whisper.cpp `d09f61a7`, static; `whisper-native-contract` proves compile+link+pin without a model | `Transcriber` implementation, model load/warmup, real dictation, CPU parity, Windows launch scenario |
| ASR-02 | `src/platform/windows/parakeet_runtime.*` (LoadLibraryW/GetProcAddress, six symbols, ABI 6 asserted, explicit unavailable reasons); `native-dependency-contract` observes **ABI 6 with all six symbols bound** on Windows and `platform_unsupported` off Windows | `Transcriber` implementation, GGUF load, first Parakeet dictation, error-path scenario |
| PRIV-01 | Manifest states no network/telemetry/downloads at recognition; both engines are local CPU inference and the contract test checks it | disconnect-network dictation scenario |

Status of these rows stays `planned`. Evidence: Arch Release GUI-off
`VOICETYPER_BUILD_ASR=ON` configure/build/ctest = 0/0/0 with 19/19, Arch
GUI-off offline = 0/0 with `native-dependency-contract` 1/1, Windows MinGW13 +
Qt 6.11.2 = 0/0/0 with 19/19. Details in
`docs/migration/cpp/native-dependencies.md`.

## Phase D platform evidence (task t_c0e3b56d32d2)

LIFE-01 and UPD-01 stay `planned` until the Windows launch/physical scenarios
run. What exists and is machine-verified as of 2026-10-01:

| Part | Implementation | Test | Status |
|---|---|---|---|
| Win32 clock, clipboard, paste, executor, file system, logger | `src/platform/windows/win32_{clock,clipboard,paste,executor,file_system,logger}.*` | `win32-platform-contract` (189 checks) | green |
| Hotkeys + gamepad (XInput) | `src/platform/windows/win32_{hotkeys,gamepad}.*` | `win32-input-contract` (166 checks) | green; DInput still out of scope (PAD-01) |
| WASAPI capture, device enumeration and selection | `src/platform/windows/{wasapi_capture,windows_audio_capture,windows_microphone}.*` | `windows-audio-capture-contract` | **skipped**: needs an interactive desktop session (COM), never reported as passed |
| Autostart Run key | `src/platform/windows/win32_startup.*` | `win32-startup-contract` (real HKCU Run key, per-process value name) | green |
| Tray, single instance, ordered teardown | `src/app/tray_controller.*`, `src/app/windows_application.cpp` | Qt UI test (close-to-tray) | green |
| Frameless status overlay | `src/app/status_overlay.*` (contract in `src/platform/api/status_overlay.hpp`) | `ui-status-overlay-test` (11 cases, offscreen) | green; activation/taskbar behaviour on a real desktop waits for the physical smoke |

Evidence (2026-10-01): Windows MinGW13 + Qt 6.11.2 configure/build/ctest =
0/0/0 with **29/29** (2 skipped: no model in the environment, no capture
endpoint); Arch GUI-off (ASR ON, GUI OFF) build/ctest = 0/0 with 25/25; Arch
GUI-on Release builds and runs the overlay UI test offscreen = 11/11. Nothing
in this table marks a ledger row `done`.

## GigaAM-v3: C++-only extension beyond the .NET reference

The native build adds a third speech-to-text engine the .NET reference does not
have, so this row has no `C# evidence` column by construction (decision by
Alexander, 2026-10-06: "третий движок рядом с Whisper и Parakeet"). It is recorded
here so the intent-parity rule - no feature is claimed without a contract test and
a Windows scenario - is satisfied by evidence rather than by prose.

| Row | What | C++ contract evidence | Windows evidence | Status |
| --- | --- | --- | --- | --- |
| ASR-04 | GigaAM-v3-e2e-rnnt (MIT weights) through the pinned transcribe.cpp runtime (commit `3f32fbcc7bb3246851a0234263438bc3c0fa1cac`, v0.3.1) | `speech-segments-contract` (trim margins/pause compression, chunk planning at pauses, forced-cut determinism, transcript joining), `gigaam-engine-contract` (window read from the engine, chunking, refusal without a segmenter, no lazy load, deep warm-up, capabilities all false, no fallback), `settings-json-contract` (22 properties; the five .NET fixtures still parse and serialize with exactly one added line), `model-store-contract` (four quants with exact bytes) | `voicetyper-asr-native-smoke` with `VOICETYPER_ENGINE=gigaam`: 4.5 s reference clip -> `Важно различать глаголы и дополнения.` (exact match, punctuation and casing included); 10.98 s FLEURS ru clip -> full transcript, load 136 ms, deep warm-up 38 ms, transcribe 1565 ms; 33.84 s clip (over the model window) -> cut at a pause and transcribed in 4869 ms | done |

Known compatibility consequence, accepted explicitly: `transcriptionEngine: "gigaam"`
is a wire value the legacy .NET serializer cannot resolve, and its settings loader
falls back to a whole-document default when that happens. A .NET rollback with a
GigaAM setting therefore resets settings.json; a backup copy is taken before the
engine is switched. The new `gigaamModelSize` property is safe in the other
direction: unknown properties are ignored by the .NET reader.

Deployment note: the runtime must be built with the SAME MinGW generation as the
application. A GCC 16 build of the same source dies in-process with 0xC0000139 or
error 127 (module-name reuse by the loader); see `native/transcribe/BUILD.txt`.

## Terms dictionary: prompt, exact pairs and a similarity rule

The .NET reference stored `termsDictionary` and passed the whole string to its
Whisper engine as an initial prompt (`WithPrompt(...).WithCarryInitialPrompt(true)`).
A prompt is only a hint: measured 2026-10-07 on the target machine with a
synthesised Russian clip (Microsoft Irina, "Добавь коммит в ветку и отправь на
ревью"), GigaAM answered `Добавь комит в ветку и отправь на ревью.` and did not
change it with a context prompt, while the library itself reported
`model 'gigaam' does not support vocabulary` for a vocabulary attempt. The Parakeet
C API has neither a prompt nor a vocabulary. So "the dictionary must work on every
engine" (Alexander, 2026-10-07) cannot be satisfied by an engine-side mechanism,
and the C++ build deliberately goes beyond the reference:

| Row | What | C++ contract evidence | Windows evidence | Status |
| --- | --- | --- | --- | --- |
| ASR-05 | Terms dictionary read as a prompt for engines that declare one, plus explicit `as heard=as written` pairs and a similarity rule applied by `domain::TermsDictionaryPort` to the finished text of EVERY engine | `terms-dictionary-contract`: parsing (pairs, duplicates, first `=`, empty sides), prompt building and its byte budget, exact replacement with whole-word boundaries and the capital rule, similarity matrix (`комит/камит/коммит/comit/comite` corrected; `комик/камин/комитет/команда` untouched), ambiguity skip, short-term guard, idempotency, and the port decorator applying the same rule to any engine | `voicetyper-asr-native-smoke` with `VOICETYPER_ENGINE=gigaam`: transcript `Добавь комит в ветку и отправь на ревью.` -> `final_text` `Добавь commit в ветку и отправь на ревью.` (exit 0), i.e. on an engine that has no native dictionary at all | done |

The similarity rule in one line: both words are transliterated to Latin, their
consonant skeletons must be equal, and their full forms may differ by at most one
edit for a six-letter word (two from eight letters). A skeleton shorter than three
consonants is never matched that way - `API`, `CPU` and `IDE` therefore still need
an explicit pair - and a word that matches two different entries is left alone
instead of guessed. Residual risk is stated rather than hidden: a rare word that
shares both the skeleton and the form could still be rewritten.

Measured effect of the prompt itself (same clip, Whisper small q8, 2026-10-07):

| Engine prompt | Transcript of "Добавь коммит в ветку и отправь на ревью" |
| --- | --- |
| none | `Добавь коммит в ветку и отправь на ревью.` |
| legacy comma list with `commit` | `Добавь комит в ветку и отправь на ревью.` (Cyrillic, and the Russian word got worse) |
| built sentence `Термины пишутся латиницей: ... commit.` | `Добавь коммит в ветку и отправь на ревью.` (correct again, still Cyrillic) |

So a prompt does **not** deliver the Latin spelling on this material: neither form
made Whisper write `commit`. The sentence form at least stops degrading the Russian
word that the legacy comma list damaged. This is why the prompt is kept as a hint
that costs nothing, while the promise of the feature rests on the replacement
(`TermsDictionaryPort`), which was verified to produce `commit` on both Whisper and
GigaAM for the same clip.
