# VoiceTyper

Local speech-to-text on hotkeys. Press a hotkey — speak — the text appears in the clipboard
and (optionally) is auto-pasted into the active field.

Works **fully offline** on CPU (whisper.cpp), no GPU, and no audio/text is sent anywhere —
everything is recognized locally.

---

## Features

### Recording and recognition
- **Global hotkeys** — work in any application, even without window focus.
- **Three recording modes**:
  - *Hold (Push-to-Talk)* — records while the key is held;
  - *Toggle* — press to start, press again to stop;
  - *Auto (VAD)* — automatic stop on silence (Silero VAD) + configurable silence threshold.
- **Hotkeys by click** — click the "Record"/"Cancel" field and press the desired combination; it
  is captured automatically (Escape — cancel).
- **Microphone capture** via a native WASAPI backend — supports built-in microphone arrays
  (e.g. Intel Smart Sound: **EXCLUSIVE + PCM16 48 kHz stereo**), with a fallback to NAudio
  WASAPI and MME.
- **Background noise suppression** (on/off) — a lightweight local filter (high-pass + adaptive
  noise floor) with no external models.
- **Auto-paste** (Ctrl+V) into the active field — toggled by a setting.

### Recognition (Whisper)
- **Russian + English** (multilingual model), automatic language detection.
- **Model selection** in a dedicated section — a list of models with description, speed, quality,
  size, "Download" / "Delete from disk" buttons and a mutually exclusive selector.
- **Technical terms dictionary** — mixed into the model's initial prompt, improving recognition of
  "API", "CPU", "JSON", etc.
- **Temperature** (0 — strict/deterministic … 0.8 — softer) — affects the "strictness" of
  recognition.
- **Use context** (for long speech) — use the text of the previous segment.
- **Model download progress and cancel** — in the status bar: progress bar, size/speed/time and a
  "Cancel" button; a partially downloaded file is cleaned up.
- **Capture is blocked** until the selected model is downloaded/loaded (to avoid freezes).

### Interface and settings
- **Settings window with a left menu**: General · Appearance · Models · Hotkeys ·
  Microphone · Startup · About.
- **Auto-save** — every change applies immediately (no "Save" button needed).
- **Theme**: Light / Dark / **Auto** (follows the Windows theme), including the window title bar
  (custom modern titlebar) and matching look of fields/buttons/toggles.
- **Modern toggle switches**.
- **Tooltips (ⓘ)** next to every setting with an explanation.
- **Status overlay** — a "Capturing"/"Recognizing" indicator at the bottom of the screen, above
  windows.
- **"Hide window on focus loss"** setting (disabled by default).
- **Tray**: minimizes into the background, opens settings on double-click of the icon; context menu.
- **Tray and taskbar icon** changes with the Windows theme (light/dark glyph).
- **Autostart** with Windows (optional).

---

## Requirements

- Windows 10/11 (11 recommended).
- CPU with **AVX/AVX2/FMA/F16C** support (otherwise use the `Whisper.net.Runtime.NoAvx` runtime).
- **Microsoft Visual C++ Redistributable 2015–2022 (x64)** — required by whisper.cpp.
- A microphone and permission to use it in Windows privacy settings.
- Internet on first launch (model download).

---

## Models

All models are **q8-quantized ggml** (Q8_0) — considerably smaller in memory and on disk than fp16, at virtually the same quality and speed.

| Size | File on disk | Speed | Quality | Comment |
|---|---|---|---|---|
| Tiny (q8) | ~42 MB | very fast | low | for simple tasks |
| Base (q8) | ~78 MB | fast | medium | a compromise |
| **Small (default)** (q8) | ~252 MB | medium | high | good RU/EN quality |
| Medium (q8) | ~785 MB | slow | very high | more accurate, slower |
| Large (turbo, q8) | ~834 MB | very slow | maximum | compact, for powerful CPUs |

Models are stored in `%LOCALAPPDATA%\VoiceTyper\models` and are downloaded once.
In the "Models" section they can be pre-downloaded and deleted from disk (to free up space).
On update, obsolete fp16 and q5 files are automatically removed from disk.

---

## Build (manual)

