# VoiceTyper — Platform Module Specification

**Covers:** `src/platform/` — the frozen backend interfaces in `src/platform/api/`
(ports) and their implementations in `src/platform/windows/` (real Win32) and
`src/platform/portable/` (fallbacks for non-Windows hosts and CI).
**Baseline:** `main` @ `3cc4c5a9a602afb96c2f478a562005f5eb850068` (v2.2.1).
**Requirement keywords:** RFC 2119. Parent document: [index.spec.md](../index.spec.md).
**Prefix:** `VT-PLT-*`.

- **VT-PLT-001.** Every port in `src/platform/api/` MUST be a header-only abstract interface
  that links nothing: no Qt, no OS SDK, no third-party library
  ([CMakeLists.txt:79-85](../../CMakeLists.txt#L79-L85), `voicetyper_platform` is `INTERFACE`).
- **VT-PLT-002.** Production code above the platform layer MUST use only these interfaces;
  an implementation MUST be reachable through its own CMake target declared behind
  `if(WIN32)` (or the GUI switch) so a non-Windows configure never sees it.

| Port | File | Windows implementation | Linux implementation | Portable implementation |
|---|---|---|---|---|
| `AudioCapture` | [api/audio_capture.hpp](../../src/platform/api/audio_capture.hpp) | **none** (⚠ G-1: no implementing class) | **none** (the seam is `domain::RecordingPort`, see `linux/linux_audio_capture.*`) | — |
| `AudioConverter` | [api/audio_converter.hpp](../../src/platform/api/audio_converter.hpp) | **none** (⚠ G-1: conversion is called directly through `domain::*` free functions) | **none** (same: `domain::downmix_to_mono`/`resample_to_16k`) | — |
| `CaptureGuard` | [api/capture_guard.hpp](../../src/platform/api/capture_guard.hpp) | `capture_guard.cpp` | `capture_guard.cpp` (no-op hooks + `log_stack_trace` through execinfo) | ✓ (no-op hooks) |
| `Clipboard` | [api/clipboard.hpp](../../src/platform/api/clipboard.hpp) | `windows/win32_clipboard.*` | `linux/linux_clipboard.*` (Qt, marshalled to the GUI thread) | `portable_runtime` |
| `Clock` | [api/clock.hpp](../../src/platform/api/clock.hpp) | `windows/win32_clock.*` | `portable_runtime` (`PortableClock`) | `portable_runtime` |
| `CpuTopologyProvider` | [api/cpu_topology.hpp](../../src/platform/api/cpu_topology.hpp) | `windows/win32_*` | `domain::StandardCpuTopologyProvider` | `domain::StandardCpuTopologyProvider` |
| `EngineRegistry` | [api/engine_registry.hpp](../../src/platform/api/engine_registry.hpp) | `asr::NativeEngineRegistry` | `asr::NativeEngineRegistry` | ✓ |
| `Executor` | [api/executor.hpp](../../src/platform/api/executor.hpp) | `windows/win32_executor.*` | `linux/linux_executor.*` (`LinuxExecutor`) | `ManualExecutor`, `InlineExecutor` |
| `FileSystem` | [api/file_system.hpp](../../src/platform/api/file_system.hpp) | `windows/win32_file_system.*` | `portable_runtime` (`PortableFileSystem`) | `portable_runtime` |
| `GamepadService` | [api/gamepad.hpp](../../src/platform/api/gamepad.hpp) | `windows/win32_gamepad.*` | `linux/linux_gamepad.*` (evdev, XInput-name bindings mapped onto evdev codes) | `portable_runtime` |
| `HotkeyService` | [api/hotkeys.hpp](../../src/platform/api/hotkeys.hpp) | `windows/win32_hotkeys.*` | `linux/linux_hotkeys.*` + `linux/linux_keymap.*` (evdev), with `linux/linux_kglobalaccel.*` (D-Bus) as the fallback | `portable_runtime` |
| `HttpClient` | [api/http.hpp](../../src/platform/api/http.hpp) | `app/qt_http_client.*` | `app/qt_http_client.*` | — |
| `LifecycleService` | [api/lifecycle.hpp](../../src/platform/api/lifecycle.hpp) | **none** (⚠ G-2: single-instance and shutdown are implemented ad hoc in `src/app/tray_controller.cpp` and the composition; `SingleInstanceGuard` is never instantiated); autostart lives in `windows/win32_startup.*` | **none** (same ad-hoc `QLocalServer` channel in `src/app/tray_controller.cpp`); autostart lives in `linux/linux_startup.*` | — |
| `Logger` | [api/logger.hpp](../../src/platform/api/logger.hpp) | `windows/win32_logger.*` | `domain::FileLogger` | `domain::FileLogger` |
| `MicrophoneService` | [api/microphone.hpp](../../src/platform/api/microphone.hpp) | `windows/windows_microphone.*` | `linux/linux_microphone.*` (libpulse) | `portable_runtime` |
| `MicrophoneLevelPort` | [api/microphone_level.hpp](../../src/platform/api/microphone_level.hpp) | `windows/mc_wasapi.cpp`, `microphone_level.cpp` | `linux/linux_microphone_level.*` (libpulse source volume/mute) | — |
| `ModelStore` | [api/model_store.hpp](../../src/platform/api/model_store.hpp) | `platform/catalog_model_store.cpp`, `model_catalog.cpp` | `platform/catalog_model_store.cpp`, `model_catalog.cpp` | ✓ (policy) |
| `PasteSimulator` | [api/paste.hpp](../../src/platform/api/paste.hpp) | `windows/win32_paste.*` | `linux/linux_paste.*` (ydotool) | `portable_runtime` |
| `Paths` | [api/paths.hpp](../../src/platform/api/paths.hpp) | `domain::AppPaths` | `domain::AppPaths` + `domain::linux_app_path_roots` (XDG) | ✓ |
| `StatusOverlay` | [api/status_overlay.hpp](../../src/platform/api/status_overlay.hpp) | `app/status_overlay.*` (Qt) | `app/status_overlay.*` (Qt, host-window on Wayland) | ✓ (offscreen in tests) |
| `Transcriber` | [api/transcriber.hpp](../../src/platform/api/transcriber.hpp) | `asr::*` | `asr::*` (Whisper, Parakeet, GigaAM) | ✓ (policy) |
| `Tray` | [api/tray.hpp](../../src/platform/api/tray.hpp) | **none** (⚠ G-3: `app/tray_controller.*` is a Qt class that does not inherit the port) | **none** (same) | — |
| `UpdateService` | [api/updater.hpp](../../src/platform/api/updater.hpp) | **none** (⚠ G-3: `core::support::UpdateService` is its own class, not a `platform::UpdateService` subclass) | **none** (same; the install action replaces the running AppImage, VT-SYS-014) | ✓ (policy) |

---

## 1. Lifecycle and single instance (`VT-PLT-1xx`)

Port: [src/platform/api/lifecycle.hpp](../../src/platform/api/lifecycle.hpp).

- **VT-PLT-101.** `LifecycleService` MUST expose `acquire_single_instance()`,
  `set_run_at_startup(bool)`, `is_run_at_startup_enabled()`, `autostart_command_line()` and
  `shutdown(handler)`.
- **VT-PLT-102.** `SingleInstanceGuard` MUST be move-only and MUST keep the cross-process lock
  for exactly as long as it is alive; `release()` MUST be idempotent
  ([api/lifecycle.hpp:101-123](../../src/platform/api/lifecycle.hpp#L101-L123)).
- **VT-PLT-103.** Shutdown MUST run in the fixed order
  `cancel_recording → stop_capture → dispose_transcriber → dispose_input_and_ui →
  release_single_instance`, and a failing step MUST NOT abort the sequence
  ([api/lifecycle.hpp:91-97](../../src/platform/api/lifecycle.hpp#L91-L97)).
- **VT-PLT-104.** The Windows gate MUST be the global mutex `Global\VoiceTyper_SingleInstance`
  and the process MUST exit on `ERROR_ALREADY_EXISTS` (VT-SYS-040…043).
- **VT-PLT-105.** A second launch MUST send the `show` message over the local socket channel;
  a request arriving before the window exists MUST be remembered and consumed later.

### Autostart (`Win32Startup`)

- **VT-PLT-110.** Autostart MUST use `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`,
  value name `VoiceTyper`, type `REG_SZ` — the same key and name the legacy build used.
- **VT-PLT-111.** `is_enabled()` MUST answer the existence question and MUST return `false` on
  any failure; a missing value or missing Run key MUST be `present == false` with a successful
  status ("disabled" is a state, not an error).
- **VT-PLT-112.** `set_enabled(true, start_minimized)` MUST store the quoted full path from
  `GetModuleFileNameW` plus ` --start-minimized` when the setting is on; `set_enabled(false, …)`
  MUST remove the value. Both directions MUST be idempotent.
- **VT-PLT-113.** Quoting MUST follow `CommandLineToArgvW` rules (embedded quotes escaped,
  backslashes before a quote doubled), not naive double-quoting.
- **VT-PLT-114.** A missing Run key MUST be created on write (`RegCreateKeyExW`); a refused
  write MUST be reported as `permission_denied`/`io_failure` with the `LSTATUS` and logged.
- **VT-PLT-115.** A launch MAY rewrite an entry only under VT-SYS-053
  (`win32::launch_may_rewrite()`); a different, still-present executable MUST be left alone with
  a log warning.
- **VT-PLT-116.** `--selftest` MUST report the Run key and MUST NOT write it.
- **VT-PLT-117.** Only `HKCU` is written: no `HKLM`, no elevation prompt, no Task Scheduler, no
  `StartupApproved` interaction and no `REG_EXPAND_SZ` expansion.

---

## 2. Clock, executor, file system (`VT-PLT-2xx`)

### Clock

- **VT-PLT-201.** `Clock` MUST provide `now()` (steady), `wall_now()` (system),
  `elapsed_since()`, and cancellable `sleep_for()` / `sleep_until()`
  ([api/clock.hpp:39-65](../../src/platform/api/clock.hpp#L39-L65)).
- **VT-PLT-202.** The frozen VAD timing constants MUST live here: poll `250 ms`, minimum speech
  `250 ms`, no-speech stop `5000 ms`, default silence threshold `1200 ms`.

### Executor

- **VT-PLT-210.** `Executor` MUST expose `post(task)`, `post(generation, task)`,
  `post_delayed(generation, task, delay)`, `invoke(task, timeout)`, `shutdown(deadline)`,
  `on_this_thread()` and `pending()` ([api/executor.hpp:24-34](../../src/platform/api/executor.hpp#L24-L34)).
- **VT-PLT-211.** `ManualExecutor` and `InlineExecutor` MUST be deterministic test doubles;
  the production implementation is the Win32 worker executor
  (`windows/win32_executor.*`) and the Qt GUI executor.
- **VT-PLT-212.** UI updates from worker threads MUST be marshalled onto the GUI executor with a
  queued invocation (VT-SYS-033); the Win32 executor is a worker thread and MUST NOT be used to
  touch widgets (commit `eda7312`).
- **VT-PLT-213.** A generation/epoch-scoped post MUST allow a superseded task to be dropped
  without running.

### File system

- **VT-PLT-220.** `FileSystem` MUST expose `exists`, `file_size`, `create_directories`,
  `open_write` (with `FileWriteMode`), `read_text`, `read_binary`, `atomic_write`,
  `replace_file`, `remove_file`, `list_directory` and `available_space`
  ([api/file_system.hpp:96-152](../../src/platform/api/file_system.hpp#L96-L152)).
- **VT-PLT-221.** `atomic_write` and `replace_file` MUST provide the atomic replace required by
  VT-DOM-220 and VT-RULE-006; a failed write MUST leave the previous file intact.

---

## 3. Audio capture (`VT-PLT-3xx`)

Port: [src/platform/api/audio_capture.hpp](../../src/platform/api/audio_capture.hpp).

- **VT-PLT-301.** `CaptureStartOptions` MUST carry `device_id` (empty = system default),
  `requested_format` (default 48000 Hz, 2 channels, PCM16) and a `CancellationToken`.
  A backend MAY negotiate a different format, and the caller MUST read the granted format from
  `device_format()`.
- **VT-PLT-302.** Frames MUST be delivered through a `CaptureFrameSink` on the backend audio
  thread; `CapturedFrame::bytes` is owned by the capture object and valid only for the duration
  of the callback, so a sink that keeps data MUST copy it.
- **VT-PLT-303.** `stop()` MUST report a `CaptureStopReason`
  (`completed | empty | cancelled | device_lost | failed`) so an empty capture is
  distinguishable from device loss without reading logs.
- **VT-PLT-304.** `cancel()` MUST be idempotent and MUST discard the partial buffer;
  `drain()` MUST return what was captured since the previous drain without stopping;
  `destroy()` MUST release the client.

### Windows WASAPI

Contract: `docs/migration/cpp/compatibility-contracts.md` §2; test: `windows-audio-capture-contract`.

- **VT-PLT-310.** `WindowsMicrophone` MUST enumerate **active `eCapture`** endpoints over
  `IMMDeviceEnumerator`, returning id, friendly name (`PKEY_Device_FriendlyName`, falling back to
  `PKEY_DeviceInterface_FriendlyName`), the default flag and the probed mix format.
- **VT-PLT-311.** The default flag MUST be resolved by comparing endpoint ids (console role
  first, communications role second), never by enumeration order. An empty id and the legacy
  `"0"` MUST both mean "system default".
- **VT-PLT-312.** Diagnostics MUST distinguish causes: a refused enumeration is `unavailable`, a
  privacy-blocked name is `permission_denied`, and an empty *active* list MUST be re-checked
  against `DEVICE_STATEMASK_ALL` so "no microphone" differs from "present but not active".
- **VT-PLT-313.** `WasapiCapture` MUST negotiate from an ordered candidate list and MUST record
  every refused combination with its HRESULT: shared mix format (event driven) → exclusive
  `WAVEFORMATEXTENSIBLE` PCM16 48 kHz stereo (polling) → exclusive `WAVEFORMATEX` PCM16 48 kHz
  stereo with a 1 s buffer → shared PCM16.
- **VT-PLT-314.** `AUDCLNT_BUFFERFLAGS_SILENT` packets MUST be delivered as real zeros so the
  session timeline stays continuous.
- **VT-PLT-315.** Every COM interface handle MUST be owned by a `ComPtr` and released on every
  path; the capture thread MUST be the only thread touching `IAudioClient`; COM MUST be
  initialized and balanced per thread; no HRESULT may escape a call boundary (VT-RULE-002,
  VT-SYS-031).
- **VT-PLT-316.** `WindowsAudioCapture` MUST implement `domain::RecordingPort`: `stop()` MUST run
  device → unsubscribe → snapshot and only then clear; `drain()` MUST move a watermark so `stop()`
  still returns the whole session; `cancel()` MUST be idempotent; the session MUST be bounded at
  512 MiB and overflow MUST fail with `resource_exhausted` instead of truncating.
- **VT-PLT-317.** Conversion to the 16 kHz mono float contract MUST go through the portable layer
  (`domain::downmix_to_mono`, `domain::resample_to_16k`), including a local N-channel average for
  endpoints with more than two channels.
- **VT-PLT-318.** A non-empty `device_id` MUST be honoured through `IMMDevice::GetDevice`, closing
  the legacy "selected microphone silently ignored" gap.
- **VT-PLT-319.** Hot-plug **while recording** is deliberately not handled: a removed endpoint
  ends the session as `device_lost` and is NOT re-resolved onto another device.

### Native capture library (`mc_wasapi.dll`)

- **VT-PLT-320.** `mc_wasapi.dll` MUST be shipped beside the executable (114,213 bytes,
  SHA-256 `4f97ea7fed7670e91a99cc7ba233efc86f04de9e8cfb9e8f8da5bc08d16944e0`), hash-verified at
  configure time ([CMakeLists.txt:33-35](../../CMakeLists.txt#L33-L35)).
- **VT-PLT-321.** `mc_wasapi` exposes no ABI contract (its `mc_start`/`mc_stop` are
  process-global with no callback-lifetime guarantee); its use MUST stay confined to the
  microphone-level feature and MUST NOT be the dictation capture path.

### Microphone level and capture guard

- **VT-PLT-330.** `MicrophoneLevelPort::read()` MUST return a level and mute state, and `write()`
  a level and mute flag, for the settings "sensitivity" control. The sensitivity slider is a
  Windows-level control and MUST NOT be persisted as a product setting.
- **VT-PLT-331.** `CaptureGuard` MUST arm a best-effort device release before a session starts,
  disarm it when the session ends normally, and run it exactly once from a crash path
  (`install_crash_release_hook`), taking no mutex on the crash path.
- **VT-PLT-332.** `log_stack_trace(reason)` MUST append a symbolised stack of the calling thread
  to `wrong-thread-<pid>.txt` in the log directory; it MUST be a no-op without `dbghelp`.

---

## 4. Input: hotkeys and gamepads (`VT-PLT-4xx`)

### Hotkeys

Port: [api/hotkeys.hpp](../../src/platform/api/hotkeys.hpp);
implementation: `windows/win32_hotkeys.*`; test: `win32-input-contract`.

- **VT-PLT-401.** `HotkeyService` MUST expose `set_event_sink(sink)`,
  `apply_settings(settings)`, `unregister_all()` and `record_key_code()`; `apply_settings()` MUST
  return a `HotkeyRegistrationReport` listing per-action success and error text.
- **VT-PLT-402.** `HotkeyAction` MUST distinguish "record" from "cancel".
- **VT-PLT-403.** Registration MUST use `RegisterHotKey` on a dedicated message-pump thread
  (record id `1`, cancel id `2`), with modifiers `kModAlt 0x0001`, `kModControl 0x0002`,
  `kModShift 0x0004`, `kModWin 0x0008`, plus `kModNoRepeat 0x4000`.
- **VT-PLT-404.** A registration conflict MUST be reported with a reason that names the real
  cause (a Win key reserved by the OS is not a plain conflict).
- **VT-PLT-405.** Push-to-talk release MUST be observed by polling with
  `kReleasePollIntervalMs = 30`; unregistering MUST release every registration.
- **VT-PLT-406.** Hotkey capture in the settings MUST use a `WH_KEYBOARD_LL` hook
  (`kDefaultPumpActionTimeoutMs = 3000`, install timeout `2000 ms` →
  `timeout "the WH_KEYBOARD_LL hook was not installed within 2000 ms"`), MUST suppress the
  Windows key so a Win combination can be captured, and MUST cancel on `Escape`.
- **VT-PLT-407.** The hook MUST NOT leak: it MUST be removed on capture end, and a refusal to
  install MUST fall back to `RegisterHotKey` rather than leave a half-installed state.
- **VT-PLT-408.** Shutdown of the service MUST be bounded by
  `kDefaultShutdownTimeoutMs = 2000`.
- **VT-PLT-409.** Applied settings MUST be parsed with the domain grammar (VT-DOM-8xx); a hotkey
  the parser rejects MUST be reported, not silently replaced.

### Gamepads

Port: [api/gamepad.hpp](../../src/platform/api/gamepad.hpp);
implementation: `windows/win32_gamepad.*`.

- **VT-PLT-410.** `GamepadService` MUST expose `set_event_sink`, `apply_settings`, `stop`,
  `capture_next(cancellation)`, `cancel_capture()` and `devices()`.
- **VT-PLT-411.** Input MUST be polled every `kGamepadPollIntervalMs = 33` over XInput devices
  `0..3` (`kXInputDeviceCount = 4`) plus DirectInput polling.
- **VT-PLT-412.** A binding MUST be parsed with the domain grammar
  (`XInput|A`, `DInput|Product|Index`); `GamepadEdge` MUST carry the action and the binding so a
  release can be matched to the press.
- **VT-PLT-413.** Device loss MUST be recovered: a controller that disappears MUST NOT leave a
  capture or a recording stuck.

---

## 5. Output: clipboard and paste (`VT-PLT-5xx`)

- **VT-PLT-501.** `Clipboard::set_text(text, cancellation)` MUST write the system clipboard and
  MUST be the authoritative delivery path (VT-DOM-602); `get_text()` and `has_text()` MUST be
  available for diagnostics.
- **VT-PLT-502.** `RetryingClipboard` MUST retry `5` times with `120 ms` between attempts and MUST
  expose `last_attempts()` (VT-DOM-604).
- **VT-PLT-503.** The Windows clipboard writer MUST retry a busy clipboard, and a UAC integrity
  mismatch MUST be reported as an injection failure while the clipboard keeps the text.
- **VT-PLT-504.** `PasteSimulator::paste()` MUST send `Ctrl+V` (`kPasteChord`) as a synthetic
  input to the foreground window; `is_injection_supported()` MUST report whether the backend can
  inject at all.
- **VT-PLT-505.** Automatic pasting MUST be preceded by `kPasteDelay = 80 ms` (VT-DOM-603).

---

## 6. Status overlay (`VT-PLT-6xx`)

Port: [api/status_overlay.hpp](../../src/platform/api/status_overlay.hpp);
implementation: `src/app/status_overlay.*`; test: `ui-status-overlay-test`.

- **VT-PLT-601.** `OverlayState` MUST be `idle | recording | processing | error`, matching the
  recording state machine so the pill can never show a state the recorder is not in.
- **VT-PLT-602.** The overlay MUST be a frameless, always-on-top pill, horizontally centred,
  `26 px` above the bottom of the working area, showing an `11 px` dot and a `15 px` semibold
  label; the two rendered states use `#4C8BF5` (recording) and `#F5A623` (processing). The error
  state is never rendered (VT-PLT-608).
- **VT-PLT-603.** The dot MUST pulse with `kOverlayPulsePeriod = 350 ms` between opacity `1.0` and
  `0.35` (a full cycle therefore takes 700 ms).
- **VT-PLT-604.** The pill MUST NOT enter the taskbar, MUST NOT take focus and MUST NOT swallow a
  click or a keystroke.
- **VT-PLT-605.** Geometry MUST be computed in Qt logical pixels from `QScreen::availableGeometry()`,
  and the pill MUST follow the display the user last interacted with, re-positioning when screens
  are added, removed or the primary screen changes.
- **VT-PLT-606.** `destroy()` MUST be terminal (a queued worker event after shutdown MUST NOT
  create a window); `hide()` MUST be reusable and MUST NOT release the window.
- **VT-PLT-607.** A worker thread MUST publish through a thread-safe queued `post_state()`;
  `create/show/set_state/hide/destroy` MUST be UI-thread-only and MUST report `invalid_state` when
  called from another thread.
- **VT-PLT-608.** The `error` state MUST NOT be rendered. The overlay MUST hide and report `idle`,
  because the reason belongs to the window's status line and the log — a pill that stayed on
  screen sat on top of the window the user was typing into (reported from the running build,
  2026-10-11). `OverlayState::error` stays in the port contract for parity, and an `idle`
  transition MUST leave the overlay hidden.
- **VT-PLT-609.** The overlay MUST NOT show a level meter, a progress bar or any transcript text
  (VT-SYS-012, VT-SYS-073).

---

## 7. Tray (`VT-PLT-7xx`)

Port: [api/tray.hpp](../../src/platform/api/tray.hpp); implementation: `src/app/tray_controller.*`.

- **VT-PLT-701.** `TrayAction` MUST be `open_settings | record | cancel | quit`; `quit` is the only
  action that ends the process.
- **VT-PLT-702.** The offered record/cancel item MUST follow `TrayRecordingState`
  (`idle` → offer record; `recording`/`processing` → offer cancel) so the menu can never show a
  dead action.
- **VT-PLT-703.** `set_readiness()` MUST surface model presence, engine readiness and an optional
  detail in the tooltip.
- **VT-PLT-704.** The tray MUST support `set_tooltip`, `set_recording_state`,
  `set_recording_glyph`, `apply_theme`, `notify` and `quit`.
- **VT-PLT-705.** Tray text MUST be localized through the UI text table and MUST be re-rendered on
  a language change without losing the current recording state (commit `957b340`).

---

## 8. Networking (`VT-PLT-8xx`)

Port: [api/http.hpp](../../src/platform/api/http.hpp); implementation: `src/app/qt_http_client.*`.

- **VT-PLT-801.** `HttpClient` MUST provide `get(request, cancellation)` returning a fully
  buffered `HttpResponse`, and `open(request, cancellation)` returning an `HttpByteStream` for
  large downloads (status code, headers, `content_length()`, `read_into(sink, cancellation)`,
  `close()`).
- **VT-PLT-802.** The default timeout MUST be `kHttpDefaultTimeout = 1800 s`; the user agent MUST
  be `VoiceTyper/1.0`; release requests MUST add `Accept: application/vnd.github+json`; the copy
  buffer MUST be `128 KiB`.
- **VT-PLT-803.** The client MUST follow redirects and MUST NOT treat an intermediate 3xx status
  as the final answer (commit `98e4732`: Hugging Face answers `/resolve/main/` with a 302 to a CDN).
- **VT-PLT-804.** TLS MUST work in the shipped build: the deploy tree MUST include the Qt Schannel
  TLS backend (`tls/qschannelbackend.dll`), otherwise HTTPS fails with "TLS initialization failed"
  ([CMakeLists.txt:1029-1038](../../CMakeLists.txt#L1029-L1038)).
- **VT-PLT-805.** Download progress MUST be reported at most every `120 ms` and once at completion,
  and the sink MUST be invoked off the GUI thread with the caller marshalling to the UI.
- **VT-PLT-806.** Only the allowed endpoints may be contacted: the model hosts, the release API and
  the release asset (VT-SYS-001, VT-SYS-091).

---

## 9. Model store and catalog (`VT-PLT-9xx`)

Port: [api/model_store.hpp](../../src/platform/api/model_store.hpp), implementation
`platform/catalog_model_store.*` + `platform/model_catalog.cpp`; tests: `model-store-contract`,
`model-download-contract`.

- **VT-PLT-901.** `ModelKind` MUST be `whisper | parakeet | vad | gigaam`.
- **VT-PLT-902.** `ModelDescriptor` MUST carry `kind`, `size`, `parakeet_size`, `gigaam_size`,
  `file_name`, `download_url` and `expected_bytes`; the file name and URL MUST be frozen.
- **VT-PLT-903.** `ensure(descriptor, progress, cancellation)` MUST return the path of a
  complete file; `is_downloaded()`, `path_for()`, `models_directory()` and `remove()` MUST be
  available.
- **VT-PLT-904.** Base URLs (frozen):

| Family | Base URL |
|---|---|
| Whisper | `https://huggingface.co/ggerganov/whisper.cpp/resolve/main/` |
| VAD | `https://huggingface.co/ggml-org/whisper-vad/resolve/main/` |
| Parakeet | `https://huggingface.co/mudler/parakeet-cpp-gguf/resolve/main/` |
| GigaAM | `https://huggingface.co/handy-computer/gigaam-v3-e2e-rnnt-gguf/resolve/main/` |

- **VT-PLT-905.** The catalog MUST contain these files and expected sizes:

| Family | File | Expected bytes |
|---|---|---|
| Whisper | `ggml-tiny-q8_0.bin` | 43,537,433 |
| Whisper | `ggml-base-q8_0.bin` | 81,768,585 |
| Whisper | `ggml-small-q8_0.bin` | 264,464,607 |
| Whisper | `ggml-medium-q8_0.bin` | 823,369,779 |
| Whisper | `ggml-large-v3-turbo-q8_0.bin` | 874,188,075 |
| VAD | `ggml-silero-v6.2.0.bin` | 885,098 |
| Parakeet | `tdt-0.6b-v3-q4_k.gguf` | 675,200,864 |
| Parakeet | `tdt-0.6b-v3-q5_k.gguf` | 741,867,360 |
| Parakeet | `tdt-0.6b-v3-q6_k.gguf` | 812,700,512 |
| Parakeet | `tdt-0.6b-v3-q8_0.gguf` | 940,663,680 |
| GigaAM | `gigaam-v3-e2e-rnnt-Q4_K_M.gguf` … `-Q8_0.gguf` | (4 quants, sizes in `model_catalog.cpp`) |

- **VT-PLT-906.** `ModelDownloadProgress::fraction()` MUST be clamped to `[0, 1]` and be `0` when
  the total is unknown; `remaining_seconds()` MUST be `nullopt` when the total is unknown, the
  download is complete or the rate is non-positive.
- **VT-PLT-907.** A download MUST be written to `<file>.download` (suffix
  `kModelDownloadSuffix`), with a `128 KiB` buffer, a `30-minute` timeout, and a final atomic
  move; on error or cancel the temporary file MUST be deleted.
- **VT-PLT-908.** Progress reporting MUST be at most every `120 ms`, and `Content-Length` MUST
  override the expected total when present.
- **VT-PLT-909.** `path_for()` and every catalog entry MUST use the models directory from
  `platform::Paths` (VT-SYS-061), never a locally assembled path.
- **VT-PLT-910.** An already-present file was historically accepted by existence alone; the
  product MUST NOT treat a truncated file as a valid model (VT-RULE-001) — the `.download` +
  rename protocol is what guarantees this for new downloads.

---

## 10. CPU topology (`VT-PLT-10xx`)

Port: [api/cpu_topology.hpp](../../src/platform/api/cpu_topology.hpp).

- **VT-PLT-1001.** `detect()` MUST succeed when only the logical processor count is available, with
  `physical_cores_known = false`, and MUST return `unsupported` only when the platform exposes no
  topology at all.
- **VT-PLT-1002.** Thread counts MUST be derived with the frozen helpers:
  `inference_thread_count()` = physical cores (or `max(1, logical/2)`) clamped to `1..16`;
  `vad_thread_count()` = `logical/2` clamped to `2..8`.
- **VT-PLT-1003.** The detected topology MUST be logged once so a performance report can be
  interpreted.

---

## 11. Linux backends (`VT-PLT-11xx`…`VT-PLT-16xx`)

Added 2026-10-10 with the multiplatform work (VT-SYS-010). Every rule below
describes `src/platform/linux/`, `src/app/linux_application.cpp` and the Linux
branches of the shared runtime loaders. The Windows rules above are unchanged.

### Lifecycle, paths and executor (`VT-PLT-11xx`)

- **VT-PLT-1101.** `domain::linux_app_path_roots(environment, application_directory)` MUST
  resolve the XDG layout: settings under `$XDG_CONFIG_HOME/VoiceTyper` (fallback
  `$HOME/.config`), models/logs/updates under `$XDG_DATA_HOME/VoiceTyper` (fallback
  `$HOME/.local/share`). An unset **or empty** variable MUST be treated as unset, a missing
  root MUST fall back to the other one, and a completely stripped environment MUST fall back
  to the application directory — never to an empty path
  ([src/domain/app_paths.cpp](../../src/domain/app_paths.cpp)).
- **VT-PLT-1102.** `linuxos::executable_file_path()` MUST resolve `/proc/self/exe` with a
  buffer that grows until the link fits, and MUST return an empty path when it cannot
  ([src/platform/linux/linux_paths.cpp](../../src/platform/linux/linux_paths.cpp)).
- **VT-PLT-1103.** Autostart MUST be a freedesktop desktop entry
  `$XDG_CONFIG_HOME/autostart/voicetyper.desktop`, written atomically (temporary file +
  rename) so a crash cannot leave a truncated entry, with the `Exec` value quoted by the
  freedesktop rules and ` --start-minimized` appended when the setting is on
  ([src/platform/linux/linux_startup.cpp](../../src/platform/linux/linux_startup.cpp)).
- **VT-PLT-1104.** Enabling twice MUST store byte-identical content, and disabling an absent
  entry MUST succeed (idempotence in both directions).
- **VT-PLT-1105.** A launch-time reconciliation MUST NOT rewrite an entry that names a
  different executable which still exists (the Linux form of VT-SYS-053).
- **VT-PLT-1106.** Single instance on Linux MUST use the `QLocalServer` channel of
  `src/app/tray_controller.cpp`; the global mutex of VT-PLT-104 is Windows-only. A second
  launch MUST exit and the owner MUST raise its window.
- **VT-PLT-1107.** `LinuxExecutor` MUST reproduce the frozen `Executor` semantics: a
  generation-tagged `post`, silent drop of a stale epoch, `invoke()` running inline when
  called on the executor's own thread, `shutdown()` clearing the queue and advancing the
  epoch, and an exception becoming `ErrorCode::internal`
  ([src/platform/linux/linux_executor.cpp](../../src/platform/linux/linux_executor.cpp)).

### Audio (`VT-PLT-12xx`)

- **VT-PLT-1201.** Microphone enumeration MUST use libpulse sources and MUST exclude monitor
  sources (`monitor_of_sink != PA_INVALID_INDEX`); the id MUST be the source name and an
  empty id MUST mean "the system default source"
  ([src/platform/linux/linux_microphone.cpp](../../src/platform/linux/linux_microphone.cpp)).
- **VT-PLT-1202.** An empty device list MUST be a success (VT-PLT-302), and the diagnostic
  MUST distinguish "no sound server" from "no capture device".
- **VT-PLT-1203.** Capture MUST be a `domain::RecordingPort` over `pa_simple` (10 ms blocks)
  that delivers 16 kHz mono float through `domain::downmix_to_mono` /
  `domain::resample_to_16k`, with the D7 bound of VT-DOM/D7
  ([src/platform/linux/linux_audio_capture.cpp](../../src/platform/linux/linux_audio_capture.cpp)).
- **VT-PLT-1204.** A source name that does not exist MUST be detected by enumeration *before*
  the stream is opened: measured 2026-10-08, pipewire-pulse answers `pa_simple_new()` with a
  working stream for an unknown name and no error.
- **VT-PLT-1205.** `stop()` without a live session MUST be `invalid_state`, `cancel()` MUST be
  idempotent, `drain()` MUST move a watermark without clearing, and a session that hits the
  bound MUST fail with `resource_exhausted` instead of returning a truncated buffer (the
  Windows rules of VT-PLT-3xx).
- **VT-PLT-1206.** The microphone level MUST be the default source's volume and mute through
  `pa_sw_volume_to_linear` / `pa_sw_volume_from_linear`, reported as `available == false`
  when the session has no sound server or no source
  ([src/platform/linux/linux_microphone_level.cpp](../../src/platform/linux/linux_microphone_level.cpp)).
- **VT-PLT-1207.** The capture backend MUST expose the last ~0.2 s peak for the microphone
  test (the Linux form of the Windows level meter).

### Input (`VT-PLT-13xx`)

- **VT-PLT-1301.** The key map MUST translate the settings' WPF key names ("Space", "D0"…"D9",
  "NumPad0"…"NumPad9", "F1"…"F24", "Oem*", letters) to Linux input event codes and back, and
  MUST report 0 / an empty name for anything it does not know
  ([src/platform/linux/linux_keymap.cpp](../../src/platform/linux/linux_keymap.cpp)).
- **VT-PLT-1302.** Global hotkeys MUST be read from `/dev/input/event*` nodes that report
  `EV_KEY` and the letter keys, and MUST deliver the press **and** the release edge
  (push-to-talk) on a backend-owned reader thread
  ([src/platform/linux/linux_hotkeys.cpp](../../src/platform/linux/linux_hotkeys.cpp)).
- **VT-PLT-1303.** A session without readable input devices MUST report per-hotkey
  registration errors (never a whole-call failure) with a diagnostic naming the `input` group
  or a udev rule, and MUST NOT block.
- **VT-PLT-1304.** `unregister_all()` MUST stop the reader and close every device descriptor;
  it MUST be idempotent and MUST leave no callback in flight.
- **VT-PLT-1305.** The capture hook MUST reject a bare key without a modifier unless it is
  F1…F24 (the frozen rule of `src/domain/hotkey_gesture.hpp`) and MUST return `cancelled` on
  Escape or on a cancelled token.
- **VT-PLT-1306.** (was G-15) The Linux gamepad backend MUST poll `/dev/input/event*` nodes that
  report the gamepad buttons (`BTN_SOUTH`/`BTN_EAST`/`BTN_NORTH`/`BTN_WEST`) **and** the absolute
  axes (`EV_ABS`), deliver the press **and** the release edge on a backend-owned poll thread at
  the frozen 33 ms cadence, and debounce an edge until the same sample has been seen twice,
  exactly as the Windows backend does. A node that reports the button codes without `EV_ABS` — a
  virtual keyboard/mouse, such as ydotoold's device — MUST NOT be treated as a controller
  (measured live 2026-10-11)
  ([src/platform/linux/linux_gamepad.cpp](../../src/platform/linux/linux_gamepad.cpp)).
- **VT-PLT-1310.** The settings bindings keep the Windows grammar, so the backend MUST map the
  `XInputPadButton` names onto evdev codes: A=`BTN_SOUTH`, B=`BTN_EAST`, X=`BTN_WEST`, Y=`BTN_NORTH`,
  LB=`BTN_TL`, RB=`BTN_TR`, LT/RT=`ABS_Z`/`ABS_RZ` normalised to the 0..255 scale with the 30
  threshold, the D-pad to `ABS_HAT0X`/`ABS_HAT0Y`, Start=`BTN_START`, Back=`BTN_SELECT`,
  LeftStick=`BTN_THUMBL`, RightStick=`BTN_THUMBR`. A `DInput|...` binding parses and is accepted but
  never matches — the same deliberate gap the Windows backend documents.
- **VT-PLT-1311.** An absent or empty binding MUST be a success ("no gamepad action"), a present but
  malformed one MUST be `invalid_argument`, and a machine with no controller MUST keep polling with
  an empty `devices()` instead of failing. `capture_next()` MUST return the first pressed button as
  its settings spelling and MUST be cancelable; losing a controller MUST NOT stop the service.
- **VT-PLT-1312.** The composition MUST connect the gamepad edges to the same recording machine as
  the hotkeys and MUST expose `capture_gamepad` to the settings window, which shows the record and
  cancel bindings in their own card with read-only readouts (empty when nothing is bound, and the
  capture buttons disabled with a reason when the port is absent).
- **VT-PLT-1307.** When the session has no readable `/dev/input` node, the global hotkeys MUST fall
  back to `org.kde.kglobalaccel` through D-Bus
  ([src/platform/linux/linux_kglobalaccel.cpp](../../src/platform/linux/linux_kglobalaccel.cpp)).
  Registration MUST be `doRegister(actionId)` followed by
  `setShortcut(actionId, keys, flags)` with `flags = SetPresent | NoAutoloading` (`SetPresent = 2`,
  `NoAutoloading = 4`, from `kglobalaccel_p.h`): without `SetPresent` kglobalaccel stores the keys
  but keeps the component inactive and emits no signal at all (measured 2026-10-11). The action id
  is `{"voicetyper", "record" | "cancel", "VoiceTyper", "<friendly>"}`, and the signals are read
  from `/component/voicetyper`, the same object the C++ API subscribes to.
- **VT-PLT-1308.** The fallback MUST report what it can deliver, because the release edge decides
  whether push-to-talk works at all. `HotkeyCapability` is `none` when neither mechanism is
  available, `evdev` while keyboards are open (press **and** release), `kglobal_accel` when the bus
  exposes `globalShortcutReleased`, and `kglobal_accel_press_only` when it does not. In the
  press-only case the composition MUST force `RecordingMode::toggle` for the session and say so in
  the window (`UiKey::k182`), never pretending push-to-talk is available.
- **VT-PLT-1309.** The key names of the settings file MUST map to Qt key codes for the fallback
  (`hotkey_qt_key_code`, `hotkey_qt_modifier_flags`): `Qt::Key_*` plus `Qt::KeypadModifier` for
  `NumPad0`…`NumPad9`, and `Qt::*Modifier` for the gesture modifiers. A name the map does not know
  MUST be refused per hotkey, not registered as something else. With kglobalaccel unreachable the
  apply MUST report a per-hotkey reason, `unregister_all()` MUST release both actions and stay
  idempotent, and no edge may reach the sink after it returns.

### Output: clipboard and paste (`VT-PLT-14xx`)

- **VT-PLT-1401.** The clipboard MUST be Qt's, marshalled to the GUI thread with a bounded
  wait; a call with no `QGuiApplication` MUST be `unavailable`, never a silent success (the
  gap the Windows backend closes, VT-PLT-501)
  ([src/platform/linux/linux_clipboard.cpp](../../src/platform/linux/linux_clipboard.cpp)).
- **VT-PLT-1402.** A clipboard holding an empty string MUST read back as "no text", exactly
  like the Windows backend.
- **VT-PLT-1403.** Paste MUST be a synthetic Ctrl+V through ydotool
  (`key 29:1 47:1 47:0 29:0`), available only when the tool and the daemon socket exist;
  otherwise it MUST report `unavailable` so the caller raises the explicit `clipboard_only`
  state (VT-PLT-504)
  ([src/platform/linux/linux_paste.cpp](../../src/platform/linux/linux_paste.cpp)).
- **VT-PLT-1404.** The paste simulator MUST expose the suspend flag used while the settings
  window captures a hotkey.

### Status overlay on Wayland (`VT-PLT-15xx`)

- **VT-PLT-1501.** The overlay MUST satisfy VT-PLT-602/605 (horizontally centred, 26 px above
  the bottom of the working area) also on Wayland, where a client cannot place its own
  top-level window: the top-level MUST be a full-screen click-through host window and the pill
  a child widget positioned by the client
  ([src/app/status_overlay.cpp](../../src/app/status_overlay.cpp)).
- **VT-PLT-1502.** The host window MUST NOT take focus, MUST NOT swallow input and MUST NOT
  appear in the window list / taskbar (the flags of VT-PLT-603/604), and the pill MUST remain
  the widget the UI tests address as `statusOverlay`. On Wayland the plain window flags are
  **not sufficient** for that: an ordinary `xdg-toplevel` still lands in the window list and can
  be activated (reported from the running build, 2026-10-11). The host MUST therefore be
  promoted to a `wl-layer-shell` surface through LayerShellQt with `LayerOverlay`,
  `KeyboardInteractivityNone`, an anchor on the **bottom edge only** (the compositor centres it
  horizontally), the frozen gap as the bottom margin and `ExclusiveZone = -1`, and MUST NOT be
  raised on show. The surface MUST be sized to the pill, never to the screen: a screen-sized
  surface in the overlay layer swallows every click, because its input region covers the whole
  screen (also reported 2026-10-11). `overlay_needs_layer_shell()` is the testable decision
  (Wayland only) and `QtStatusOverlay::layer_shell_active()` reports what happened; without
  LayerShellQt the build keeps the ordinary window and configures.

### Engine libraries (`VT-PLT-16xx`)

- **VT-PLT-1601.** Parakeet and GigaAM MUST load `libparakeet.so` / `libtranscribe.so` through
  `dlopen`/`dlsym`, binding exactly the symbol lists of VT-ASR-5xx/6xx and asserting the same
  ABI (Parakeet ABI 6, transcribe.cpp 0.3.1) as the Windows DLLs
  ([src/platform/windows/parakeet_runtime.cpp](../../src/platform/windows/parakeet_runtime.cpp),
  [src/platform/windows/transcribe_runtime.cpp](../../src/platform/windows/transcribe_runtime.cpp),
  `#elif defined(__linux__)` branches).
- **VT-PLT-1602.** The library MUST be looked for first next to the running executable and then
  in `<exe_dir>/engine-libs`, so a development build works without copying files.
- **VT-PLT-1603.** A missing or unloadable library MUST report
  `EngineAvailabilityReason::native_library_missing` with the loader's own message; no other
  engine MAY be substituted (VT-ASR-3xx).
- **VT-PLT-1604.** `libparakeet.so` MUST link its ggml statically: parakeet.cpp pins ggml 0.13
  and transcribe.cpp pins ggml 0.25 while both shared libraries carry the same
  `libggml*.so.0` soname, so two shared copies cannot coexist in one process
  ([CMakeLists.txt](../../CMakeLists.txt), target `voicetyper_parakeet_cpp`).

## 12. Known gaps

- **G-1.** `platform::AudioCapture` and `platform::AudioConverter` have **no implementation** in
  this repository. The capture path that runs is `WindowsAudioCapture : domain::RecordingPort`
  plus `WasapiCapture` plus `McWasapiSession`; conversion is performed by the
  `domain::downmix_to_mono` / `domain::resample_to_16k` free functions
  ([src/platform/windows/windows_audio_capture.cpp:207-233](../../src/platform/windows/windows_audio_capture.cpp#L207-L233)).
  A requirement phrased against `AudioCapture`/`AudioConverter` therefore has no implementing
  code today.
- **G-2.** `platform::LifecycleService` has **no implementation**; `SingleInstanceGuard` is never
  instantiated. The single-instance gate and the shutdown order are implemented ad hoc in
  `src/app/tray_controller.cpp` and in the composition root.
- **G-3.** `platform::Tray` and `platform::UpdateService` are **not implemented as ports**:
  `app/tray_controller.*` is a Qt class and `core::support::UpdateService` is a plain class.
  Only `platform::StatusOverlay` has a real implementation outside `src/platform/`
  (`QtStatusOverlay`).
- **G-4.** `unregister_all()` does not remove the permanent dictation low-level hook and does not
  clear its gestures, so hotkey edges may still reach an installed sink after the call. The port
  contract (`api/hotkeys.hpp:127-129`) says no callback may still be in flight after the return;
  whether "no further events" is also required must be decided explicitly.
- **G-5.** `PortableFileSystem::remove_file` returns `not_found` for an absent file
  ([src/platform/portable/portable_runtime.cpp:243-246](../../src/platform/portable/portable_runtime.cpp#L243-L246)),
  contradicting `api/file_system.hpp:141-143` and the Win32 implementation, which both treat an
  absent file as success.
- **G-6.** `api/http.hpp:18-21` requires that the release API must not silently follow a redirect
  to another host, but `QtHttpClient` only sets `NoLessSafeRedirectPolicy` and never compares
  hosts; the rule is not implemented at this baseline.
- **G-7.** The hotkey release-poll cadence is 30 ms in code while comments say 25 ms.
- **G-8.** On a pump timeout in `apply_settings` the real OS registration state is undefined:
  the operation may still complete after the call returned `timeout`.
- **G-9.** In hook mode `record_key_code()` returns the record virtual key even though
  `RegisterHotKey` was never called for it, so the value does not prove an OS registration.
- **G-10.** `platform/api/updater.hpp` documents a `.download` temp file renamed onto the target,
  while `core::support::UpdateService::download` writes the **target** directly and deletes the
  target on error. The header and the implementation disagree (see
  [core-support.spec.md](core-support.spec.md) §G-1).
- **G-11.** `WavAudio::frame_count()`/`duration_seconds()` include the 44-byte header while
  `payload_bytes()` does not (see [domain.spec.md](domain.spec.md) §G).
- **G-12.** `ModelDownloadService::download` deletes an existing target file before starting, so a
  failed re-download leaves no old file. This is intentional (a file that "looks like a model"
  must not survive a failed download) but resume/skip behavior is not specified.
- **G-13.** Hardware-only verification is not formalized in code: the physical Intel Smart Sound
  array check and the backend-chain acceptance procedure exist only as comments and plan notes;
  `windows-audio-capture-contract` skips with exit 77 when no usable endpoint is present.
- **G-14.** The synthetic `VK 'V'` injection has no layout-specific handling and no test proving
  correctness under non-Latin keyboard layouts.
