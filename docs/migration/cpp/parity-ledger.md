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
