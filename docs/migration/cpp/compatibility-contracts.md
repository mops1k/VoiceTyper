# VoiceTyper C++ migration — compatibility contracts

**Baseline:** `main` @ `fc7d69c1a32be71a19634c16904d0a29a24135b0`  
**Purpose:** freeze externally observable .NET/Avalonia behavior before implementing C++/Qt replacements.

## 1. Settings JSON

### Serializer contract

- Indented UTF-8 JSON.
- camelCase property names.
- camelCase string enums.
- Property reads are case-insensitive; enum names are case-insensitive.
- Unknown properties are ignored.
- Nullable properties are emitted as `null`; no default-ignore policy.
- Missing properties use constructor/property defaults.
- Invalid JSON/type/enum string causes `Load()` to return a complete default `AppSettings`.
- Current schema has **no `SchemaVersion`**. A future schema version must be optional and backward compatible.
- Integer enum values are currently accepted by `System.Text.Json`; this behavior must be explicitly tested or deliberately deprecated before C++ implementation.
- Duplicate-key behavior is untested and must not be assumed.

### Exact current defaults

| JSON property | Wire type | Default |
|---|---|---|
| `recordingMode` | `pushToTalk` / `toggle` / `vad` | `pushToTalk` |
| `recordHotkey` | string | `Ctrl+Alt+Space` |
| `cancelHotkey` | string | `Ctrl+Alt+Escape` |
| `recordGamepadButton` | string/null | `null` |
| `cancelGamepadButton` | string/null | `null` |
| `language` | `auto` / `ru` / `en` | `ru` |
| `modelSize` | `tiny` / `base` / `small` / `medium` / `large` | `small` |
| `transcriptionEngine` | `whisper` / `parakeet` | `whisper` |
| `parakeetModelSize` | `q4K` / `q5K` / `q6K` / `q8_0` | `q8_0` |
| `autoPasteEnabled` | bool | `true` |
| `termsDictionary` | string | `API,CPU,GPU,ASR,STT,TTS,LLM,JSON,IDE,SQL` |
| `silenceThresholdMs` | integer | `1200` |
| `startWithWindows` | bool | `false` |
| `startMinimized` | bool | `false` |
| `theme` | `system` / `light` / `dark` | `system` |
| `hideOnFocusLoss` | bool | `false` |
| `appLanguage` | `ru` / `en` | `ru` |
| `noiseReductionEnabled` | bool | `false` |
| `temperature` | number | `0.0` |
| `conditionOnPreviousText` | bool | `false` |
| `microphoneDeviceId` | string/null | `null` |

The `AppSettings` declaration order is the current output order. The C++ serializer should either preserve it for byte fixtures or deliberately version the JSON output and provide a migration fixture.

### Load/save behavior

- Default path: `%APPDATA%\\VoiceTyper\\settings.json` on Windows.
- Missing file → defaults.
- `JsonException`, `IOException`, `UnauthorizedAccessException` → defaults; other exceptions may escape.
- Save writes `settings.json.tmp`, then replaces `settings.json` with overwrite semantics.
- No fsync, transaction, backup, concurrency control, or guaranteed temp cleanup exists.
- UI autosave is debounced by 700 ms.
- UI save canonicalizes hotkeys, blanks gamepad strings to null, and clamps `silenceThresholdMs` to 300..10000; direct service save does not clamp.
- C++ must decide whether to preserve failure residue or strengthen atomicity. Any strengthening requires recovery and compatibility tests.

### C++ Phase B read/save decisions (settings_json)

Decided in `src/domain/settings_json.hpp` / `.cpp` and locked by
`tests/contract/settings_json_contract.cpp`:

| Topic | C++ decision | Rationale |
|---|---|---|
| `null` for a **non-nullable** string (`recordHotkey`, `cancelHotkey`, `termsDictionary`) | accepted, stored as empty, **warning**; document still loads | measured: System.Text.Json assigns `null` to a `string` property without `JsonException`, so the document is accepted there; `std::string` cannot hold `null`, and empty is the closest representable value. "Default + error" was rejected — it would refuse a document the measured .NET path accepts. Known residual divergence: C# rewrites the value as `null`, C++ rewrites `""`. |
| `null` for a **nullable** string (`recordGamepadButton`, `cancelGamepadButton`, `microphoneDeviceId`) | unset, silent | plain System.Text.Json semantics |
| Duplicate properties (also case-only) | de-duplicated **before** dispatch, last occurrence wins, superseded value never validated | object binding in System.Text.Json overwrites without validating the earlier value; validating in parse order would wrongly force a document-wide fallback |
| Save of out-of-range UI values | **no `validate()` gate** in `save_file`; values are written verbatim | `SettingsService.Save` serializes what it is given; `AppSettings::validate()` stays for the UI/settings layer that owns clamping |
| Non-finite `temperature` | normalized to `0` by the serializer | JSON has no `NaN`/`Infinity` literal and System.Text.Json throws on it; normalize instead of throwing from the pure formatter (documented divergence) |
| Atomic replace | `.tmp` write + replace, `.tmp` removed on every failure path, `save_file`/`load_file` are no-throw boundaries | stronger than the .NET path (which has "no guaranteed temp cleanup"); covered by overwrite + failed-save tests |
| Missing settings file | defaults, **no** diagnostic | first run is normal; `SettingsService.Load` also returns defaults silently |

