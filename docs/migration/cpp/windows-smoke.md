# Windows smoke: how to run the C++ build and what to check

This is the hand-off document for the C++ port. It is the physical-run
counterpart of the contract tests: the tests prove behaviour with fakes, this
page proves the real machine. Every scenario names the parity row it closes, so
a result can be recorded against evidence instead of "seemed to work".

## 0. Build

```powershell
$env:VOICETYPER_MINGW_ROOT = "C:\Users\Kvintilyanov\tools\Qt\Tools\Tools\mingw1310_64"
$env:VOICETYPER_QT_ROOT    = "C:\Users\Kvintilyanov\tools\Qt\6.11.2-mingw-retry\6.11.2\mingw_64"
# MinGW13 MUST come before WinLibs, or every test dies with 0xC0000139.
$env:PATH = "$env:VOICETYPER_MINGW_ROOT\bin;C:\Users\Kvintilyanov\tools\mingw64\bin;$env:PATH"

cmake --preset windows-mingw-release --fresh -B build/windows-smoke `
      -DVOICETYPER_WHISPER_PREFETCHED_DIR="$env:LOCALAPPDATA\Temp\vt-whisper-d09f61a7"
cmake --build build/windows-smoke
ctest --preset windows-mingw-release --test-dir build/windows-smoke
```

`-DVOICETYPER_WHISPER_PREFETCHED_DIR` is required: the Windows CMake curl cannot
verify github.com (libcurl code 60). The offline path still re-hashes the three
manifest content pins, so the tree is only accepted when it is byte-identical to
the pinned commit. `-B build/windows-smoke` keeps this build out of the way of
other builds on the same machine.

The runtime DLLs must sit next to the exe. Copy the Qt6 runtime from
`$env:VOICETYPER_QT_ROOT\bin` (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`, `Qt6Network`,
`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`) into
`build/windows-smoke`, plus `parakeet.dll` from
`native/parakeet/`. whisper.cpp and ggml are linked statically, so
no model-side DLL is needed. Do **not** copy the newer WinLibs `libstdc++-6.dll`
over the MinGW13 one.

## 1. Automated start proof

```powershell
build/windows-smoke/voicetyper-qt-shell.exe --selftest
```

It builds every service, prints a JSON report (settings path, engine readiness,
device count, hotkey errors) and exits 0. Exit 0 with `engine_readiness` moving
from `loading` to `ready` within a minute is the "the app starts and can see the
machine" check. `--selftest` never loads a model unless
`VOICETYPER_WHISPER_MODEL` is set.

## 2. Scenarios

Record the result of each row. "Pass" needs a note on what was observed, not
just a tick.

