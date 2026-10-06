# VoiceTyper C++ migration — Phase 0 baseline

**Статус:** source/contract evidence collected; exact-commit CI build/test confirmed; physical Windows startup and dictation baseline captured; Linux physical matrix pending  
**Master plan:** `p_7524fedbf69e`  
**Phase 0 subplan:** `p_735286e2bf5b`  
**Источник:** локальная ветка `main`, без изменения product-кода

## 1. Идентификация baseline

| Параметр | Проверенное значение |
|---|---|
| Branch | `main` |
| HEAD | `fc7d69c1a32be71a19634c16904d0a29a24135b0` |
| Tag | `v1.1.3` |
| Remote state | `main...origin/main`, HEAD совпадает с `origin/main` |
| Solution | `VoiceTyper.slnx`: `VoiceTyper.App`, `VoiceTyper.Core`, `VoiceTyper.Tests` |
| UI | Avalonia 11.3.21, `net10.0-windows`, `win-x64` |
| Core TFM | `net10.0-windows` |
| Tests | 15 test source files, 17 public test classes, 87 `Fact`, 14 `Theory`, 78 `InlineData`: 165 statically counted cases |
| Native assets | `VoiceTyper.App/Native/mc_wasapi.dll`, `parakeet.dll`, `parakeet_capi.h`, `parakeet/BUILD.txt` |
| License | MIT (`LICENSE`) |

Рабочее дерево до начала Phase 0 output creation имело untracked paths:

```text
.dsh/
continuous-mode-plan.md
project-plan.md
```

Tracked/staged diff был пуст. Эти paths не очищались. После создания Phase 0 документов добавлены untracked `docs/migration/cpp/` и `tests/fixtures/migration/manifest.json`; product source/workflow/config не изменялись.

## 2. Toolchain

| Среда | Результат |
|---|---|
| WSL/non-interactive shell | `dotnet` отсутствует в `PATH` |
| WSL SDK | `$HOME/.dotnet/dotnet`, SDK `10.0.401`, MSBuild `18.9.11`, host/runtime `10.0.12`, `linux-x64` |
| Windows host | `/mnt/c/Program Files/dotnet/dotnet.exe`, host `10.0.11`, установленных SDK нет; доступны runtimes .NET `8.0.14` и `10.0.11` |
| Visual Studio/MSBuild | В стандартных Program Files paths `MSBuild.exe`, `devenv`, `vswhere.exe` и Visual Studio не обнаружены |

Approved build workflow использует `$HOME/.dotnet/dotnet`. Этого SDK недостаточно для Windows solution, потому что отсутствует `Microsoft.WindowsDesktop.App.WindowsForms` targeting pack. Установка SDK/pack не входит в Phase 0 и не выполнялась.

## 3. Команды baseline и CI

### Локальный WSL workflow

| Команда | Результат |
|---|---|
| `git status --short --branch` | exit 0; `main...origin/main`, pre-existing untracked paths |
| `git rev-parse HEAD` | exit 0; `fc7d69c1a32be71a19634c16904d0a29a24135b0` |
| `git log -5 --oneline` | exit 0; startup/minimized, Win-key capture, Parakeet и Avalonia commits |
| `dotnet --info` | exit 127; команда отсутствует в `PATH` |
| `$HOME/.dotnet/dotnet --info` | exit 0; SDK `10.0.401` |
| `$HOME/.dotnet/dotnet restore VoiceTyper.slnx` | exit 0; 3 projects restored; только generated `obj`/NuGet assets |
| `$HOME/.dotnet/dotnet build VoiceTyper.slnx -c Release --no-restore` | exit 1; `NETSDK1073`: framework `Microsoft.WindowsDesktop.App.WindowsForms` not recognized; 0 warnings, 1 error |
| `$HOME/.dotnet/dotnet build ... -p:EnableWindowsTargeting=true` | exit 1; `NETSDK1127`: targeting pack `Microsoft.WindowsDesktop.App.WindowsForms` is not installed |
| `$HOME/.dotnet/dotnet test VoiceTyper.Tests/VoiceTyper.Tests.csproj -c Release --no-build` | exit 1; testhost aborted: `Microsoft.WindowsDesktop.App 10.0.0` x64 отсутствует, `No frameworks were found` |
| GUI/runtime smoke | выполнен через уже установленный v1.1.3; новый publish exe не запускался |

Build logs доступны только во временной среде агента: `/tmp/voicetyper-phase0-build.log` и `/tmp/voicetyper-phase0-build-enable-windows-targeting.log`; они не являются repository deliverables. Evidence subplan: `p_772df9e476be`.