Requires the **.NET 10 SDK** ([download](https://dotnet.microsoft.com/download/dotnet/10.0)).

```powershell
# 1) Build the solution (Release)
dotnet build VoiceTyper.slnx -c Release

# 2) Run the tests
dotnet test VoiceTyper.Tests -c Release
```

### Run
```powershell
dotnet run --project VoiceTyper.App
```
or the built exe:
```powershell
VoiceTyper.App\bin\Release\net10.0-windows\VoiceTyper.exe
```

### Native capture library (mc_wasapi.dll)
The repository already contains a built `VoiceTyper.App\Native\mc_wasapi.dll` (WASAPI capture for
Intel Smart Sound, built with MinGW, statically linked — depends only on system
`KERNEL32/ole32/UCRT`). It is copied to the output automatically.

If this file is missing — the application still builds, but the native backend is skipped and
capture goes through NAudio WASAPI/MME (sufficient for ordinary microphones).

To rebuild `mc_wasapi.dll` from the `mc_wasapi.cpp` source (optional, requires MinGW-w64):
```powershell
g++ -std=c++17 -O2 -shared -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE `
    -I <mingw>\x86_64-w64-mingw32\include mc_wasapi.cpp -o mc_wasapi.dll -lole32 -luuid
```

### Publish (self-contained, without installed .NET)
```powershell
dotnet publish VoiceTyper.App -c Release -r win-x64 --self-contained true -o publish
```
Run `publish\VoiceTyper.exe`. Requires the VC++ Redistributable.

---

## Usage

1. Launch VoiceTyper. The settings window opens by default; the application also runs in the tray.
2. Settings: double-click the tray icon (or menu → "Open settings").
3. Defaults:
   - **Record/stop:** `Ctrl+Alt+Space`
   - **Cancel:** `Ctrl+Alt+Escape`
4. Focus the target field, press the record hotkey, speak, release/press again.
5. The text appears in the clipboard; with auto-paste enabled — directly in the input field.
6. All settings are saved automatically.

---

## Diagnostics

The application writes a detailed log to **`%LOCALAPPDATA%\VoiceTyper\logs\voiceTyper.log`**
(format `yyyy-MM-dd HH:mm:ss.fff [Level] message`, errors include a stack trace).
The log is cleared on each launch; at ~1 MB the file is rotated (up to 5 archives `voiceTyper.N.log`).
Logged: startup, settings, microphones, hotkeys, model downloads, recording states,
recognized text and all errors.

---

## Known limitations

- **Auto-paste does not work in windows with elevated privileges (UAC)** if VoiceTyper is launched
  without administrator rights. Fix: run both with the same privilege level.
- Quality depends on the model size and the microphone. For Russian, "Small" gives an acceptable
  result; for demanding scenarios use "Medium".
- A hotkey may conflict with system shortcuts — the application will show a notification;
  change the combination in settings.
- Noise suppression is a light filter (high-pass + adaptive noise floor). For stronger suppression
  it can be extended to RNNoise (separately).

---

## Solution structure

```
VoiceTyper.slnx
├── VoiceTyper.Core/     # logic without UI: settings, audio (NAudio + native WASAPI),
│                        # Whisper (Whisper.net), VAD, noise suppression, recording state machine
├── VoiceTyper.App/      # WPF: settings window (MVVM, section menu), tray, status overlay,
│                        # themes, global hotkeys (NHotkey.Wpf), Native/mc_wasapi.dll
└── VoiceTyper.Tests/    # xUnit tests
```

Settings: `%APPDATA%\VoiceTyper\settings.json`
Models: `%LOCALAPPDATA%\VoiceTyper\models`

---

## Main libraries

- [Whisper.net](https://github.com/sandrohanea/whisper.net) + [whisper.cpp](https://github.com/ggml-org/whisper.cpp) — CPU recognition
- [NAudio](https://github.com/naudio/NAudio) — audio capture
- [NHotkey.Wpf](https://github.com/thomaslevesque/NHotkey) — global hotkeys
- [WindowsInput](https://github.com/michaelnoonan/inputsimulator) — Ctrl+V simulation
- [CommunityToolkit.Mvvm](https://github.com/CommunityToolkit/dotnet) — MVVM
