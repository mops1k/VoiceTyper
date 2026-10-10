# VoiceTyper C++ migration — feature parity matrix

**Baseline:** `main` @ `fc7d69c1a32be71a19634c16904d0a29a24135b0` (`v1.1.3`)  
**Reference branch:** local-only `feature/multiplatform-core-split` @ `514fe60835bb95933ffc49b27d9ba46fe5602405`

## Priority rules

- **Blocking:** current user-visible behavior or data-safety contract; C++ Windows stable cutover is blocked until implemented and verified.
- **Non-blocking parity:** visual/polish or implementation detail where the workflow and data contract remain equivalent.
- **Future:** not implemented on current `main`; do not silently add it to migration scope.

A C++ implementation may intentionally fix a current defect, but the decision and a regression test must be recorded before calling the result parity.

## Current source feature matrix

| Area | Current behavior | Evidence | Existing tests | Priority / acceptance |
|---|---|---|---|---|
| Application shell | Avalonia desktop app, custom titlebar, tray-resident lifecycle, explicit shutdown | `VoiceTyper.App/App.axaml.cs:71-91`, `App.axaml`, `MainWindow.axaml` | none for shell | **Blocking:** C++/Qt window opens, closes to tray, starts minimized and exits without orphan processes |
| Single instance | Global named mutex `Global\\VoiceTyper_SingleInstance`; second instance shows dialog and exits; no IPC | `App.axaml.cs:27,112-117,214-230` | none | **Blocking:** cross-version lock and no settings corruption |
| Settings navigation | Main, Appearance, Models, Hotkeys, Microphone, Startup, About, Log sections | `MainWindow.axaml:150-787`, `SettingsViewModel.cs:307-349` | UI not covered | **Blocking:** all sections reachable and controls usable |
| Settings persistence | 700 ms debounce, atomic temp+rename, defaults on missing/corrupt/IO/unauthorized load | `SettingsViewModel.cs:629-651`, `SettingsService.cs:20-77` | `SettingsServiceTests` | **Blocking:** exact JSON/defaults and failure recovery |
| Recording modes | Push-to-Talk, Toggle, VAD; full final transcription; no streaming preview | `RecordingStateMachine.cs:27-37,108-239` | `RecordingStateMachineTests` | **Blocking:** state transitions, cancel, empty capture, VAD no-speech/trailing silence |
| Recording start gate | Capture disabled until selected model file exists and engine is initialized | `App.axaml.cs:522-535` | indirect only | **Blocking:** clear status, no freeze/deadlock |
| Windows microphone | Active endpoint enumeration; selected ID passed to Raw/NAudio but not Native/MME | `MicrophoneService.cs:15-29`, `AudioRecorder.cs:232-311` | none | **Blocking:** selected device, default device, disconnect and fallback diagnostics |
| Windows audio backends | Native `mc_wasapi.dll` → Raw WASAPI → NAudio WASAPI → MME; native path asks 48 kHz stereo | `AudioRecorder.cs:92-119,194-339` | none | **Blocking:** ordinary mic and Intel Smart Sound physical validation; current Raw path has no verified `IAudioClient.Start()` |
| Audio conversion | 16-bit PCM → 16 kHz mono float/PCM16 WAV via NAudio `WdlResamplingSampleProvider`; stereo downmix | `WavBuilder.cs:15-56` | `WavBuilderTests` | **Blocking:** format, duration, resampling and tail parity |
| Noise reduction | Optional high-pass + adaptive noise-floor damping | `NoiseSuppressor.cs:5-57` | no dedicated test | **Blocking:** option applies deterministically without clipping; quality threshold to define |
| Silence trimming | Adaptive threshold, leading/trailing trim, internal pause compression | `SilenceTrimmer.cs:18-163` | `SilenceTrimmerTests` | **Blocking:** preserve quiet speech and long-pause behavior; port feature-branch partial-frame fix |
| VAD | Silero/whisper-vad threshold 0.5, min speech 250 ms, no-speech max 5 s, energy fallback | `SileroSpeechSegmenter.cs:23-50`, `SilenceAutoStopDetector.cs:30-155` | `SilenceAutoStopDetectorTests` | **Blocking:** no mid-dictation cut; deterministic stop reasons |
| Whisper | q8 models, language Auto/RU/EN, prompt, temperature, context, greedy/bestOf, CPU threads, load + deep warmup | `WhisperEngine.cs:12-152`, `App.axaml.cs:243-307` | `EngineManagerTests`, model tests; no real inference fixture | **Blocking:** same model path and acceptable transcript/latency |
| Parakeet | Native C API ABI v6, q4_k/q5_k/q6_k/q8_0 GGUF, language auto, explicit error if native library missing | `ParakeetEngine.cs:19-146`, `ParakeetNative.cs`, `Native/parakeet/BUILD.txt` | `ParakeetNativeTests` are presence/ABI/null-load only | **Blocking:** ABI check, load/free, real model smoke, no silent Whisper fallback |
| Model manager | Download from HuggingFace to `.download`, atomic move, progress/cancel, delete, legacy q5/fp16 cleanup | `ModelManager.cs:43-298` | `ModelManagerTests` | **Blocking:** filenames/URLs/sizes and no corrupt partial install; add checksum/resume only as explicit improvement |
| ASR model lifecycle | One loaded engine, serialized inference, reload on engine/size change, background warmup | `App.axaml.cs:233-405`, engine classes | fakes in state tests | **Blocking:** cancellation/switch/dispose must not race or leak |
| Clipboard / paste | Clipboard first; 80 ms delay; optional Ctrl+V; Windows Avalonia writer retries 5×120 ms | `TextOutputService.cs:15-45`, `AvaloniaClipboardWriter.cs:9-35` | `TextOutputServiceTests` | **Blocking:** clipboard remains source of truth; UAC and busy clipboard diagnostics |
| Global hotkeys | Win32 `RegisterHotKey`, dedicated message-pump thread, conflict errors, push-to-talk release polling | `HotkeyService.cs:14-210,363-398` | key mapping tests only | **Blocking:** registration, release, collision and unregister behavior |
| Hotkey capture | `WH_KEYBOARD_LL`, suppresses Win, Escape cancels, modifier validation | `HotkeyCaptureHook.cs:17-292` | none for hook | **Blocking:** capture works with Win/combinations and cannot leak hook |
| Gamepads | XInput 0–3 plus DirectInput polling at 33 ms; XInput and DInput binding grammar; capture/release | `GamepadInputService.cs:15-389`, `GamepadBindingParser.cs` | parser/matcher tests; no hardware | **Blocking:** real controller record/cancel/release and device loss recovery |
| Clipboard UI | Status text, last-text field in ViewModel; `LastText` has no located XAML binding | `SettingsViewModel.cs:82,110-111`, `MainWindow.axaml` | none | **Non-blocking visual parity**, unless user confirms last-text is required UX |
| Tray | Open settings, record/cancel action, quit, readiness tooltip, toast windows | `Tray/TrayIcon.cs`, `Tray/ToastWindow.cs` | none | **Blocking:** no background dead app; exact tray action behavior |
| Tray theme/recording glyph | `ApplyTheme` and `SetRecording` currently no-op | `TrayIcon.cs:89-106` | none | **Non-blocking parity / explicit product decision**: implement or remove from C++ scope |
| Hide on focus loss | Setting is bound and persisted but no hide consumer found | `AppSettings.cs:125`, `SettingsViewModel.cs:136,1183`, `MainWindow.axaml.cs:137-145` | none | **Blocking decision:** implement promised behavior or get explicit deprecation approval |
| Status overlay | Frameless topmost bottom-center pill, pulse 350 ms, recording/processing colors | `Overlay/StatusOverlayWindow.*`, `App.axaml.cs:619-653` | none | **Blocking:** visible state, no focus/taskbar presence, DPI/multi-monitor |
| Theme | System/Light/Dark, Windows registry probe, palette and titlebar | `ThemeManager.cs:15-94` | none | **Blocking:** system theme and manual themes; Linux probe is separate |
| UI language | RU/EN resources, live switch, OS language on first run only | `Loc.cs:7-55`, `Strings.resx`, `Strings.en.resx`, `App.axaml.cs:203-212` | `LocTests` | **Blocking:** both languages complete, no missing-key UI |
| Startup | HKCU `Run` value `VoiceTyper`, quoted process path, errors swallowed | `StartupManager.cs:9-51` | none | **Blocking:** toggle survives restart; errors are visible diagnostically |
| Autostart / startup state | Start-with-system and start-minimized settings applied live | `SettingsViewModel.cs:1261-1263`, `App.axaml.cs:470-510` | none | **Blocking:** immediate apply and restart behavior |
| Updates | GitHub latest API, setup asset regex, optional SHA-256, cancel, download, Inno handoff | `UpdateService.cs:31-316`, `UpdateLauncher.cs:13-50` | `UpdateServiceTests`; no launcher test | **Blocking:** SHA mismatch cleanup, update handoff and rollback |
| Installer | Per-user Inno Setup, `AppMutex`, upgrade/uninstall, release tag version | `installer/installer.iss:17-69`, `.github/workflows/release.yml` | none | **Blocking:** clean install/upgrade/uninstall and .NET/C++ overlap |
| Logging | `%LOCALAPPDATA%\\VoiceTyper\\logs\\voiceTyper.log`, 1 MB rotation, 5 archives, clear at startup, no logging failure propagation | `FileLogger.cs:5-100` | none | **Blocking:** readable diagnostics; preserving logs across crash is a stability improvement to approve |
| Privacy | Offline CPU recognition; no telemetry in current source | README, `WhisperEngine`, `ParakeetEngine` | none | **Blocking:** C++ must not add network audio/text/telemetry paths |
| UI styling | Avalonia Fluent theme, Inter, custom palette, 980×640 default, min 760×480 | `Program.cs`, `App.axaml`, `ThemeManager`, `MainWindow.axaml` | none | **Non-blocking parity:** Qt layout must fit same workflows and DPI without clipping |