### Exact-commit Windows CI

GitHub Actions Release run `36128060214` подтверждён через API:

- `head_sha`: `fc7d69c1a32be71a19634c16904d0a29a24135b0`
- `windows-latest`
- `status`: `completed`, `conclusion`: `success`
- Restore, Build, Test, Publish, Inno Setup, SHA-256, release upload и SHA-body steps — success.
- [Run API](https://api.github.com/repos/mops1k/VoiceTyper/actions/runs/36128060214)
- [Jobs API](https://api.github.com/repos/mops1k/VoiceTyper/actions/runs/36128060214/jobs)

Это закрывает reproducible build/test baseline для exact commit, но не заменяет physical runtime measurements.

## 4. Physical Windows startup baseline

Read-only проверка выполнялась через абсолютный Windows PowerShell из WSL.

- Установленный файл: `C:\Users\Kvintilyanov\AppData\Local\Programs\VoiceTyper\VoiceTyper.exe`.
- FileVersion: `1.1.3.0`; ProductVersion: `1.1.3+fc7d69c1a32be71a19634c16904d0a29a24135b0`.
- SHA-256: `E178CF49C10E13A878551B4F472C39B0209B54E90D2730A8B4611F3D29A00D43`; size: `267776` bytes.
- Process: PID `8256`, `Responding=True`, hidden tray window, 28 threads.
- Startup log:
  - settings mode `PushToTalk`, language `Ru`, model `Small`, auto-paste `True`;
  - configured record hotkey `Alt+Win+Space`, cancel `Ctrl+Alt+Space`;
  - Parakeet ABI 6 and q8 GGUF loaded;
  - Silero VAD loaded;
  - microphone `NATIVE-WASAPI`, Intel Smart Sound test ≈1260 ms;
  - VAD warmup `18101 ms`;
  - deep engine warmup `19434 ms`.
- Initial observed process metrics: Working Set `159068160` bytes; Private Bytes `1137774592` bytes.

## 5. Physical dictation baseline

Read-only parsing of the Windows log after the user interaction found:

- 10 complete `Recording → Processing → TextReady` cycles.
- Recognized phrases included `Проверка миграции.`, `Проверка миграции два.` … `Проверка миграции восемь.`.
- `Recording → Processing`: min `1074 ms`, p50 `2208 ms`, p95 `2651 ms`.
- `Processing → TextReady`: min `409 ms`, p50 `438 ms`, p95 `22732 ms` by nearest-rank with n=10; the p95 is the first cold cycle, while the remaining nine were `409–678 ms`.
- One additional `Recording` had no following `Processing`/text and is counted as an incomplete interaction.
- `Error`, `Warn`, `Failed`, and explicit `cancel` events: 0.
- User confirmed normal Windows dictation; the configured record hotkey was `Alt+Win+Space`, not `Ctrl+Win+Space`.
- Process remained alive/responding; a later 180-second monitor observed stable process state but no additional interaction in that particular window.

The stale repository publish executable was not used: its ProductVersion was `1.0.0+8aa1080…`, so it is not a valid v1.1.3 runtime baseline.

## 6. Read-only reference environment and artifact measurements

### Environment

- WSL2 Arch Linux, kernel `6.18.33.2-microsoft-standard-WSL2`, distro `archlinux`.
- Windows 11 Pro, version/build `10.0.26200`/`26200`, 64-bit.
- CPU: Intel Core i7-1165G7 @ 2.80 GHz, 4 physical / 8 logical cores.
- Windows physical RAM: `16,895,152,128` bytes; WSL-visible RAM: `7,995,008 kB` (`8,186,888,192` bytes).

### Existing artifacts (potentially stale)

| Path | Files | Regular-file bytes | Note |
|---|---:|---:|---|
| `VoiceTyper.App/bin/Release/net10.0-windows/win-x64/publish` | 488 | 200,267,054 | pre-existing, mtime 2026-09-10 |
| `VoiceTyper.App/bin/Release/net10.0-windows/win-x64` | 555 | 240,644,910 | may include build outputs |
| `dist` | 2,223 | 1,483,295,430 | stale/pre-existing; 4 symlinks not followed |
| `VoiceTyper.App/Native/mc_wasapi.dll` | 1 | 114,213 | binary only; source absent |
| `VoiceTyper.App/Native/parakeet.dll` | 1 | 5,682,591 | pinned Parakeet build |

These are byte counts only, not a clean publish or performance result. Hardware/artifact collection is recorded in plan `p_49ef390bda00`.

## 7. Что подтверждено по исходникам

- `VoiceTyper.Core/Models/AppSettings.cs`: 21 JSON property, camelCase string enums, без `SchemaVersion`.
- `VoiceTyper.Core/Services/SettingsService.cs`: `settings.json.tmp` + move; load fallback на defaults при JSON/IO/Unauthorized ошибках.
- `VoiceTyper.Core/Audio/AudioRecorder.cs`: fallback order Native WASAPI → Raw WASAPI → NAudio WASAPI → MME.
- `VoiceTyper.Core/Services/RecordingStateMachine.cs`: `Idle → Recording → Processing`, Push-to-Talk/Toggle/VAD, final full transcription, cancellation.
- `VoiceTyper.Core/Audio/SileroSpeechSegmenter.cs` и `SilenceAutoStopDetector.cs`: Silero VAD, threshold `0.5`, min speech `250 ms`, no-speech max `5 s`, default silence threshold `1200 ms`.
- `VoiceTyper.Core/Services/Transcription/WhisperEngine.cs` и `ParakeetEngine.cs`: Whisper.net и native Parakeet C API; model load и deep warmup выполняются в фоне.
- `VoiceTyper.App/App.axaml.cs`: Avalonia composition root, tray/overlay, global hotkeys, model initialization, single-instance `Global\\VoiceTyper_SingleInstance`.
- `VoiceTyper.App/ViewModels/SettingsViewModel.cs`: 700 ms autosave debounce, UI languages RU/EN, model download/delete/update flows.
- `VoiceTyper.Core/Services/ModelManager.cs`: q8 Whisper, Silero и Parakeet q4_k/q5_k/q6_k/q8_0 catalog; temporary `.download` files; legacy q5/fp16 cleanup.
- `VoiceTyper.Core/Services/UpdateService.cs`: GitHub latest release API, `VoiceTyper-*-Setup.exe` asset selection, optional body `SHA256: <hex>`, custom semver comparison.

## 8. Известные baseline gaps

1. README и `project-plan.md` содержат устаревшее описание WPF/NHotkey/q5, тогда как source — Avalonia/custom hotkeys/q8.
2. `mc_wasapi.cpp` и header отсутствуют во всей reachable Git history; доступна только binary DLL без стабильного source/ABI contract.
3. Managed `RawWasapiCapture` не содержит найденного `IAudioClient.Start()`; fallback не подтверждён.
4. Native и MME paths не получают selected device ID; MME hardcodes device 0.
5. `HideOnFocusLoss` сохраняется и показывается в UI, но отдельный consumer скрытия окна не найден.
6. `TrayIcon.ApplyTheme` и `SetRecording` — no-op; `LastText` не имеет найденного XAML binding.
7. `SettingsViewModel.LoadFromSettings` не загружает `MicrophoneDeviceId`; `RefreshMicrophones` выбирает первое устройство.
8. `conditionOnPreviousText` только вызывает `WithNoContext()` при false; previous transcript не хранится и не передаётся.
9. Source-level tests не покрывают real WASAPI/microphone, real inference, global hotkey registration/hooks, gamepad runtime, tray/overlay/autostart/single-instance, updater launcher, DPI/theme hardware и Linux physical matrix.
10. WSL/WSLg evidence не является Linux acceptance evidence для `/dev/input`, PipeWire, clipboard injection, tray и physical microphone.

## 9. Runtime measurement status

| Scenario | Evidence | Status |
|---|---|---|
| Cold process → visible window | installed v1.1.3 startup captured; hidden tray path | partial |
| Global hotkey → recording state | `Alt+Win+Space` produced 10 complete cycles | measured |
| Stop → lossless audio tail | no explicit tail-loss instrumentation; text output succeeded | partial |
| Whisper inference | no real Whisper cycle in this run | pending |
| Parakeet inference | 10 Parakeet cycles recognized | measured |
| Model/VAD warmup | log durations captured: 18.101 s / 19.434 s | measured |
| CPU/RAM/package size | process metrics and stale artifact sizes captured | partial |
| Windows hardware smoke | native mic test and dictation passed; explicit cancel not logged | mostly passed |
| Linux physical matrix | not run; WSL is non-acceptance | pending |

## 10. Phase 0 exit conditions

- Source/build baseline воспроизводим на exact-commit Windows CI. **Build/test confirmed; local WSL limitation documented.**
- Feature matrix, compatibility contracts и fixture manifest созданы.
- Known gaps превращены в явные blocking/non-blocking решения.
- Performance methodology и release budgets предложены; hardware-only values не выдумываются.
- Diff не содержит product-code изменений.
- Structured summary представлен пользователю; phase 1 не начинается без отдельного Approve.
