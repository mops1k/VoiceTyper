<p align="center">
  <img src="assets/icon-256.png" width="120" alt="VoiceTyper">
</p>

<h1 align="center">VoiceTyper</h1>

<p align="center"><b>Speech to text on a hotkey — entirely on your own computer.</b></p>

<p align="center">Press the hotkey, speak, and the text lands in the clipboard and pastes itself into the active field.</p>

<p align="center"><a href="README.ru.md">Русский</a> · <b>English</b></p>

---

## Why it is pleasant to use

- **Works offline.** Neither the audio nor the recognised text leaves your machine — everything is computed on your CPU.
- **No graphics card needed.** The engine runs on the CPU, so the application stays light and quiet.
- **Lives in the tray.** The settings window never gets in the way: hide it and keep dictating.
- **Fast.** The native C++ and Qt 6 build recognises noticeably faster than the earlier .NET version.
- **Russian first.** The interface and Russian speech recognition work out of the box; English is there too.

## Features

**Dictation**
- A global hotkey that works in any application, even when the window is not focused.
- Three recording modes: hold (push-to-talk), toggle, and automatic stop on silence with a configurable threshold.
- The recognised text is pasted into the active field — or you can turn that off and use the clipboard only.
- The combination is captured by clicking: press "Record" and type the gesture you want; Escape cancels.

**Recognition**
- Two engines to choose from: **Whisper** (models from Tiny to Large turbo) and **Parakeet v3** by NVIDIA — large-level quality at small speed.
- Recognition language: Russian, English, or detected automatically.
- Fine tuning: temperature, number of candidates, a dictionary of terms and names, and whether to condition on the previous text.
- **Background noise suppression** — the filter removes the rumble and damps quiet noise without squeezing a calm voice.

**Microphone**
- Device selection, a sensitivity slider, and a **microphone test** right in the settings: the application tells you whether it hears you and what it heard.
- Audio is captured directly through WASAPI, including Intel Smart Sound microphone arrays.

**Interface**
- Russian and English, light and dark themes (or follow the system), Inter typeface.
- A clear status: the overlay above your windows shows whether it is recording or recognising.
- A work log, start with Windows, and hiding the window when it loses focus.
- Settings save themselves — there is no Save button.

**Updates**
- The application checks for new versions and can download and install one: the previous version is removed, the app closes, installs the new one, and starts again.

## Engines and models

| Engine | Models | Size | What it is good at |
|---|---|---|---|
| **Whisper** (whisper.cpp) | Tiny, Base, Small, Medium, Large turbo — q8 quants | 42 MB … 834 MB | The classic choice, predictable quality |
| **Parakeet v3** (NVIDIA, 0.6B) | q4_k, q5_k, q6_k, q8_0 | 0.64 … 0.9 GB | Multilingual (25 languages, Russian included), very fast on the CPU |

The engine and the model are chosen in the application, next to their size and speed.

## Installation

1. Open the [latest release](https://github.com/mops1k/VoiceTyper/releases/latest).
2. Download `VoiceTyper-<version>-win64-Setup.exe` (about 17 MB).
3. Run it: it installs for **the current user**, no administrator rights needed. An earlier version, if present, is removed automatically.
4. The application starts after installation and stays in the tray.

To remove it, use the usual "Apps" page in Windows settings.

## Requirements

- Windows 10 or 11, 64-bit.
- An x64 processor. No graphics card needed.
- Disk space for a model: from 42 MB (Tiny) to about 1 GB (Parakeet q8).
- A microphone.

## First run

1. On the "Microphone" page pick your device and press "Test microphone" — make sure the indicator reacts to your voice.
2. On the "Models" page choose an engine and a model, and press "Download" in its row: the file is fetched into the models folder (from 42 MB to about 1 GB, depending on the model).
3. Press the hotkey (by default <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>Space</kbd>) and say a phrase — the text appears in the active field.

Any combination can be reassigned: the "Record" field in the settings captures the gesture itself.

## Not done yet

- Only the Windows version is published.

## Building from source

For development and builds: [docs/build.md](docs/build.md).

## License

MIT — see [LICENSE](LICENSE).
