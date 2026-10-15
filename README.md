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
- **No graphics card needed.** The engines run on the CPU, so the application stays light and quiet.
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
- Three engines to choose from: **Whisper** (models from Tiny to Large turbo), **Parakeet v3** by NVIDIA — large-level quality at small speed — and **GigaAM v3** by SberDevices/SaluteDevices (Russian only, punctuation and casing out of the box).
- Recognition language: Russian, English, or detected automatically.
- Fine tuning: a dictionary of terms and names with exact replacements (`commit=commit`), works with every engine, plus temperature for Whisper.
- **Filler words**: "uh", "um", "like", "you know", "so" and friends are removed from the recognised text at three levels — soft, strict and extreme — and your own words can be added on top.
- **Background noise suppression** — the filter removes the rumble and damps quiet noise without squeezing a calm voice.

**Text cleanup with a local model**
- The recognised phrase can be polished further by a small language model that runs **on your machine**: nothing is sent anywhere, and the internet is needed only to download the file.
- Three sizes: **Qwen2.5 0.5B** (≈491 MB, the fastest), **Qwen2.5 1.5B** (≈1.1 GB, noticeably better on long phrases) and **Llama 3.2 3B** (≈2 GB, the best quality).
- Downloaded straight from the settings, switched on the fly (the previous model is unloaded from memory) and deleted with one button.
- The built-in instruction can be **extended, not replaced**: add a requirement of your own (up to 400 bytes), for example "do not replace words with synonyms".
- Safety first: an answer is rejected if it loses a number or changes their order, switches language, loses a third of the meaning words of what you said, or talks about something else entirely — then the result of the rules above is used, so an error never reaches your text. The model cleans the phrase, it does not retell it: "перезапустил, выпускай" will not come back as "Запускай.".

**Microphone**
- Device selection, a sensitivity slider, and a **microphone test** right in the settings: the application tells you whether it hears you and what it heard.
- Audio is captured directly through WASAPI, including Intel Smart Sound microphone arrays.

**Interface**
- Russian and English, light and dark themes (or follow the system), one bundled typeface for every language — Source Sans 3 — and Fluent System Icons glyphs for the navigation: the same on Windows and Linux.
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
| **GigaAM v3** (SberDevices/SaluteDevices, e2e-rnnt) | q4_k_m, q5_k_m, q6_k, q8_0 | 184 … 274 MB | Russian only, punctuation and casing out of the box; a phrase longer than ~25 s is cut at a pause |

The engine and the model are chosen in the application, next to their size and speed.

## Installation

1. Open the [latest release](https://github.com/mops1k/VoiceTyper/releases/latest).
2. Download `VoiceTyper-<version>-win64-Setup.exe` (about 21 MB).
3. Run it: it installs for **the current user**, no administrator rights needed. An earlier version, if present, is removed automatically.
4. The application starts after installation and stays in the tray.

To remove it, use the usual "Apps" page in Windows settings.

On Linux every release also carries `VoiceTyper-<version>-x86_64.AppImage` (about 52 MB) with a
`.sha256` file: make it executable and run it — no installation, and the models are downloaded on
first use. The AppImage updates itself: the About page downloads the new image, verifies its
sha256, replaces the running file and restarts the application (a build that was not started from
an AppImage opens the release page instead). To build it (or the application) from source, see
[docs/build.md](docs/build.md).

## Requirements

- Windows 10 or 11, 64-bit, or Linux: Arch Linux with KDE Plasma 6 (X11 or Wayland), Qt 6.9+.
  The Linux build needs `libpulse`, the Qt 6 DBus module, readable `/dev/input/event*` for the
  global hotkeys and `ydotool` for automatic pasting — see [docs/build.md](docs/build.md).
- An x64 processor. No graphics card needed.
- Disk space for a model: from 42 MB (Whisper Tiny) to about 2 GB (the largest text-cleanup model).
- A microphone.

## First run

1. On the "Microphone" page pick your device and press "Test microphone" — make sure the indicator reacts to your voice.
2. On the "Models" page choose an engine and a model, and press "Download" in its row: the file is fetched into the models folder (from 42 MB to about 1 GB, depending on the model).
3. On the "Recognition" page choose how hard the filler words are cleaned and add your own words if you want; the local cleanup model on the same page is optional.
4. Press the hotkey (by default <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>Space</kbd>) and say a phrase — the text appears in the active field.

Any combination can be reassigned: the "Record" field in the settings captures the gesture itself.

## Not done yet

- The Linux build (Arch/KDE Plasma 6, Wayland and X11) ships as an AppImage; a distribution
  package (pacman/AUR, Flatpak) is not done yet.

## Building from source

For development and builds: [docs/build.md](docs/build.md).

## License

MIT — see [LICENSE](LICENSE).
