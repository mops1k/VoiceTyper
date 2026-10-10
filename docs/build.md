# Building VoiceTyper from source

The application is C++20 with Qt 6 Widgets. This document is for development; what the
application does and how to install it is in [README.ru.md](../README.ru.md) /
[README.md](../README.md).

## What you need

- **Qt 6.9 or newer** with the Widgets and Network modules (development files).
- **MinGW-w64 13.1** on Windows, or any C++20 compiler on Linux.
- **CMake 3.21+** and **Ninja**.
- Python 3 — optional, only for the deterministic fixture tools under `tools/`.

The toolchain file `cmake/toolchains/mingw-x86_64.cmake` reads two environment variables:

```powershell
$env:VOICETYPER_QT_ROOT='C:\Qt\6.11.2\mingw_64'
$env:VOICETYPER_MINGW_ROOT='C:\Qt\Tools\mingw1310_64'
```

## Configure, build, run

```powershell
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release --target voicetyper-deploy
.\build\windows-mingw-release\deploy\voicetyper-qt-shell.exe
```

`voicetyper-deploy` produces the runnable tree: one executable plus the Qt runtime, the
platform plugin, the recognisers and `mc_wasapi.dll` (the native capture library, taken from
`native/`).

### Linux (Arch Linux, KDE Plasma 6, Wayland)

The Linux build uses the same sources; only the platform backends and the recogniser
libraries differ. Beyond the common list it needs:

- `libpulse` and `libpulse-simple` development packages — microphone enumeration, capture
  and the level meter.
- Qt 6 **DBus** module — the `org.kde.kglobalaccel` hotkey fallback for a session where
  `/dev/input` is not readable. Without it the build configures and simply stays evdev-only.
- Readable `/dev/input/event*` nodes — global hotkeys and gamepad buttons (the `input`
  group or a udev ACL).
- `ydotool` with its daemon running (`systemctl --user start ydotool`, the user unit) —
  automatic pasting; without it the application degrades to clipboard-only and says so in
  the log.

```bash
cmake --preset linux-arch-release
cmake --build --preset linux-arch-release --target voicetyper-qt-shell
# The recogniser shared objects are built into engine-libs; the loaders also look
# next to the executable, so a development build runs without copying anything.
./build/linux-arch-release/voicetyper-qt-shell
```

`linux-arch-gui-off` configures the portable window without the Qt backends (a headless
compile check). Paths follow XDG: settings in `$XDG_CONFIG_HOME/VoiceTyper`, models and logs
in `$XDG_DATA_HOME/VoiceTyper`, the autostart entry in `$XDG_CONFIG_HOME/autostart`.

The product version lives in one place — the `VOICETYPER_VERSION` CMake variable. It is what
the About page shows and what the update check compares against the release tag, so a release
build passes the tag: `cmake --preset windows-mingw-release "-DVOICETYPER_VERSION=2.0.1"`.

## Tests

```powershell
cmake --build --preset windows-mingw-release --target voicetyper-tests
ctest --test-dir build/windows-mingw-release --output-on-failure
```

Every other target is `EXCLUDE_FROM_ALL`, so a normal build compiles the application and
nothing else; `voicetyper-tests` builds all the test executables at once. The suite covers the
contracts with the .NET behaviour (settings JSON byte for byte, hotkey grammar, update
manifest, capture guard, microphone level), the recording state machine, the update service
and the settings window itself.

On Linux the same commands work with the Linux preset:

```bash
cmake --build --preset linux-arch-release --target voicetyper-tests
ctest --test-dir build/linux-arch-release --output-on-failure
```

The Linux contract tests are `linux-platform-contract` (XDG paths, `/proc/self/exe`,
executor, freedesktop autostart), `linux-clipboard-contract`, `linux-audio-contract`
(libpulse), `linux-hotkeys-contract` (evdev key map and capture grammar),
`linux-kglobalaccel-contract` (the WPF→Qt key map, the unreachable-bus refusal and the
press-only release model; a real registration runs only with
`VOICETYPER_KGLOBALACCEL_LIVE=1`), `linux-gamepad-contract` (the evdev poll loop through its
injected reader) and `linux-engines-contract` (`dlopen` search path). They never require the
`input` group or a sound server: a session without them skips explicitly instead of failing.

## Installer

The installer is Inno Setup 6 (`installer/installer-native.iss`), driven by

```powershell
pwsh installer/build-native-installer.ps1 -Version 2.0.1
```

It installs per user into `%LOCALAPPDATA%\Programs\VoiceTyper`, keeps the same AppId as the
earlier .NET installer (so a previous installation is removed first), closes the running
application through the mutex `Global\VoiceTyper_SingleInstance`, and starts it again after
the installation. The asset is named `VoiceTyper-<version>-win64-Setup.exe`: the `win64`
marker is what the application's updater requires before it runs a downloaded installer.

## Releases

`.github/workflows/release.yml` builds and publishes a release on every `v*` tag: it installs
the newest **published** Qt (6.9.3; 6.11 exists only as a local build) with MinGW 13.1, builds
the deploy tree and the installer, and attaches the Setup executable to the release. The
version comes from the tag.

## Where the code lives

```
src/domain/      settings, hotkey grammar, recording state machine, models, errors
src/core/        recognition, audio, VAD, update service, SHA-256, support
src/platform/    platform boundaries: ports and their Windows implementations
src/app/         Qt: window, pages, tray, status overlay, HTTP client, composition
native/          shipped native libraries (mc_wasapi.dll, parakeet.dll, transcribe/libtranscribe.dll + ggml) and their licences
tests/           contract and UI tests, plus the .NET-generated migration fixtures
installer/       Inno Setup script and the build script
tools/           diagnostics, UI snapshot, fixture generators
```