| # | Scenario | Closes | How to check | Expected |
|---|---|---|---|---|
| 1 | Second launch while the first runs | SHELL-02 | start the app, run the exe again | the first window comes forward, no second microphone owner, no settings change |
| 2 | Close to tray, then quit | SHELL-01 | close the window, look for the tray icon; choose Выход | app keeps running hidden; quit releases hotkeys and exits |
| 3 | Settings survive a restart | SET-01 | change theme, language, hotkey; wait ~1 s; quit; start again | the same values are shown, and `settings.json` in `%LOCALAPPDATA%\VoiceTyper` has them |
| 4 | Autosave timing | SET-02 | change one value and watch the status bar | the "изменено" indicator clears ~700 ms after the last edit |
| 5 | Push-to-talk dictation | REC-01, REC-02 | hold the record hotkey, speak, release | the full sentence appears in the focused editor exactly once, no partial text |
| 6 | Toggle mode | REC-01 | switch to Toggle; press once, speak, press again | one dictation per press pair, nothing on the first press alone |
| 7 | VAD mode auto-stop | VAD-01 | switch to VAD, speak, then stay quiet | recording stops after the silence threshold, the speech before it is transcribed |
| 8 | Cancel during recording | REC-03 | start recording, press the cancel hotkey | no text is delivered, the app is idle again, a second cancel is harmless |
| 9 | Cancel during recognition | REC-03 | speak a long clip, press cancel while it is transcribing | no text appears, and the next dictation still works (the engine was not freed) |
| 10 | Microphone selection | AUD-01 | pick a device in Микрофон, dictate | audio comes from the chosen device; the selection survives a restart |
| 11 | Hotkey conflict | KEY-01 | bind a combination another app already owns | registration is refused with a readable reason in the status area, the other app keeps its hotkey |
| 12 | Clipboard only (paste declined) | OUT-01 | run VoiceTyper elevated, dictate into a normal editor | the text lands in the clipboard, is NOT pasted, and the state reads clipboard_only |
| 13 | UAC mismatch, reverse direction | OUT-01 | run the editor elevated, dictate into it | same: clipboard filled, no injection, no crash |
| 14 | Busy clipboard | OUT-01 | hold the clipboard with another app that keeps it open | up to 5 attempts over ~600 ms, then a clear error instead of a silent success |
| 15 | First Parakeet dictation | ASR-02 | switch the engine to Parakeet, dictate | either a correct transcript, or an explicit "движок недоступен" naming ABI/reason — never a Whisper result |
| 16 | Engine switch mid-session | ASR-01 | dictate with Whisper, switch to Parakeet, dictate again | the switch takes effect; the first transcript is not lost, no crash |
| 17 | Model missing | ASR-01 | point the model path at a deleted file | the UI says the model is not selected; dictation refuses with a readable error |
| 18 | Long transcript | OUT-01 | dictate until the clipboard holds ~1 MB | the whole text is written and pasted, nothing truncated |
| 19 | Exit during inference | REC-03 | start a long dictation and quit from the tray immediately | clean exit, no crash, and the next start is not affected |
| 20 | Offline | PRIV-01 | disconnect the network, dictate | recognition still works; nothing is sent anywhere |
| 21 | Log hygiene | LOG-01 | read the log after the run | paths and error codes are present; no audio and no recognised text in the log |
| 22 | Startup minimized | LIFE-01 | enable автозапуск + свёрнутым, restart Windows, log in | the app appears only in the tray, no window pops up |
| 23 | Overlay while dictating | LIFE-01 | focus a text editor, hold the record hotkey, speak, release, keep typing through it | a frameless pill appears at the bottom center showing «Захват» (blue dot pulsing every ~350 ms), then «Распознавание» (orange), then disappears; it never appears in the taskbar or Alt+Tab, never takes focus, and the typing/caret in the editor is untouched |
| 24 | Overlay keeps an error visible | LIFE-01 | point the model path at a deleted file, dictate, then fix the setting and dictate again | the pill shows the error with its reason and stays visible while idle; the next dictation replaces it with «Захват» |
| 25 | Overlay follows the active display | LIFE-01 | move the mouse to a second monitor and dictate | the pill appears centered at the bottom of that monitor, not of the primary one |

## 3. If a model is missing

The model cards/download screen is not ported yet: the store exists
(`CatalogModelStore`, with its own 14-scenario contract) but no screen drives it.
So if a model file is absent the app **says so and refuses the dictation** with
`model_missing` in the status line and in the log; it does not download anything
and it does not fall back to another engine or model.

To point the app at a specific model without touching the UI, set
`VOICETYPER_WHISPER_MODEL` before launching — it applies only while the selected
engine is Whisper, so it can never silently force the other engine. Otherwise the
app resolves `%LOCALAPPDATA%\VoiceTyper\models` and picks the file name for the
size chosen in Модели (`ggml-*-q8_0.bin` or `tdt-0.6b-v3-*.gguf`).

A Parakeet dictation additionally needs `parakeet.dll` next to the executable,
which `voicetyper-deploy` copies. When the DLL is missing or the ABI is not 6 the
app reports the engine as unavailable with the reason and keeps working on the
engine you actually selected.

## 4. What is still not covered here

- `UPD-01` update check/rollback has no C++ implementation yet; the update menu
  entry is absent rather than silently failing.
- mc_wasapi.dll's original C ABI is not bound: capture uses the native WASAPI
  path directly, so `NATIVE -> RAW-WASAPI -> WASAPI` collapse into one path and
  the MME fallback does not exist.
- Quality, latency and RAM have no C++ baseline yet; the first Arch run is
  pipeline evidence only (model load 1.8 s, deep warm-up 24 s, 2 s of audio
  transcribed in 15 s, WSL/NTFS sandbox).
- Every checked-in WAV fixture is sine or silence, so a transcript from them
  proves the pipeline, never the recognition quality.
- VAD mode uses the energy heuristic, not Silero. `ggml-silero-v6.2.0.bin` is
  installed but not bound to a native runtime, so steady background noise can
  fool the detector and it has no notion of a speaker.
- Autostart reconciles the HKCU Run entry against the setting. The test never
  writes the production value name: the installed .NET app's own entry is read
  before and after and must be byte-identical, and all test writes use a
  per-process value name that is removed again.
- Settings that no running service reads would be indistinguishable from working
  ones, so the UI tests assert that a value reached the service and not only the
  file. If you add a control, add that assertion.