## Defects and explicit decisions before cutover

These are observed current-source gaps, not requirements to reproduce:

1. `MicrophoneDeviceId` is not loaded by `SettingsViewModel.LoadFromSettings`; C++ should preserve the user's selected device and add a regression test.
2. `RawWasapiCapture` has no visible `IAudioClient.Start()` call; C++ must either implement a correct native WASAPI backend or remove the fallback from the advertised order after hardware verification.
3. `HideOnFocusLoss`, tray theme/recording state and `LastText` display are incomplete. Each needs an explicit product decision in the UI parity subplan.
4. `SilenceTrimmer` partial-frame fix is present in the local multiplatform branch (`cbfdaf6`) but not in current `main`; port the fix and regression test rather than reproducing the main-branch edge case.
5. Current tests are predominantly portable unit tests. They do not close hardware, UI, installer, or Linux gates.

## Local multiplatform reference

The local-only branch `feature/multiplatform-core-split` contains six commits after `v1.1.1`:

- `5a5cc9d` — platform-neutral Core / Core.Windows split.
- `54f0c50` — Core.Linux audio, evdev, clipboard, gamepad and `libparakeet.so`.
- `cb8d541` — multi-target App and Linux packaging.
- `cbfdaf6` — Linux fonts/icons and `SilenceTrimmer` edge fix.
- `4cf8002` — gdbus KGlobalAccel fallback instead of fragile D-Bus task behavior.
- `514fe60` — inaccessible evdev handling and udev rule.

The branch has no `origin/feature/multiplatform-core-split` ref. It is a behavioral/design reference, not a directly portable C++ codebase. It provides reusable Linux fallback semantics, synthetic fixtures, Parakeet ABI/header information and packaging lessons. It does **not** provide `mc_wasapi.cpp`.

## Current gaps that are not parity blockers yet

- `continuous-mode-plan.md` describes a fourth Continuous mode, but it is not implemented on current `main`; keep it as a future feature.
- Pixel-perfect Avalonia styling, toast animation and icon theme are non-blocking if workflows, accessibility and layout remain equivalent.
- Linux self-update is implemented differently from Windows, not skipped: the AppImage the process
  was started from is replaced in place (download → sha256 → rename → restart, VT-SYS-014), and a
  build that is not an AppImage (source tree, distribution package) opens the release page instead.

## Parity gate

C++ Windows parity is complete only when every Blocking row has an implemented test or an explicitly approved exception, the .NET reference remains runnable, and a side-by-side Windows 10/11 smoke covers microphone, hotkeys, VAD, both ASR engines, model lifecycle, clipboard/paste, tray/overlay, settings persistence, update and clean install/upgrade.