Evidence covers exactly these tests. It is not a claim of full storage parity:
unreadable-file diagnostics, concurrent writers and Windows-only replace failures
are not exercised by the contract suite.

## 2. Windows paths and lifecycle

| Surface | Current location/semantics |
|---|---|
| Settings | `%APPDATA%\\VoiceTyper\\settings.json` |
| Models | `%LOCALAPPDATA%\\VoiceTyper\\models` |
| Log | `%LOCALAPPDATA%\\VoiceTyper\\logs\\voiceTyper.log` |
| Updates | `%LOCALAPPDATA%\\VoiceTyper\\updates\\VoiceTyper-{version}-Setup.exe` |
| Native libraries | beside application: `parakeet.dll`, `mc_wasapi.dll` |
| Autostart | `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run`, value `VoiceTyper`, quoted current exe |
| Single instance | `Global\\VoiceTyper_SingleInstance` |

Additional lifecycle rules:

- First run chooses UI language from installed UI culture; an existing settings file always uses stored `appLanguage`.
- Close hides the window; process remains in tray.
- Start minimized creates/assigns the window without showing it.
- Second instance shows a dialog and exits; there is no activation IPC.
- Autostart failures are swallowed and the Run key is not created when absent.
- `IsRunAtStartupEnabled()` checks value existence only and is not used by the current UI.

### Path resolution is one decision (fixed 2026-10-01, task `t_19bc5fcbed99`)

The table above is normative, and the first Windows composition root violated it:
it built its own strings and used `%LOCALAPPDATA%` for **settings**, so the app
created a fresh `settings.json` in the Local profile and silently ignored the
user's real Roaming file. Found by an end-to-end launch (the app reported
`settings: ...\AppData\Local\...`, and `%APPDATA%\VoiceTyper\settings.json`
existed with 783 bytes while the Local one did not).

Now the composition builds a single `domain::AppPaths` from
`domain::windows_app_path_roots(environment, executable_directory)`:

- Roaming for settings (`SpecialFolder.ApplicationData` in
  `SettingsService.cs:37-41`);
- Local for models, logs and updates (`FileLogger.cs:22-25`,
  `ModelManager.cs:68-71`, `UpdateService.cs:46-48`);
- the executable directory for native libraries;
- a missing environment root falls back to the other one and finally to the
  executable directory, so a stripped environment cannot produce an empty path.

The layout is contract-tested on any host by `core-support-contract`
(`check_windows_app_path_roots`: both roots present, only one present, neither
present, and an empty variable counting as unset), because the bug was a decision
made in the wrong place rather than a coding slip in the codec. The
`VOICETYPER_SETTINGS_PATH` / `VOICETYPER_LOG_DIR` / `VOICETYPER_MODELS_DIR`
overrides remain, and they are overrides on top of the resolver, never a second
source of truth.

### The stored microphone is never erased by a launch (fixed 2026-10-01, task `t_69d60fb63710`)

`MainWindow::refresh_devices()` rebuilds the device list on construction. Its
`clear()` + `addItem()` sequence emits `currentIndexChanged`, and the handler for
that signal treated "По умолчанию" as a user choice: on any launch that
enumerated no device (or a list without the remembered one) the stored
`microphoneDeviceId` was reset and `settings.json` was rewritten with `null`.
Reproduced on the target machine, where the C++ build erased the selection the
installed .NET app had stored.

Rules now enforced, each with a regression test in `ui-settings-test`:

- populating the list is not a user edit (`QSignalBlocker` around the rebuild);
- a remembered device that is not currently enumerated is kept in the settings
  and shown as `<id> (недоступен)`, instead of silently switching the user to
  another microphone;
- picking "По умолчанию" is the explicit way to clear it;
- a real selection reaches the running services exactly once. The
  `pending_service_change_ = SettingsChange::microphone` assignment used to sit
  outside the lambda (a botched edit), where it ran during binding and meant a
  microphone change never reached the capture backend at all.

### Engine readiness is logged (added 2026-10-01, task `t_b0731d50b26d`)

The engine state used to be visible only in the window, so "распознавание не
работает" left no line in the log a user could send. The composition now polls
`EngineHost::state()` every 500 ms and logs each transition once
(`engine state=<readiness>` with the availability reason or the load error), and
a terminal state (`failed`, `unavailable`, `model_missing`) is also posted to the
status overlay's error state.


### Windows capture backend (Phase D, `t_bec8c2127352`)

`src/platform/windows/wasapi_capture.{hpp,cpp}`,
`src/platform/windows/windows_microphone.{hpp,cpp}` and
`src/platform/windows/windows_audio_capture.{hpp,cpp}` are the WASAPI link of
the .NET backend chain. The CMake targets `voicetyper_audio_capture` and the
CTest `windows-audio-capture-contract` are declared inside `if(WIN32)`, so a
Linux/macOS configure never sees them.

**Implemented and covered by the contract test:**

