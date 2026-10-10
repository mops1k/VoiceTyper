# VoiceTyper — System Specification (index)

**Status:** normative specification of the code in this repository.
**Baseline:** `main` @ `3cc4c5a9a602afb96c2f478a562005f5eb850068` (tag `v2.2.1`, 2026-10-08).
**Linux extension:** the requirements marked `VT-*-11xx`…`VT-*-16xx` (and the
`Linux` rows of the port tables) describe the multiplatform work of 2026-10-09/10 in
the working tree: `src/platform/linux/`, `src/app/linux_application.cpp`, the Linux
branches of `parakeet_runtime.cpp` / `transcribe_runtime.cpp` and the Linux CMake
targets. They are written against the code as it stands in that tree, not against the
`3cc4c5a` baseline, and are marked as such wherever they change a Windows-only rule.
**Product version constant:** `VOICETYPER_VERSION` (currently `3.0.0`, [CMakeLists.txt:22](../CMakeLists.txt#L22)).
**Requirement keywords:** RFC 2119 (`MUST`, `MUST NOT`, `SHALL`, `SHALL NOT`, `SHOULD`, `SHOULD NOT`, `MAY`).

This file is the entry point. Module-level requirements live in
[`specs/modules/`](modules/):

| Module spec | Covers | Source tree |
|---|---|---|
| [domain.spec.md](modules/domain.spec.md) | settings schema, recording state machine, VAD/segmentation, terms dictionary, text output, errors | `src/domain/` |
| [platform.spec.md](modules/platform.spec.md) | frozen backend interfaces (ports) and their Win32/portable implementations | `src/platform/` |
| [asr.spec.md](modules/asr.spec.md) | recognition engines, engine host lifecycle, Silero VAD, native libraries | `src/asr/`, `native/` |
| [core-support.spec.md](modules/core-support.spec.md) | paths, logger, CPU topology, update manifest/service/launcher, model download, SHA-256 | `src/core/support/`, `src/domain/app_paths.hpp`, `src/domain/file_logger.hpp` |
| [ui.spec.md](modules/ui.spec.md) | settings window (8 pages), tray, status overlay, localization, themes | `src/app/` |
| [build-release.spec.md](modules/build-release.spec.md) | CMake targets, presets, pinned dependencies, CI, installer, test suite, tools | `CMakeLists.txt`, `cmake/`, `installer/`, `tests/`, `tools/`, `.github/` |

---

## 1. Purpose and scope

### 1.1 Purpose

VoiceTyper is a **Windows desktop dictation utility**: the user presses a global
hotkey, speaks, and the recognized text is placed on the clipboard and pasted
into the field that had focus. Recognition runs **entirely on the local CPU**
([README.md:17](../README.md#L17)); no audio and no recognized text is sent anywhere.

Business intent, as stated by the project owner across the history that produced v2.x
(PR #2, PR #3, and the C++ rewrite commit `61cf4b0`):

- **VT-SYS-001 — Offline operation.** The system MUST perform speech recognition locally on the CPU and MUST NOT transmit audio or transcript to any network endpoint. The only outbound requests allowed are (a) model file downloads from the configured Hugging Face URLs and (b) the GitHub release check for self-update.
- **VT-SYS-002 — No GPU requirement.** The system MUST run on an x64 CPU without a GPU; native engines MUST be pinned to the CPU backend.
- **VT-SYS-003 — Hotkey-driven dictation anywhere.** The system MUST accept a global hotkey that works while another application is focused.
- **VT-SYS-004 — Clipboard is authoritative.** The recognized text MUST always reach the system clipboard when it is non-empty; automatic pasting is best-effort and MUST NOT be the only delivery path.
- **VT-SYS-005 — Tray-resident.** Closing the settings window SHOULD hide it and keep the process alive in the notification area.
- **VT-SYS-006 — Russian first, English supported.** The interface and recognition MUST support Russian and English; the installer language MAY remain Russian-only (`installer/installer-native.iss`).
- **VT-SYS-007 — Self-update.** The application MUST be able to check for a newer release, download the installer, verify its SHA-256 when the release body supplies one, and hand over to it.
- **VT-SYS-008 — Intent parity over bug parity.** Where the legacy .NET implementation had a setting with no effect or a known gap, the replacement MUST implement the declared intent rather than reproduce the bug, and MUST document the deviation (decision recorded 2026-09-25, `docs/migration/cpp/feature-parity.md`, "Defects and explicit decisions before cutover").

### 1.2 In scope

- The native Windows application (`voicetyper-qt-shell`), its engines, its
  platform backends, its installer and its update path.
- The portable domain logic and the contract test suite that pin the behavior.
- The build, the pinned native dependencies and the release pipeline.

### 1.3 Out of scope

- **VT-SYS-010 — Two shipped platforms: Windows and Linux.** The Windows build is
  published as `VoiceTyper-<version>-win64-Setup.exe`; the Linux build targets Arch
  Linux with KDE Plasma 6 (X11 and Wayland) and is produced from the same sources
  (`src/platform/linux/`, [platform.spec.md](modules/platform.spec.md) §VT-PLT-11xx…16xx).
  Both platforms MUST run the same composition (hotkeys → capture → engine → clipboard
  → paste), and neither MAY be a reduced "contract-test only" build. macOS is out of
  scope: there is no macOS backend, and a configure for any other platform still
  produces the portable contract build only.
- **VT-SYS-011 — No telemetry.** The system MUST NOT add analytics, crash
  reporting or usage reporting. The updater MAY report nothing beyond what an
  HTTP GET to `api.github.com` inherently carries.
- **VT-SYS-012 — No streaming preview.** Recognition is final-only: the system
  MUST NOT show partial transcripts while the user is speaking.
- **VT-SYS-013 — Continuous dictation mode is not implemented.** It is described
  only in [`continuous-mode-plan.md`](../continuous-mode-plan.md) and is a future feature.
- **VT-SYS-014 — Linux self-update is out of scope.** A Linux package is updated by
  the system package manager, not by this updater. The update check MUST still work
  (VT-SYS-080…083), and the install action MUST open the release page instead of
  downloading an installer the platform cannot run
  ([src/app/linux_application.cpp](../src/app/linux_application.cpp), `update_install`).

### 1.4 Actors

| Actor | Interaction |
|---|---|
| End user | Dictates via hotkey or gamepad; edits settings; reads the log; triggers update check. |
| Active foreground window | Receives the pasted text through the synthetic `Ctrl+V`. |
| Hugging Face (`huggingface.co`) | Source of model files; HTTPS GET only. |
| GitHub Releases API (`api.github.com`) | Source of the latest-release manifest and the installer asset. |
| Windows | Provides WASAPI capture, global hotkeys, clipboard, registry autostart, DPI and theming. |

### 1.5 Supported environment

- **VT-SYS-020 — OS.** Windows 10 or Windows 11, 64-bit ([README.md:69-74](../README.md#L69-L74)).
- **VT-SYS-021 — Runtime.** Qt 6, minimum 6.9 (`find_package(Qt6 6.9 REQUIRED COMPONENTS Core Gui Widgets Network)`, [CMakeLists.txt:46](../CMakeLists.txt#L46)); the local reference build is 6.11.2 with MinGW-w64 13.1, CI uses the newest published 6.9.3.
- **VT-SYS-022 — C++.** The sources MUST be C++20 (`CMAKE_CXX_STANDARD 20`, [CMakeLists.txt:5](../CMakeLists.txt#L5)), extensions off.
- **VT-SYS-023 — Build tools.** CMake 3.28+ and Ninja ([CMakeLists.txt:1](../CMakeLists.txt#L1)); Python 3 is optional and only enables fixture/differential tests ([CMakeLists.txt:53-58](../CMakeLists.txt#L53-L58)).
- **VT-SYS-024 — Disk.** Model files range from 42 MB (Whisper Tiny) to about 941 MB (Parakeet q8_0) plus the VAD model ([README.md:53-56](../README.md#L53-L56), [platform.spec.md](modules/platform.spec.md) §model catalog).
- **VT-SYS-025 — Linux.** The Linux build targets Arch Linux with KDE Plasma 6 (X11 and
  Wayland), GCC and Qt 6.9+ Widgets, and additionally needs `libpulse`/`libpulse-simple`
  (capture and the level meter), readable `/dev/input/event*` nodes (global hotkeys and gamepad
  buttons; the `input` group or an ACL), `org.kde.kglobalaccel` on the session bus (the hotkey
  fallback when `/dev/input` is not readable, Qt 6 D-Bus module) and `ydotool` with its daemon
  socket (automatic pasting). Missing input devices or a missing sound server MUST degrade per
  port (VT-PLT-12xx/13xx), never abort the start.

---

## 2. Architecture

### 2.1 Layering

The system is a set of static libraries with a single executable composition root.
Dependencies point inward; the portable layer MUST NOT include Qt or an OS SDK.

```
                       +---------------------------------------------+
                       | src/app   (Qt 6 Widgets UI, composition)     |
                       | main_window, tray, overlay, windows_app     |
                       +---------------+-----------------------------+
                                       |
        +------------------------------+---------------------------+
        |                              |                           |
+-------v--------+          +----------v---------+      +----------v---------+
| src/domain     |          | src/asr            |      | src/core/support   |
| settings, FSM, |          | EngineHost,        |      | update, download,  |
| VAD, terms,    |          | registries,        |      | sha256, paths,     |
| audio, text    |          | silero segmenter   |      | logger, cpu        |
+-------+--------+          +----------+---------+      +--------------------+
        |                              |
        |                   +----------v---------+
        |                   | src/platform       |
        +------------------>| api/*.hpp (ports)  |
                            | windows/, linux/,  |
                            | portable/           |
                            +--------------------+
```

- **VT-SYS-030 — Ports are the only crossing point.** Code above `src/platform/`
  MUST depend on `src/platform/api/*.hpp` (abstract interfaces) and MUST NOT call
  Win32 APIs directly. Every backend implements a port behind its own CMake target
  ([CMakeLists.txt:79-85](../CMakeLists.txt#L79-L85) declares `voicetyper_platform` as header-only and deliberately links nothing).
- **VT-SYS-031 — Portable layer stays portable.** `src/domain/`, `src/platform/api/`
  and `src/platform/portable/` MUST compile without Qt and without `<windows.h>`; this is
  enforced at configure time by `voicetyper_check_portable_headers`
  ([CMakeLists.txt:44-45](../CMakeLists.txt#L44-L45), `cmake/PortableHeaders.cmake`) and re-checked by the `portable-headers` CTest.
- **VT-SYS-031a — Linux backends follow the same layering.** `src/platform/linux/` MUST be
  split into a Qt-free target (`voicetyper_platform_linux`), a Qt-dependent part
  (`voicetyper_platform_linux_gui`, the `QClipboard` marshalling) and a libpulse part
  (`voicetyper_platform_linux_audio`), and the composition (`src/app/linux_application.cpp`)
  MUST reach the OS only through the ports (VT-PLT-11xx…16xx).
- **VT-SYS-032 — Domain must not include ASR.** `src/domain/` MUST NOT depend on
  `src/asr/`; limits that both layers need are duplicated on purpose
  ([src/domain/settings.hpp:225-236](../src/domain/settings.hpp#L225-L236) explains `kBestOfMin`/`kBestOfMax` duplicating the engine's limits).
- **VT-SYS-033 — UI-thread discipline.** Widgets MUST be touched only from the GUI
  thread; worker callbacks MUST be marshalled with a queued invocation. Violating this
  produced real crashes (`QWidget::repaint: Recursive repaint detected`, commit `eda7312`).

### 2.2 Key runtime components and their responsibilities

**Placeholder directories.** `src/audio/` and `src/ui/` contain only a `.gitkeep`
(`"Audio subsystem placeholder for a later migration phase."` and
`"UI placeholder for a later migration phase."`). They hold no code: audio DSP lives in
`src/domain/audio_wav.*` and all UI code lives in `src/app/`. A specification MUST NOT
attribute behavior to these two directories.

| Component | Responsibility | Requirement block |
|---|---|---|
| `MainWindow` / `WindowServices` | Builds the 8 settings pages, owns the authoritative `AppSettings`, drives autosave, capture and model UI | [ui.spec.md](modules/ui.spec.md) §VT-UI-1xx |
| `windows_application` (composition root) | Assembles real backends on Windows, wires hotkeys → capture → engine → text output, single-instance guard, tray, overlay, update check | [ui.spec.md](modules/ui.spec.md) §VT-UI-4xx, [platform.spec.md](modules/platform.spec.md) |
| `domain::RecordingStateMachine` | Idle→Recording→Processing→Idle control flow, cancellation epochs, final-only transcription | [domain.spec.md](modules/domain.spec.md) §VT-DOM-2xx |
| `asr::EngineHost` | Owns the single loaded engine, serializes inference, background load/warm-up, epoch-based supersession | [asr.spec.md](modules/asr.spec.md) §VT-ASR-2xx |
| Engine decorators (`TermsDictionaryPort`, `SilenceTrimmingPort`) | Post-process the transcript and the audio in front of any engine | [domain.spec.md](modules/domain.spec.md) §VT-DOM-5xx, [asr.spec.md](modules/asr.spec.md) §VT-ASR-5xx |
| `platform::WasapiCapture` / `WindowsAudioCapture` | Device I/O and session capture behind `RecordingPort` | [platform.spec.md](modules/platform.spec.md) §VT-PLT-3xx |
| `core::support::ModelDownloadService` | HTTPS model download with progress, cancel and atomic `.part` rename | [core-support.spec.md](modules/core-support.spec.md) §VT-COR-3xx |
| `core::support::UpdateService` / `UpdateLauncher` | Release manifest, SHA-256 verification, installer handoff | [core-support.spec.md](modules/core-support.spec.md) §VT-COR-4xx |

### 2.3 Threads

| Thread | Owns | Rules |
|---|---|---|
| GUI thread | All widgets, `MainWindow`, overlay, tray, settings autosave | MUST be the only thread touching widgets (VT-SYS-033) |
| Capture thread | `IAudioClient` and the audio buffer | Only this thread touches the client (commit `wasapi_capture` contract, [platform.spec.md](modules/platform.spec.md) §VT-PLT-3xx) |
| Engine thread (`EngineHost::EngineExecutor`) | Native inference and model load | A dedicated single thread, LIFO-drop by epoch; MUST NOT share the recording worker (a multi-second warm-up would stall the VAD loop, memory `m_307c93343e6d`) |
| Recording worker (`ThreadRecordingWorker`) | Stop-before-process buffer handoff | Publishes through callbacks that are invoked outside the state-machine lock |
| Qt HTTP client worker | Downloads and release checks | Progress MUST be delivered to the GUI thread via queued invocation (commit `eda7312`) |

### 2.4 Single instance

- **VT-SYS-040.** The application MUST run as a single instance per user session. The
  gate MUST be the global named mutex `Global\VoiceTyper_SingleInstance`, the same name
  the installer uses as `AppMutex` (`docs/migration/cpp/compatibility-contracts.md` §2; commit `dfca3a1`).
- **VT-SYS-041.** Before `CreateMutexW` the implementation MUST call
  `::SetLastError(ERROR_SUCCESS)`; on `ERROR_ALREADY_EXISTS` the process MUST exit
  immediately without touching the IPC channel (commit `dfca3a1`).
- **VT-SYS-042.** A second launch MUST notify the running instance to bring its window
  to the front (local socket message `show` over `QLocalServer`/`QLocalSocket`) and then exit.
- **VT-SYS-043.** If the channel cannot be claimed while the mutex is owned, the process
  MUST refuse to start and log a warning rather than run a duplicate (commit `dfca3a1`).
- **VT-SYS-044.** On Linux the single-instance gate MUST be the `QLocalServer` channel
  (`src/app/tray_controller.cpp`): the named mutex of VT-SYS-040 is Win32-only, and a second
  launch MUST hand the `show` request to the owner and exit (VT-PLT-1106).

### 2.5 Lifecycle and startup

- **VT-SYS-050.** The process MUST NOT quit when the last window is closed
  (`application.setQuitOnLastWindowClosed(false)`, [src/app/main.cpp:65](../src/app/main.cpp#L65)) unless it was started by `--selftest`.
- **VT-SYS-051.** When the settings window loses focus, the window MAY hide if and only
  if `hideOnFocusLoss` is enabled.
- **VT-SYS-052.** On launch the process MUST start window-hidden when it was invoked with
  `--start-minimized` or when `startMinimized` is enabled. The window object and the tray
  icon MUST still exist.
- **VT-SYS-053.** A launch MAY rewrite the `HKCU\...\Run\VoiceTyper` value only when the
  entry is absent, unparsable, already owned by this product, or points at a file that no
  longer exists; an entry naming a different, still-present executable MUST be left alone
  and logged (commit `wasapi_capture`/`win32_startup` contract, `docs/migration/cpp/compatibility-contracts.md` §2).
- **VT-SYS-054.** `--selftest` MUST report state (including autostart) and MUST NOT write
  to the registry or modify settings.
- **VT-SYS-053a.** On Linux the same reconciliation rule MUST hold for the freedesktop entry
  `$XDG_CONFIG_HOME/autostart/voicetyper.desktop`: it MAY be rewritten only when it is absent,
  unparsable, already owned by this product, or points at a file that no longer exists
  (VT-PLT-1105).
- **VT-SYS-054a.** On Linux `--selftest` MUST NOT write the autostart entry either; the
  reconciliation runs only on a normal launch.

### 2.6 File locations (Windows)

| Artifact | Location | Requirement |
|---|---|---|
| Settings | `%APPDATA%\VoiceTyper\settings.json` | VT-SYS-060 |
| Models | `%LOCALAPPDATA%\VoiceTyper\models` | VT-SYS-061 |
| Log | `%LOCALAPPDATA%\VoiceTyper\logs\voiceTyper.log` | VT-SYS-062 |
| Update download | `%LOCALAPPDATA%\VoiceTyper\updates\VoiceTyper-<version>-Setup.exe` | VT-SYS-063 |
| Native libraries | beside the executable (`mc_wasapi.dll`, `parakeet.dll`), `transcribe\` subdirectory for `libtranscribe.dll` | VT-SYS-064 |
| Autostart | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value `VoiceTyper`, `REG_SZ` | VT-SYS-065 |

- **VT-SYS-060 … VT-SYS-065.** The paths above MUST be produced by one resolver
  (`domain::AppPaths` from platform roots) and MUST NOT be re-assembled at call sites.
  A wrong place for this decision already caused the app to create a second settings file
  under `%LOCALAPPDATA%` and silently ignore the user's real one (`docs/migration/cpp/compatibility-contracts.md` §2, task `t_19bc5fcbed99`).
- **VT-SYS-066.** `VOICETYPER_SETTINGS_PATH`, `VOICETYPER_LOG_DIR` and
  `VOICETYPER_MODELS_DIR` MUST be honoured as overrides **on top of** the resolver, never
  as a second source of truth.
- **VT-SYS-067.** A missing environment root MUST fall back to the other root and finally
  to the executable directory; an empty variable counts as unset.

### 2.6a File locations (Linux)

| Artifact | Location | Requirement |
|---|---|---|
| Settings | `$XDG_CONFIG_HOME/VoiceTyper/settings.json` (fallback `$HOME/.config/VoiceTyper/settings.json`) | VT-SYS-068 |
| Models | `$XDG_DATA_HOME/VoiceTyper/models` (fallback `$HOME/.local/share/VoiceTyper/models`) | VT-SYS-068 |
| Log | `$XDG_DATA_HOME/VoiceTyper/logs/voiceTyper.log` | VT-SYS-068 |
| Update download | `$XDG_DATA_HOME/VoiceTyper/updates` (the Linux build does not download installers, VT-SYS-014) | VT-SYS-068 |
| Engine libraries | beside the executable, then `<exe_dir>/engine-libs` | VT-PLT-1602 |
| Autostart | `$XDG_CONFIG_HOME/autostart/voicetyper.desktop` (freedesktop desktop entry) | VT-SYS-065a |

- **VT-SYS-068.** The Linux paths MUST come from the same resolver
  (`domain::AppPaths` over `domain::linux_app_path_roots`, VT-COR-105) and MUST NOT be
  re-assembled at call sites; `VOICETYPER_SETTINGS_PATH`, `VOICETYPER_LOG_DIR` and
  `VOICETYPER_MODELS_DIR` MUST keep overriding them (VT-SYS-066).

### 2.7 Logging

- **VT-SYS-070.** The system MUST log to `voiceTyper.log` in the format
  `yyyy-MM-dd HH:mm:ss.fff [LEVEL] message` in local time, with the exception text on the
  following line ([domain.spec.md](modules/domain.spec.md) §VT-DOM-6xx).
- **VT-SYS-071.** Rotation MUST happen before an append that would cross 1,000,000 bytes;
  archives MUST be `voiceTyper.1.log` … `voiceTyper.5.log`.
- **VT-SYS-072.** A logging failure MUST NOT propagate to the caller.
- **VT-SYS-073.** Recognized text MUST NOT be written to the log (`LOG-01` rule, `docs/migration/cpp/compatibility-contracts.md` §7).
- **VT-SYS-074.** Qt's own `qInfo`/`qWarning` output MUST be forwarded into the
  application log so the UI is not a black box (commit `24d3b56`).

### 2.8 Update flow

- **VT-SYS-080.** The update check MUST query
  `https://api.github.com/repos/mops1k/VoiceTyper/releases/latest`.
- **VT-SYS-081.** The installer asset MUST be selected by the pattern
  `^VoiceTyper-\d[^/]*?-Setup\.exe$`, first match in API order.
- **VT-SYS-082.** The version comparison MUST ignore build metadata after `+`, pad
  numeric segments with zero, treat invalid segments as zero, rank a stable release above
  any prerelease, and treat two prereleases with equal numeric cores as equal
  (`docs/migration/cpp/compatibility-contracts.md` §8).
- **VT-SYS-083.** When the release body supplies a SHA-256 (case-insensitive marker
  `SHA256:` or `SHA-256:` followed by 64 hex characters), a mismatch MUST delete the
  downloaded temp file and preserve the previously downloaded installer. A missing SHA
  MUST be accepted.
- **VT-SYS-084.** The launcher MUST start the installer with `/AutoUpdate`, wait for it,
  and relaunch the application regardless of the installer's exit status.
- **VT-SYS-085.** A downloaded asset whose name lacks the `win64` marker MUST NOT be run
  (`src/app/windows_application.cpp` check documented in `.github/workflows/release.yml`).
- **VT-SYS-086.** On Linux the check of VT-SYS-080…083 MUST still run and report a newer
  version, but the install action MUST open the release page in the browser instead of
  downloading and launching an installer (VT-SYS-014,
  [src/app/linux_application.cpp](../src/app/linux_application.cpp) `update_install`).

### 2.9 Privacy

- **VT-SYS-090.** Audio buffers, floating-point samples and recognized text MUST NOT be
  written to disk or sent over the network, except the WAV needed for recognition and the
  clipboard the user asked for.
- **VT-SYS-091.** The application MUST NOT open a listening network port.

---

## 3. Cross-cutting business rules

These rules apply to every module; module files repeat them only by reference.

- **VT-RULE-001 — No silent engine fallback.** If the selected engine or model is
  unavailable, the system MUST report a specific reason and MUST NOT substitute another
  engine or model (`src/platform/api/engine_registry.hpp` contract; commit `53c23ea`,
  `docs/migration/cpp/compatibility-contracts.md` §15).
- **VT-RULE-002 — No silent data loss.** A stopped session MUST hand over the whole
  captured buffer; overflow MUST fail with `resource_exhausted` instead of returning a
  truncated buffer (512 MiB session bound, `docs/migration/cpp/compatibility-contracts.md` §2).
- **VT-RULE-003 — Final-only recognition.** A transcription is published once, after the
  session stops; blank/whitespace transcripts MUST be suppressed and MUST NOT touch the
  clipboard.
- **VT-RULE-004 — Settings survive an unavailable device.** A remembered microphone that
  is not currently enumerated MUST be kept in settings and shown as unavailable; only the
  explicit "default" choice clears it (`docs/migration/cpp/compatibility-contracts.md` §2, task `t_69d60fb63710`).
- **VT-RULE-005 — Settings are self-saving.** There is no Save button; a change MUST be
  persisted after a 700 ms debounce (`kSettingsAutosaveDebounceMs`, [src/domain/settings.hpp:238](../src/domain/settings.hpp#L238)).
- **VT-RULE-006 — Settings save is atomic.** Save MUST write a temporary file and replace
  the target; the temporary file MUST be removed on every failure path.
- **VT-RULE-007 — First run chooses the UI language from the OS; every later run uses the
  stored `appLanguage`.**
- **VT-RULE-008 — Access is per-user.** Installation, autostart and settings MUST be
  per-user; the product MUST NOT require administrator rights.
- **VT-RULE-009 — Intent parity.** Any deviation from the legacy .NET behavior MUST be
  recorded as an intent-parity decision with a regression test (decision of 2026-09-25, memory `m_6942377a8a96`).

---

## 4. Data model (system view)

The authoritative per-module models are in the module files; this section fixes the
system-level shape.

### 4.1 `AppSettings` — the persisted root entity

23 properties, declaration order = JSON output order; changing the count or the order is a
schema change ([src/domain/settings.hpp:210](../src/domain/settings.hpp#L210), `kAppSettingsPropertyCount = 23`). Full table with
defaults: [domain.spec.md](modules/domain.spec.md) §VT-DOM-1xx.

```
AppSettings
├─ recording_mode            RecordingMode      {push_to_talk|toggle|vad}      = push_to_talk
├─ record_hotkey             string                                            = "Ctrl+Alt+Space"
├─ cancel_hotkey             string                                            = "Ctrl+Alt+Escape"
├─ record_gamepad_button     optional<string>                                  = null
├─ cancel_gamepad_button     optional<string>                                  = null
├─ language                  RecognitionLanguage {auto|ru|en}                  = ru
├─ model_size                ModelSize           {tiny..large}                 = small
├─ transcription_engine      TranscriptionEngine {whisper|parakeet|gigaam}     = whisper
├─ parakeet_model_size       ParakeetModelSize   {q4k|q5k|q6k|q8_0}            = q8_0
├─ gigaam_model_size         GigaamModelSize     {q4_k_m|q5_k_m|q6_k|q8_0}     = q8_0
├─ auto_paste_enabled        bool                                              = true
├─ terms_dictionary          string                                            = "API,CPU,GPU,ASR,STT,TTS,LLM,JSON,IDE,SQL"
├─ silence_threshold_ms      int (300..10000 at the UI)                        = 1200
├─ start_with_windows        bool                                              = false
├─ start_minimized           bool                                              = false
├─ theme                     AppTheme            {system|light|dark}           = system
├─ hide_on_focus_loss        bool                                              = false
├─ app_language              AppLanguage         {ru|en}                       = ru
├─ noise_reduction_enabled   bool                                              = false
├─ temperature               double                                            = 0.0
├─ best_of                   int (1..8)                                        = 3
├─ condition_on_previous_text bool                                             = false
└─ microphone_device_id      optional<string>                                  = null
```

- **VT-SYS-100.** The serializer MUST emit indented UTF-8 JSON with camelCase property
  names and camelCase string enum values, in declaration order.
- **VT-SYS-101.** Reads MUST be case-insensitive; unknown properties MUST be ignored;
  nullable properties MUST be emitted as `null` and never omitted; missing properties MUST
  take their default; invalid JSON/type/enum MUST yield a complete default document.
- **VT-SYS-102.** `gigaamModelSize` and `bestOf` are C++-only extensions; the legacy .NET
  reader ignores them, so a rollback MUST keep working when `transcriptionEngine` is not
  `gigaam`.

### 4.2 Session and request flow

```
Hotkey/gamepad event
  → RecordingStateMachine.Idle→Recording
  → RecordingPort (WASAPI session, PCM16 device format)
  → stop(): whole session → downmix to mono → resample to 16 kHz float
  → optional NoiseSuppressor (256-sample frames, high-pass 0.94, damp 0.6)
  → optional SilenceTrimmingPort (Silero VAD map + energy guard)
  → TermsDictionaryPort → engine (Whisper | Parakeet | GigaAM)
  → text, non-empty? → ClipboardPort (mandatory) → optional PastePort (80 ms, Ctrl+V)
  → RecordingStateMachine.Processing→Idle
```

Data structures on this path: `AudioBuffer` (capacity-bounded PCM16),
`SpeechSegment`/`SpeechMap` (kept spans + detector probabilities),
`TranscriptionRequest` (samples + `SessionOptions` + speech map),
`EngineParameters` (per-engine decoded parameters), `TranscriptionResult` (text + status).
Exact fields: [domain.spec.md](modules/domain.spec.md) §VT-DOM-7xx, [asr.spec.md](modules/asr.spec.md) §VT-ASR-7xx.

### 4.3 Interfaces at a glance

The full method-by-method list is in [platform.spec.md](modules/platform.spec.md) §VT-PLT-1xx.
The frozen ports are:

`AudioCapture`, `AudioConverter`, `CaptureGuard`, `Clipboard`, `Clock`, `CpuTopology`,
`EngineRegistry`, `Executor`, `FileSystem`, `Gamepad`, `Hotkeys`, `HttpClient`, `Lifecycle`,
`Logger`, `MicrophoneService`, `MicrophoneLevel`, `ModelStore`, `Paste`, `Paths`,
`StatusOverlay`, `Transcriber`, `Tray`, `Updater`.

### 4.4 External wire formats

| Format | Producer / consumer | Rules |
|---|---|---|
| `settings.json` | application ↔ disk | §VT-SYS-100…102, [domain.spec.md](modules/domain.spec.md) |
| Model catalog (URL, filename, expected bytes) | application → Hugging Face | [platform.spec.md](modules/platform.spec.md) §model catalog |
| GitHub release JSON | application ← `api.github.com` | §VT-SYS-080…085 |
| `run-update.cmd` | application → installer handoff | §VT-SYS-084 |
| Log lines | application → support | §VT-SYS-070…074 |
| Hotkey string | settings ↔ user ↔ Win32 | `Ctrl+Alt+Space` grammar, [platform.spec.md](modules/platform.spec.md) §VT-PLT-4xx |
| Gamepad binding string | settings ↔ user ↔ XInput/DirectInput | `XInput\|A`, `DInput\|Product\|index` |

---

## 5. Requirement map

| Prefix | Module | File |
|---|---|---|
| `VT-SYS-*`, `VT-RULE-*` | system, cross-cutting | this file |
| `VT-DOM-*` | domain | [modules/domain.spec.md](modules/domain.spec.md) |
| `VT-PLT-*` | platform ports and backends | [modules/platform.spec.md](modules/platform.spec.md) |
| `VT-ASR-*` | recognition engines | [modules/asr.spec.md](modules/asr.spec.md) |
| `VT-COR-*` | core support services | [modules/core-support.spec.md](modules/core-support.spec.md) |
| `VT-UI-*` | settings window, tray, overlay | [modules/ui.spec.md](modules/ui.spec.md) |
| `VT-BLD-*` | build, release, tests | [modules/build-release.spec.md](modules/build-release.spec.md) |

## 6. Glossary

| Term | Meaning |
|---|---|
| Port | An abstract interface in `src/platform/api/*.hpp` that a backend implements. |
| Composition root | `src/app/windows_application.cpp`: the one place where real backends are assembled. |
| Final-only | A transcript is emitted once, after recording stops; no streaming preview. |
| Epoch | A generation counter that invalidates superseded loads/results (`asr::EngineHost`). |
| Kept span / speech map | The VAD's answer carried with the audio so the detector runs once. |
| Intent parity | Implementing the declared behavior instead of reproducing a legacy bug. |
| Device format | The PCM format negotiated from WASAPI (typically 48 kHz stereo PCM16). |
| Session bound | 512 MiB per recording session before `resource_exhausted`. |

## 7. Verification

- **VT-SYS-110.** Every normative rule in this specification SHOULD point at a
  contract test or a documented manual scenario; the mapping is maintained in
  [build-release.spec.md](modules/build-release.spec.md) §VT-BLD-6xx.
- **VT-SYS-111.** Behavior that can only be verified on real hardware (microphone,
  Intel Smart Sound array, overlay over a focused window, autostart across a restart)
  MUST be listed as a manual Windows smoke scenario (`docs/migration/cpp/windows-smoke.md`).
