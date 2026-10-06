# VoiceTyper C++ migration — proposed release gates

**Status:** proposals for Phase 0 approval; not yet accepted budgets  
**Rule:** no number is a measured baseline unless the table says `measured`.

## 1. Measurement method

Use the same reference machine, power plan, microphone/input fixture, model file and audio corpus for .NET and C++.

Record at least:

- 5 cold launches and 20 warm launches.
- 30 push-to-talk, 30 toggle and 30 VAD cycles.
- The same five speech fixtures for Whisper and the same five for Parakeet.
- 3 repeats per fixture/model/engine combination.
- p50 and p95, not average only.
- CPU time, working set, peak private bytes and model-load time.
- Process exit code, audio duration, transcript, backend label and exception text.
- Package byte size and count of bundled runtime/native libraries.

Hardware-only acceptance must be run on physical Windows and Arch Linux machines. WSL Arch may compile and run portable unit tests only.

## 2. Baseline evidence status

### Exact-commit CI evidence

GitHub Actions Release run `36128060214` was verified through the GitHub API:

- `head_sha`: `fc7d69c1a32be71a19634c16904d0a29a24135b0`
- `windows-latest`
- `status`: `completed`, `conclusion`: `success`
- Restore, Build, Test, Publish, Inno Setup, SHA-256, release upload and SHA-body steps all succeeded.
- [Run API](https://api.github.com/repos/mops1k/VoiceTyper/actions/runs/36128060214)
- [Jobs API](https://api.github.com/repos/mops1k/VoiceTyper/actions/runs/36128060214/jobs)

Local WSL restore succeeded, but local build/test was blocked by the absent WindowsDesktop targeting/runtime. The exact commit CI result is the reproducible build/test baseline; it does not replace physical runtime measurements.

### Read-only reference environment

- WSL2 Arch Linux, kernel `6.18.33.2-microsoft-standard-WSL2`, distro `archlinux`.
- Windows 11 Pro, version/build `10.0.26200`/`26200`, 64-bit.
- CPU: Intel Core i7-1165G7 @ 2.80 GHz, 4 physical / 8 logical cores.
- Windows physical RAM: `16,895,152,128` bytes; WSL-visible RAM: `7,995,008 kB` (`8,186,888,192` bytes).

### Physical Windows runtime evidence

- Installed v1.1.3 process: `C:\Users\Kvintilyanov\AppData\Local\Programs\VoiceTyper\VoiceTyper.exe`, ProductVersion `1.1.3+fc7d69c…`, SHA-256 `E178CF49…`, PID `8256`.
- Startup: Parakeet ABI 6/q8 GGUF, Silero VAD, `NATIVE-WASAPI` Intel Smart Sound, mic test ≈1260 ms.
- Warmup: Silero `18101 ms`; engine deep warmup `19434 ms`.
- 10 complete `Recording → Processing → TextReady` cycles were recognized on Windows.
- `Recording → Processing`: min `1074 ms`, p50 `2208 ms`, p95 `2651 ms`.
- `Processing → TextReady`: min `409 ms`, p50 `438 ms`, nearest-rank p95 `22732 ms` (first cold cycle; remaining nine `409–678 ms`).
- `Error`/`Warn`/`Failed`/explicit `cancel`: 0; one incomplete `Recording` without text.
- A later monitor window observed no additional interaction; this does not invalidate the earlier logged cycles.

### Existing artifacts (potentially stale)

| Path | Files | Regular-file bytes | Note |
|---|---:|---:|---|
| `VoiceTyper.App/bin/Release/net10.0-windows/win-x64/publish` | 488 | 200,267,054 | pre-existing, mtime 2026-09-10 |
| `VoiceTyper.App/bin/Release/net10.0-windows/win-x64` | 555 | 240,644,910 | may include build outputs |
| `dist` | 2,223 | 1,483,295,430 | stale/pre-existing; 4 symlinks not followed |
| `VoiceTyper.App/Native/mc_wasapi.dll` | 1 | 114,213 | binary only; source absent |
| `VoiceTyper.App/Native/parakeet.dll` | 1 | 5,682,591 | pinned Parakeet build |

These are byte counts only, not a clean publish or performance result. Hardware/artifact collection is recorded in plan `p_49ef390bda00`.

| Metric | .NET evidence | Gate |
|---|---|---|
| Test inventory | 165 cases statically derived from 87 Fact, 14 Theory and 78 InlineData; exact-commit Windows CI Test step succeeded | CI baseline confirmed; C++ must port/pass equivalent suite |
| Build | exact-commit Windows CI Build and Release workflow succeeded; local WSL build is environment-blocked | CI baseline confirmed |
| Cold/warm launch | installed v1.1.3 startup captured; hidden tray path | partial |
| Hotkey → recording | `Alt+Win+Space` produced 10 complete cycles | measured on Windows |
| Stop → audio ready | text output succeeded; no tail-loss instrumentation | partial |
| Whisper latency/RAM/quality | no real Whisper cycle in this run | pending licensed speech corpus |
| Parakeet latency/RAM/quality | 10 Parakeet cycles recognized; latency sample captured | measured sample, corpus pending |
| VAD real-model behavior | real Silero startup/warmup captured; mode behavior pending | partial |
| Package size | existing artifacts may be stale | clean publish required |
| Crash/hang/leak | no soak | Phase 9 |
| Linux physical behavior | historical WSL tests only | mandatory Arch matrix below |

## 3. Proposed quantitative budgets

These values are intentionally conservative proposals, not claims about the current app. Final values require the reference measurement and user approval.

| Gate | Proposed release requirement |
|---|---|
| Windows build/tests | 100% existing 165 cases ported/passed or explicitly superseded; zero P0 sanitizer findings |
| Linux portable tests | Same core tests pass on Arch Linux/KDE build environments; no Ubuntu target |
| Launch | p95 process start → usable settings/tray UI ≤ max(measured .NET p95 × 1.25, 2.5 s) on reference hardware |
| First model readiness | UI/hotkeys usable before model download/load completes; p95 model-ready time ≤ measured .NET × 1.25 |
| Hotkey → recording state | p95 ≤ max(measured .NET p95 × 1.25, 200 ms) |
| Stop → finalized WAV | p95 ≤ max(measured .NET p95 × 1.25, 250 ms); zero lost tail bytes beyond the approved 20 ms device quantum |
| ASR latency | Same model/audio/engine p95 ≤ measured .NET × 1.25; no unexplained regression by model size |
| ASR quality | Normalized WER/CER on the frozen corpus may not worsen by more than 0.02 absolute versus the pinned .NET/native baseline |
| CPU/RAM | Peak CPU time and working set ≤ measured .NET × 1.25 unless an approved change explains it |
| Audio memory | Bounded queue/ring buffer; no unbounded growth during a 30-minute recording |
| Reliability | 1,000 mixed sessions and a 24-hour soak with zero crash, hang, settings/model corruption or lost final transcript |
| Settings/model data | Zero data loss on upgrade, downgrade and failed-update rollback |
| Package size | Report-only for first spike; after .NET baseline is measured, proposed ceiling is measured .NET self-contained package × 1.10 |
| Linux stable | Mandatory physical Arch matrix has zero blocking failures and every limitation is explicit/documented |

## 4. Windows acceptance matrix

Minimum Windows 10 and Windows 11 x64:

- Clean install, upgrade, repair and uninstall.
- Start normal/start minimized, close-to-tray, tray settings/record/quit.
- Second instance and upgrade overlap with .NET.
- Default and non-default microphones; hotplug/disconnect.
- Intel Smart Sound native path and ordinary shared WASAPI device.
- Forced backend fallback chain.
- Global hotkeys outside app focus; conflict and Win-key capture.
- Push-to-talk release, toggle stop, VAD and cancel during processing.
- XInput and generic DirectInput controller.
- Whisper and Parakeet model download/switch/warmup/dispose.
- RU/EN, Light/Dark/System, DPI 100/150/200%, mixed-DPI monitors.
- Clipboard-only and auto-paste, UAC mismatch diagnostic.
- 8/24-hour soak, sleep/resume, network interruption and disk-full failure injection.

## 5. Arch Linux stable acceptance matrix

WSL is explicitly non-acceptance. Ubuntu/GNOME is out of scope.

| Axis | Mandatory environment |
|---|---|
| Distribution/DE | Arch Linux KDE Plasma 6 (единственная целевая Linux-дистрибуция) |
| Session | X11 and Wayland |
| Audio | Native PipeWire; PulseAudio or pipewire-pulse compatibility environment |
| Hardware | Built-in mic, external USB mic, physical keyboard, one XInput-compatible gamepad, two monitors |
| Permissions | Normal user; `/dev/input` access; denied-portal/evdev scenario with explicit diagnostic |
| Lifecycle | Login autostart, logout, session logout, suspend/resume, stale/second instance |
| UI | Tray, overlay, theme, settings dialog, no Wayland focus theft |
| Integration | Global hotkey activate/deactivate/release, VAD, both ASR engines, clipboard/paste, package update/open release page |

The C++ implementation may be beta with documented limitations, but it cannot be called stable until all Blocking rows pass. If Wayland denies injection, clipboard-only must be an explicit state; silent failure is a failed gate.

## 6. Cross-platform quality gates

- No Windows headers or Linux headers in portable domain/audio/ASR libraries.
- No hidden single-instance/write race; concurrency tests under TSan.
- ASan/UBSan for all tests; TSan for capture/state/engine lifecycle.
- Fuzz settings JSON, WAV parser, release JSON and model metadata.
- All network commands use argv/typed APIs, not shell-concatenated strings.
- No telemetry and no audio/text network transmission.
- Model/download/update metadata is validated before atomic install.
- Failed update preserves the old binary and all user data.
- Diagnostics report session/audio/input/backend/library versions without secrets or recognized text unless the user explicitly exports a redacted bundle.

## 7. Risk register

| ID | Risk | Evidence/impact | Mitigation | Owner phase |
|---|---|---|---|---|
| R1 | `mc_wasapi.cpp` source/ABI absent | Only binary DLL is present in all Git history; no reproducible source or ABI negotiation | Reimplement native WASAPI cleanly or recover source externally; define tested C ABI; never decompile as primary implementation | 5 |
| R2 | `main` and local multiplatform branch diverged | Branch is local-only at `514fe60`; Linux fixes and partial-frame DSP fix are not on main | Preserve commit hashes, extract tests/fixtures, port fixes explicitly, do not merge .NET UI code | 0/3 |
| R3 | Local Windows baseline unavailable in WSL | Missing WindowsDesktop targeting/runtime; no Windows SDK/MSBuild; exact-commit CI succeeds | Use exact-commit CI for build/test; keep local limitation explicit; never alter project to hide it | 0 |
| R4 | Raw WASAPI fallback may be nonfunctional | No `IAudioClient.Start()` found; no hardware test | Implement/test correct COM/WASAPI lifecycle or remove fallback from advertised order | 5 |
| R5 | Selected microphone is not restored | ViewModel omits `MicrophoneDeviceId` on load; first device selected | Golden settings/device test and explicit selected-endpoint capture | 5/6 |
| R6 | `HideOnFocusLoss`, tray record semantics and previous-text context are incomplete | Settings/UI exist but consumers/logic are missing or contradictory | Product decision per item, then implement and test or explicitly defer | 0/6 |
| R7 | Qt LGPL/deployment obligations | Dynamic Qt keeps obligations lower but notices/source availability remain | Pin Qt, use dynamic modules, generate notices/SBOM, run clean-machine tests | 1/8 |
| R8 | Linux Wayland global shortcuts/injection restrictions | Portal and helpers may be denied or unavailable | Portal-first, X11/evdev fallback, clipboard-first, explicit capability state | 7 |
| R9 | PipeWire/PulseAudio/device variability | Historical implementation used external `pw-record`/`parec`; no physical matrix | Prefer PipeWire C API, bounded queue, device-change tests, exact diagnostics | 7 |
| R10 | evdev layout/permissions/hotplug | Historical parser is x86_64-specific; udev cannot fix intentionally mode-000 nodes | Typed `input_event`, hotplug/rebuild, secure udev policy, denied-node diagnostics | 7 |
| R11 | Model files accepted by existence only | Zero-byte/truncated/wrong files block or crash engines | Minimum-size/format validation and optional published checksums; repair/re-download policy | 4 |
| R12 | Parakeet upstream/ABI drift | Bundled header/build is pinned to commit `e75de9...`, ABI v6; current app only checks ABI > 0 | Pin source commit, require/support tested ABI matrix, runtime ABI check, model license notices | 4 |
| R13 | Settings save is not a full transaction | No fsync/backup/concurrency; `.tmp` may remain | QSaveFile/atomic replacement plus backup/recovery and fault-injection tests | 3 |
| R14 | Updater accepts missing SHA and weak version/asset matching | Optional SHA, incomplete SemVer, first-match asset, shell batch launcher | Stage strict tag/SHA/signature policy; preserve overlap parser only where required | 8 |
| R15 | Current audio buffer can grow without a bound | `List<byte>` accumulates a whole recording and callbacks append per byte | Typed ring buffer with explicit maximum and observable overflow behavior | 3/5 |
| R16 | Native callback/COM ownership defects | Current managed code has lifecycle/release and no verified raw start | RAII wrappers, unit/integration tests, sanitizer/leak checks | 5/7 |
| R17 | Logger clears current log on startup | Crash evidence is lost; full recognized text is logged | Preserve format/path but retain previous/current logs unless privacy policy says otherwise | 3/9 |
| R18 | Documentation drift | README/project-plan describe WPF/q5 and stale defaults | Derive contracts from code + golden fixtures; update docs only at cutover | 0/10 |
| R19 | WSL evidence overstates Linux stability | WSL cannot validate input/audio/tray/paste hardware | Mandatory physical Arch matrix and explicit stable prohibition | 0/7/9 |
| R20 | No real ASR golden corpus | Synthetic audio tests exist but no licensed speech/expected transcript set | Create license-safe corpus with normalized WER/CER thresholds | 0/4 |

## 8. Phase gate checklist

- [x] `.NET` build/tests pass for the exact baseline commit in Windows CI.
- [x] Reference machine metadata and physical Windows startup/dictation sample recorded.
- [x] All 21 settings properties and wire values covered by fixtures.
- [x] Model/update/log/localization contracts covered.
- [ ] `mc_wasapi` source/ABI strategy chosen and recorded.
- [ ] Current product gaps have explicit implement/defer decisions.
- [x] Local multiplatform branch reusable tests/fixtures are indexed.
- [x] Proposed budgets are recorded, pending user approval.
- [x] No product source/workflow changes were made during Phase 0.
- [ ] Arch Linux physical acceptance remains pending; it is required before Linux stable.
- [ ] User approves the structured Phase 0 summary before Phase 1 planning/execution.