- `WindowsMicrophone` implements the frozen `platform::MicrophoneService` over
  `IMMDeviceEnumerator`: active `eCapture` endpoints only, id + friendly name
  (`PKEY_Device_FriendlyName`, then `PKEY_DeviceInterface_FriendlyName` as the
  fallback) + default flag + probed mix format. The default flag is resolved by
  comparing endpoint ids (console role first, communications role second), not
  by enumeration order. Empty and legacy `"0"` both mean "system default", as in
  `RawWasapiCapture.ResolveDevice`.
- Diagnostics replace the .NET "swallow every exception into an empty list":
  a refused enumeration is `unavailable`, a privacy-blocked name is
  `permission_denied`, and an empty *active* list is re-checked against
  `DEVICE_STATEMASK_ALL` so "no microphone" is distinguishable from "the
  microphone is present but not active".
- `WasapiCapture` is device I/O only. It negotiates a configuration from an
  ordered, instrumented candidate list and records every refused combination
  with its HRESULT: shared mix format (event driven) → exclusive
  `WAVEFORMATEXTENSIBLE` PCM16 48 kHz stereo (polling) → exclusive `WAVEFORMATEX`
  PCM16 48 kHz stereo, 1 s buffer (the combination the .NET build found working)
  → shared PCM16. `AUDCLNT_BUFFERFLAGS_SILENT` packets are zero-filled by the
  capture thread (event-driven mode) and delivered as real zeros, so a session
  timeline stays continuous.
- COM discipline: every interface handle is owned by a `ComPtr` and released on
  every path (the .NET `RawWasapiCapture` leaked the `IAudioClient` of every
  refused combination), the capture thread is the only thread that touches
  `IAudioClient`, COM is initialized per thread and balanced, and no HRESULT
  escapes a call boundary.
- `WindowsAudioCapture` is a `domain::RecordingPort`. `stop()` runs the .NET
  order device → unsubscribe → snapshot and only then clears, `drain()` moves a
  watermark so `stop()` still returns the **whole** session, `cancel()` is
  idempotent, and the session is bounded by the D7 512 MiB limit
  (`stop()` then fails with `resource_exhausted` instead of returning a
  truncated buffer). Conversion to the 16 kHz mono float contract goes through
  the portable layer (`domain::downmix_to_mono`, `domain::resample_to_16k`),
  including a local N-channel average for endpoints with more than two
  channels.
- Device-selected capture is honoured: a non-empty `device_id` is passed to
  `IMMDevice::GetDevice`, closing the .NET "selected mic is silently ignored"
  gap for this backend.
- The test opens the default endpoint for ~300 ms, drains mid-session, stops,
  and asserts a non-empty 16 kHz mono buffer whose size is at least the drained
  size, that every sample is inside `[-1, 1]`, and that `cancel()` is idempotent
  and leaves nothing behind. On a machine with no usable endpoint it prints the
  reason and exits 77 (`SKIP_RETURN_CODE`), and every wait inside it is bounded,
  so it cannot hang.

**Observed on the validation machine (2026-09-25), and explicitly NOT claimed:**

- The build/test run under WSL interop: every process launched that way can
  create **no** COM class at all — `CoCreateInstance` returns
  `0x80040154` (`CLASS_E_CLASSNOTAVAILABLE`) even for `CLSID_TaskbarList3`,
  while PowerShell/.NET COM on the same machine works. `MMDevApi.dll` itself
  loads, the class is registered, and `Audiosrv`/`AudioEndpointBuilder` are
  running, so this is an environment restriction, not a backend defect. The
  contract test therefore **skipped** here; real capture has *not* been
  observed on a live endpoint by this slice.
- What the OS reports about the hardware, gathered separately:
  `Win32_SoundDevice` lists `Intel(R) Smart Sound` (Built-in, USB, Bluetooth)
  and `Realtek High Definition Audio`, all `Status=OK`; the active capture
  endpoint is `Микрофон (массив микрофонов Intel® Smart Sound …)`, and
  `Audiosrv`/`AudioEndpointBuilder` are `Running`.
- **Not implemented / not claimed here:** the `mc_wasapi.dll` C ABI
  (`mc_start`/`mc_stop` are process-global with no callback lifetime guarantee
  — the reason the .NET native backend was tried first), the MME (`waveIn`)
  fallback, the NAudio WASAPI wrapper, hot-plug *while recording* (a removed
  endpoint ends the session as `device_lost`; it is not re-resolved onto another
  endpoint), and the physical Intel Smart Sound matrix (which of shared vs
  exclusive is granted on that specific array, measured in the interactive
  session).

### Windows autostart backend (Phase D, LIFE-01, task `t_c0e3b56d32d2`)

`src/platform/windows/win32_startup.{hpp,cpp}`, CMake target
`voicetyper_platform_win32_startup` and the CTest `win32-startup-contract` are
declared inside `if(WIN32)`, appended to `CMakeLists.txt`, so a Linux/macOS
configure never sees them. The header is standard-C++20 with no
`<windows.h>`, exactly like `parakeet_runtime.hpp`.

**Implemented:**

- `Win32Startup` over `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`,
  value `VoiceTyper` — the same key, the same value name and the same
  `REG_SZ` data type the .NET `StartupManager` used.
- `is_enabled()` answers the existence question with the .NET spelling (any
  failure is `false`); `read()` is the diagnostic read that returns
  `{present, command_line, value_type, start_minimized}`, so a UI can show
  the registry instead of a stored setting that may disagree with it. §2
  records that the .NET `IsRunAtStartupEnabled()` "is not used by the current
  UI"; closing that gap is the point of the read.
- **A missing value — or a missing Run key — is `present == false` with a
  successful status, not an error.** "Disabled" is a state, not a failure.
- `set_enabled(true, start_minimized)` stores `"<full path from
  GetModuleFileNameW>"` plus ` --start-minimized` when the setting is on, and
  `set_enabled(false, …)` removes the value. Both directions are idempotent:
  enabling twice stores identical bytes, and disabling an absent value
  succeeds, so a save can never leave a Run value behind.
- The command line is quoted with the **CommandLineToArgvW rules** (embedded
  quotes escaped, backslashes before a quote doubled) rather than by wrapping
  in two literal quote characters. For a normal path the result is
  byte-identical to the .NET `$"\"{exePath}\""`.
- **Intent-parity deviation:** the .NET build never wrote the switch, so
  `startMinimized` only worked for a manually started app. The switch is part
  of the stored entry here, and `src/app/windows_application.cpp` starts with
  the window hidden when the process was launched with `--start-minimized` or
  when the setting is on — "creates/assigns the window without showing it", as
  §2 requires. The window object and the tray icon still exist, so the app
  stays reachable, and the launch source is written to the log.
- **Intent-parity deviation:** a refused write is *reported*
  (`permission_denied`/`io_failure` with the `LSTATUS`) and logged, where the
  .NET `catch {}` made a failed write indistinguishable from a successful one.
- **Intent-parity deviation:** a missing Run *key* is created on write
  (`RegCreateKeyExW`). The .NET path opened the key read/write and returned
  silently when it was absent, so on such a profile the checkbox was a no-op.
- The entry is reconciled through `WindowServices::settings_applied` — the same
  hook `SettingsViewModel.cs:1262` used (`SetRunAtStartup` inside `Save`), so a
  changed `startMinimized` rewrites the command line of an already registered
  entry instead of waiting for the next launch.
- **A launch only repairs an entry it may call its own.** The first version
  reconciled the Run value on *every* start, and one launch of a development
  build (2026-10-01) repointed the installed .NET app's entry at
  `build\windows-mingw-release\deploy\voicetyper-qt-shell.exe`, so Windows would
  have started the build tree at the next logon while the .NET build is still the
  reference release. Now `win32::launch_may_rewrite()` allows a launch to write
  when the entry is absent, unparsable, already ours (refreshing the switch), or
  points at a file that no longer exists — the cutover repair case. An entry that
  names a different, still-present executable is left alone with a warning in the
  log, and the user's explicit «Запускать вместе с Windows» change always writes
  (`apply_autostart(..., reconcile_on_launch)`). Covered by
  `win32-startup-contract` (parsing the stored command line back, including a
  path ending in a backslash, and the four decision cases).
- `--selftest` **reports** the Run key and never writes it: a diagnostic launch
  must not add or remove an autostart entry on the user's machine. The JSON now
  carries `start_with_windows`, `start_minimized`, `autostart_enabled` and
  `autostart_state`.

**Deliberately NOT implemented, and not claimed:**

- **HKCU only.** No per-user/per-machine choice, no `HKLM` write and therefore
  **no elevation prompt** — this backend never shows one and never needs one.
  An autostart for all users is a product decision that does not exist yet.
- **No Task Scheduler dependency.** This is the Run key, not a scheduled task:
  no `schtasks`/`ITaskService`, no trigger delay, no run-level (only-at-logon)
  choice, no "highest privileges" task. Nothing has to keep working across
  Windows task-scheduler API changes.
- **No StartupApproved / Startup-folder support.** The value written here is
  the one the .NET build wrote; the Windows 10/11 «Startup apps» enable/disable
  state Task Manager shows is a separate `Explorer\StartupApproved\Run` value
  and is neither read nor written, so a user who disabled VoiceTyper there can
  still have the Run value present.
- **No per-app installer/update interplay** and no repair of a value whose
  target no longer exists: the entry is rewritten when the setting is on and the
  app runs, not by a background watcher.
- **No `REG_EXPAND_SZ` expansion.** A value is reported exactly as Windows
  stores it; the backend only ever writes `REG_SZ`.
- **Not verified by a restart.** `win32-startup-contract` proves the registry
  contract, not a real logon: scene 22 in `windows-smoke.md` (autostart +
  start-minimized across a real Windows restart) is still a manual scenario.

**Evidence (2026-09-25, Windows MinGW13 + Qt 6.11.2):** configure/build/test
exit codes and the machine facts are recorded in the Phase D task notes of
`.dsh/plans/p_312b2ec83985` (task `t_c0e3b56d32d2`). The contract test writes
only to a per-process value name
(`VoiceTyperStartupContract-<pid>`) in the production key, and asserts the
production `VoiceTyper` value is byte-identical before and after the run: the
installed app's entry on the validation machine
(`"C:\Users\Kvintilyanov\AppData\Local\Programs\VoiceTyper\VoiceTyper.exe"`)
is never written, never deleted, and can never end up pointing at a test build.

### Status overlay (Phase D, LIFE-01, task `t_c0e3b56d32d2`)

Contract: `src/platform/api/status_overlay.hpp` (frozen). Implementation:
`src/app/status_overlay.{hpp,cpp}`, part of the `voicetyper_ui` target, so it is
only built when `VOICETYPER_BUILD_GUI` is on. Test:
`tests/ui/ui_status_overlay_test.cpp` → CTest `ui-status-overlay-test`, which
forces `QT_QPA_PLATFORM=offscreen` itself because the contract is a widget
contract, not a desktop-session contract.

The .NET reference is `VoiceTyper.App/Overlay/StatusOverlayWindow.*`: a 1x1
transparent window, `SystemDecorations=None`, `Topmost`, `ShowInTaskbar=False`,
`ShowActivated=False`, positioned at the horizontal center of the working area,
26 px above its bottom, containing an 11 px dot and a 15 px semibold label, with
a 350 ms `DispatcherTimer` toggling the dot between opacity 1.0 and 0.35 and two
states — «Захват» in `#4C8BF5`, «Распознавание» in `#F5A623`.

**Reproduced:** the same two labels and accent colours; the same 26 px bottom
gap and horizontal centering; the same 350 ms pulse timer interval and the same
1.0/0.35 opacity pair (one full on/off cycle therefore takes 700 ms); the pill
does not enter the taskbar (`Qt::Tool`), does not take focus
(`Qt::WindowDoesNotAcceptFocus`, `Qt::NoFocus`,
`Qt::WA_ShowWithoutActivating`) and cannot swallow a click or a keystroke
(`Qt::WA_TransparentForMouseEvents`).

**Intent-parity decisions (each one asserted by the test):**

- **The `error` state really shows its reason.** The contract has an `error`
  state; the .NET overlay had none and hid itself on idle, so a missing model or
  a lost device produced no visible reason at all. Here a failed dictation posts
  the `error_code_name` plus the error message, and that text **stays on screen
  until the next dictation replaces it** (an `idle` transition does not hide an
  error). New colour `#E5484D` for that state.
- **Geometry is computed in Qt logical pixels** from
  `QScreen::availableGeometry()`. The .NET code multiplied physical pixels by
  `RenderScaling` by hand; `move()` already takes device-independent pixels, so a
  150% monitor gets the same visual 26 px gap without repeating that arithmetic.
- **The pill follows the display the user last interacted with**
  (`QGuiApplication::screenAt(QCursor::pos())`, falling back to the primary
  screen) and is repositioned when a screen is added, removed or becomes
  primary, instead of staying on a display that no longer exists.
- **`destroy()` is terminal.** It is the shutdown path; a queued worker event
  arriving afterwards must not create a window while the process is exiting.
  `hide()` is the reusable one — the pill is shown and hidden on every
  dictation, and hiding must not release the window (frozen in the contract).
- **Worker threads publish through `post_state()`**, a thread-safe queued call;
  the contract's `create/show/set_state/hide/destroy` stay UI-thread-only and
  report `invalid_state` when called from another thread.

**Deliberately NOT implemented, and not claimed:**

- No audio/level meter, no progress bar, no recognised text and no transcript
  preview: the .NET overlay showed status text only, and §7/LOG-01 still forbid
  putting recognised text anywhere but the clipboard.
- No click-through interaction with the pill, no context menu and no drag: it is
  `WA_TransparentForMouseEvents` by design.
- The real-desktop behaviour (no taskbar entry, no activation steal while
  another window is focused) is **manual**: scene "overlay during dictation" in
  `docs/migration/cpp/windows-smoke.md`. The offscreen UI test can only assert
  the window flags and attributes, because the offscreen platform activates the
  only window in its session.

## 3. Clipboard and paste

- Empty/whitespace transcript does not touch clipboard or paste.
- Nonempty transcript always writes the system clipboard.
- Auto-paste waits 80 ms then sends Ctrl+V.
- Avalonia clipboard writer requires the UI thread and main window, retries up to 5 attempts with 120 ms delay.
- If no main window exists, the current writer can silently return.
- UAC integrity mismatch can prevent SendInput paste; clipboard must remain authoritative.
- Linux inherited contract: clipboard mandatory; injection best effort with explicit `clipboard-only` state.

## 4. Model catalog

### Whisper q8

Base URL: `https://huggingface.co/ggerganov/whisper.cpp/resolve/main/`

| Enum | File | Expected bytes |
|---|---|---:|
| Tiny | `ggml-tiny-q8_0.bin` | 43,537,433 |
| Base | `ggml-base-q8_0.bin` | 81,768,585 |
| Small | `ggml-small-q8_0.bin` | 264,464,607 |
| Medium | `ggml-medium-q8_0.bin` | 823,369,779 |
| Large | `ggml-large-v3-turbo-q8_0.bin` | 874,188,075 |

### VAD

- File: `ggml-silero-v6.2.0.bin`
- Base URL: `https://huggingface.co/ggml-org/whisper-vad/resolve/main/`
- Expected bytes: `885,098`

### Parakeet

Base URL: `https://huggingface.co/mudler/parakeet-cpp-gguf/resolve/main/`

| Enum | File | Expected bytes |
|---|---|---:|
| Q4K | `tdt-0.6b-v3-q4_k.gguf` | 675,200,864 |
| Q5K | `tdt-0.6b-v3-q5_k.gguf` | 741,867,360 |
| Q6K | `tdt-0.6b-v3-q6_k.gguf` | 812,700,512 |
| Q8_0 | `tdt-0.6b-v3-q8_0.gguf` | 940,663,680 |

Current download behavior:

- Default timeout 30 minutes; User-Agent `VoiceTyper/1.0`.
- Existing file is accepted solely by `File.Exists`, even if empty/truncated/wrong.
- Writes `<target>.download` with 128 KiB buffer.
- Reports progress at most every 120 ms and once at completion.
- Content-Length overrides the expected progress total when present.
- Atomic-intended move to target; no fsync, checksum, expected-size enforcement, or format validation.
- Error/cancel best-effort deletes `.download`; stale temp files are not cleaned at startup.
- Startup deletes exact legacy fp16/q5 names; a locked legacy file can abort startup.
- Parakeet pinned build commit: `e75de9b6b9b688fd293aa22f7e27aa724ea286f8`; C header ABI version is 6.

C++ must preserve filenames and existing cache locations. Resume/checksum/validation are stability improvements that require explicit policy for old unverified files.

## 5. Audio and ASR behavior

- Output WAV: PCM16, mono, 16 kHz.
- Input conversion: downmix and resample from device format; current WDL resampler is a behavioral reference, not a code dependency.
- Optional denoise: 256-sample frames, high-pass coefficient 0.94, adaptive threshold, damp gain 0.6.
- Silence trim: 160-sample/10 ms frames, 0.25 s edge margin, internal silence over 0.6 s compressed to 0.3 s, 15th-percentile RMS threshold ×3, absolute floor `1e-6`, fallback threshold `0.005`.
- VAD: threshold 0.5, minimum speech 250 ms, no-speech stop 5 s, poll 250 ms.
- Whisper: no-speech threshold 0.6, greedy strategy, state machine requests bestOf 3, physical cores clamped 1..16, serialized context.
- `conditionOnPreviousText` currently only calls `WithNoContext()` when false; no previous transcript is stored/passed. The C++ plan must not claim implemented context without a new contract/test.
- Parakeet ignores language override, prompt, temperature, previous context and bestOf; selected target language is empty/model-auto.

## 6. Hotkey and gamepad string grammar

### Hotkeys

- Case-insensitive modifiers: `Ctrl`/`Control`, `Alt`, `Shift`, `Win`/`Windows`/`Meta`/`Super`/`Cmd`.
- Canonical order: Ctrl, Alt, Shift, Win.
- Keys are normalized by upper-casing the first character.
- Parser accepts a no-modifier string, while UI capture requires a modifier unless F1..F24.
- Reusable examples: `Ctrl+Alt+Space`, `F12`, `NumPad0`, `OemTilde`.

### Gamepads

- XInput: `XInput|A`, validated against the known button enum.
- DirectInput: `DInput|ProductName|zeroBasedButtonIndex`.
- DirectInput product name containing `|` is not representable under the current 3-part grammar.
- C++ must preserve existing bindings or ship a deterministic parser migration.

## 7. Logging

- Path: `%LOCALAPPDATA%\\VoiceTyper\\logs\\voiceTyper.log`.
- Format: `yyyy-MM-dd HH:mm:ss.fff [LEVEL] message`; exception follows on the next line.
- Local time (`DateTime.Now`) and default process encoding.
- Current log is cleared at startup; archives are retained.
- Rotation occurs before append when current size is at least 1,000,000 bytes.
- Archives: `voiceTyper.1.log` ... `voiceTyper.5.log`.
- All logging failures are swallowed.
- UI displays the current file's last 200 lines and polls every 800 ms.
- No tests exist for format, rotation or errors.

Preserve path/format for support tooling, but preserving logs across crashes instead of clearing at startup should be treated as an intentional stability improvement.

## 8. Updater and installer

### Release query

- Endpoint: `https://api.github.com/repos/mops1k/VoiceTyper/releases/latest`.
- Installer asset regex: `^VoiceTyper-\d[^/]*?-Setup\.exe$`.
- First matching asset in API order wins.
- Tag is processed with `TrimStart('v')`, not strict SemVer parsing.
- Prerelease is stored but not filtered.
- SHA marker: case-insensitive whole line `SHA256:` or `SHA-256:` followed by 64 hex characters.
- Missing SHA is accepted.

### Version comparison

- Build metadata after `+` is ignored.
- Dot-separated numeric segments are padded with zero.
- Invalid/overflow numeric segments become zero.
- Stable is newer than any prerelease.
- Two prereleases with equal numeric core compare equal regardless of identifiers.
- Whitespace is not trimmed.

This is not full SemVer. C++ may implement strict SemVer only through an explicit versioned migration/update policy; otherwise it must reproduce the current comparison for the overlap window.

### Download and launcher

- Target: `VoiceTyper-{version}-Setup.exe`; temp target `.download`.
- SHA verified case-insensitively only when supplied.
- SHA mismatch deletes temp and preserves old target.
- Missing SHA allows installation.
- Launcher writes `%LOCALAPPDATA%\\VoiceTyper\\updates\\run-update.cmd` non-atomically.
- Script starts installer with `/AutoUpdate`, waits, then always relaunches the app regardless of installer exit status.
- Installer: fixed AppId, per-user `%LOCALAPPDATA%\\Programs\\VoiceTyper`, x64-compatible, low privileges, exact global mutex, Russian-only installer language.
- Release workflow accepts any `v*` tag, builds/tests/publishes win-x64, invokes hard-coded Inno Setup path, uploads one Setup `.exe`, appends uppercase SHA-256 to release body.

Security/stability improvements—required SHA, strict tags, Authenticode, quoting/argv-safe launcher and explicit rollback—are not current parity and need a staged policy.

## 9. Localization

- Neutral `Strings.resx` is Russian; `Strings.en.resx` is English.
- Audit found 214 data keys in each with matching key names.
- Initial language: Russian; first run can select English from OS culture.
- Missing key returns the key itself.
- `Loc.Apply` changes current/default thread cultures and emits both empty-name and `Item` property notifications.
- App build output retains only `ru`, `en`, `runtimes` and `Assets` culture directories.
- Installer language remains Russian-only.

C++ must add automated key/placeholder parity; sampling two strings is insufficient.

## 10. Golden fixture requirements

Before C++ implementation, materialize fixtures for:

1. Exact default settings JSON (21 keys, order, nulls, indentation and enum spelling).
2. Legacy settings without engine fields; PascalCase properties; unknown fields; numeric/invalid enums; duplicate keys; corrupt JSON.
3. Full all-fields settings roundtrip and failure injection for temp write/move/permissions/concurrency.
4. Windows path suffix contracts with spaces and custom roots.
5. Model catalog URL/filename/size/legacy-cleanup table and HTTP 200/206/404/cancel/short-body behavior.
6. Deterministic log-line regex and 999,999/1,000,000-byte rotation boundary.
7. Updater release JSON/HTTP/SHA/version matrix, including two prereleases and missing SHA.
8. Updater download target/temp/old-target preservation.
9. Hotkey and gamepad grammar roundtrips.
10. Synthetic PCM/WAV fixtures: mono/stereo, 44.1/48 kHz, quiet speech, pure silence, long internal pause, non-canonical chunks.
11. Pactl source text, KDE globals and XDG path parser fixtures from the local multiplatform branch.
12. License-safe real speech + expected transcript corpus; none exists on current `main` or the feature branch.

The manifest at `tests/fixtures/migration/manifest.json` tracks materialization status without embedding proprietary or oversized data.


## 11. Phase B core support contracts

`src/domain/app_paths.*` and `src/domain/file_logger.*` implement the portable
`Paths` and `Logger` contracts. They accept the roaming/local/application roots
from a platform backend, so this slice contains no OS API. The logger keeps the
.NET format, D9 clear-current-only behavior, 1,000,000-byte rotation and five
archives; `clear_on_start` is explicit and defaults to true.

`src/domain/cpu_topology.*` supplies the portable fallback: logical processors
come from `std::thread::hardware_concurrency`; unknown physical cores use
`max(1, logical / 2)` and remain marked unknown. Windows physical-core detection
and the final platform roots are backend work, not implied by this slice.

Evidence: `core-support-contract` covers path suffixes, exact INFO/WARN/ERROR
format, error detail lines, rotation/archive retention, clear-on-start, tail,
and CPU thread clamps. Cleanup of a temporary test directory is best-effort on
Windows because an external scanner can transiently hold a handle.


## 12. Phase B audio contracts

`src/domain/audio_wav.*` is a bounded portable audio slice. `AudioBuffer` returns
`resource_exhausted` on capacity/D7 byte-bound overflow; WAV reading follows the
C# cursor `8 + size + (size & 1)`, checks audioFormat/bits/channels/sampleRate
in that order, and rejects stereo/non-PCM/44.1k fixtures. The writer emits
canonical PCM16 mono 16 kHz. Resampling is a documented linear fractional
stream with tail preservation; downmix is channel average. Noise suppression and
silence trimming use the C# constants (256-frame high-pass, adaptive damp; 160
frame trim, 0.25/0.6/0.3 s and percentile/ratio/floor rules).

Evidence is `audio-dsp-contract` against the 9 WAV fixtures and synthetic
44.1/48 kHz streams. Capture backends, WDL phase behavior, VAD, ASR and Windows
physical audio are deliberately not claimed by this slice.


## 13. Phase B VAD contracts

`src/domain/vad.*` contains the portable final-only VAD policy. It keeps the C#
segmenter seam, reset hook, relative/absolute segment-time normalization,
threshold/minimum-speech/no-speech/energy-floor/hold constants and final stop
reasons. `CallbackSpeechSegmenter` is the cached-context adapter used by a later
native Silero backend; this slice has no model or capture implementation and
never emits a streaming preview. `vad-contract` uses deterministic injected
segments to cover no-speech idle, trailing silence, relative-time normalization,
reset and hold clamps.


## 14. Phase B recording state machine

`src/domain/recording_state_machine.*` implements the abstract-port state
machine: Idle→Recording→Processing→Idle, push-to-talk/toggle/VAD events,
stop-before-process buffer ownership, epoch-scoped idempotent cancellation,
no callbacks under the lock, final-only one-shot transcription, blank-transcript
suppression, optional cached `SpeechSegmenter` reset without destruction, and
injected `RecordingWorker` for deterministic tests (`ThreadRecordingWorker` is
the production adapter). The contract test covers all modes, VAD hook, no-lost
buffer, blank/declined output, error paths, cancel races, dispose and reset.

This is control flow only: microphone capture, native Silero VAD, ASR engines
and clipboard adapters remain platform/ASR phases.

## 15. Phase C native dependency contract

Full detail, evidence and exact commands: `docs/migration/cpp/native-dependencies.md`;
machine-readable source of truth: `docs/migration/cpp/native-dependencies.json`,
which `cmake/NativeAsr.cmake` reads. Nothing here is a behavior change to the
product; it is the dependency boundary the later Phase C tasks build on.

| Item | Frozen value |
|---|---|
| whisper.cpp | `ggml-org/whisper.cpp` @ `d09f61a708f3487afa956ff578e60eae5e7a233c` (upstream 1.9.4, ggml 0.25.1), MIT, compiled from source, **static** |
| parakeet.cpp | `mudler/parakeet.cpp` @ `e75de9b6b9b688fd293aa22f7e27aa724ea286f8` (`v0.5.0-1-ge75de9b`), MIT, shipped `parakeet.dll`, C ABI **6** |
| `native/parakeet.dll` | 5,682,591 bytes, SHA-256 `85c7c65bfd8d467799d6cb43bd3cfac884e27dce27680189b56edc5594ec932b` |
| `native/mc_wasapi.dll` | 114,213 bytes, SHA-256 `4f97ea7fed7670e91a99cc7ba233efc86f04de9e8cfb9e8f8da5bc08d16944e0` (Phase D, no ABI contract yet) |

Rules:

- **Pinned, not "latest".** The pin lives in the manifest, FetchContent
  downloads a `URL_HASH`-verified archive of exactly that commit, and three
  files inside it are re-hashed against values taken from an independent
  `git fetch` of the same commit. A moved tag or a re-cut archive fails
  configure. A replaced or truncated shipped DLL is also a configure error.
- **Only six Parakeet symbols are bound** (`abi_version`, `load`, `free`,
  `transcribe_pcm_lang`, `free_string`, `last_error`), resolved with
  `LoadLibraryW`/`GetProcAddress`. `parakeet_capi_transcribe_pcm_lang` with an
  empty locale is the language-agnostic path, exactly as `parakeet_capi_transcribe_pcm`
  is, so the language-agnostic case is not a second entry point. No
  `parakeet_capi.h` type crosses into portable code; the handle is `void*`.
- **ABI 6 is asserted**, and the manifest and
  `src/platform/api/engine_registry.hpp` (`kParakeetAbiVersion`,
  `kParakeetPinnedCommit`) are cross-checked at every configure.
- **No silent fallback, on any platform.** A missing DLL, an unloadable DLL, a
  different build or a non-Windows host each produce a specific
  `EngineAvailabilityReason` plus a detail string; nothing substitutes another
  engine or another model.
- **Whisper decode parameters are frozen to the .NET values**: threads clamped
  1..16, greedy, `no_speech_thold 0.6`, `temperature_inc 0`,
  `entropy_thold -1`, `logprob_thold -1`, caller `best_of`, `no_context` unless
  previous context is requested, prompt with `carry_initial_prompt`, `"ru"`/`"en"`
  or auto-detect, CPU only, nothing printed. Cancellation is polled from
  whisper.cpp's `abort_callback`, so a cancel stops the compute. Parakeet stays
  non-interruptible per D1: cancellation is observed before and after the
  native call, so the maximum latency is one inference.
- **Offline contract builds stay offline.** `VOICETYPER_BUILD_ASR` defaults to
  `VOICETYPER_BUILD_GUI`; a GUI-off portable build fetches nothing and still runs
  `native-dependency-contract`. A machine that cannot download (no usable CA
  store) can pass `VOICETYPER_WHISPER_PREFETCHED_DIR`, and the content pins are
  verified on that tree exactly as on a download.
- **Two documented toolchain deviations**: the vendored sub-build is pinned to
  `_WIN32_WINNT=0x0601` because MinGW-w64 13.1 lacks
  `THREAD_POWER_THROTTLING_STATE` (only thread power-throttling is skipped), and
  the commit archive is used instead of a git clone (CMake documents
  `GIT_SHALLOW` as incompatible with a hash `GIT_TAG`). Both are recorded in the
  manifest with their rejected alternatives.

Not claimed by this section: recognition quality, latency, RAM, a real model
load, or any .NET differential transcript comparison. The two contract CTests
(`native-dependency-contract`, `whisper-native-contract`) never download or load
a model; the whisper test only proves the pinned sources compile, link and are
the pinned bytes.
