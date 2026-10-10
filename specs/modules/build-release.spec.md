# VoiceTyper — Build, Release and Test Specification

**Covers:** `CMakeLists.txt`, `CMakePresets.json`, `cmake/` (toolchain, native dependency
verification, portable header audit), `installer/`, `.github/workflows/`, `tests/`, `tools/`,
`assets/`, `packaging/`, `dist/`.
**Baseline:** `main` @ `3cc4c5a9a602afb96c2f478a562005f5eb850068` (v2.2.1).
**Requirement keywords:** RFC 2119. Parent document: [index.spec.md](../index.spec.md).
**Prefix:** `VT-BLD-*`.

---

## 1. Project definition (`VT-BLD-1xx`)

- **VT-BLD-101.** The build MUST require CMake **3.28+**, C++20 with extensions off, and produce
  the project `VoiceTyperCpp` ([CMakeLists.txt:1-7](../../CMakeLists.txt#L1-L7)).
- **VT-BLD-102.** The three options MUST be:

| Option | Default | Meaning |
|---|---|---|
| `VOICETYPER_BUILD_GUI` | `ON` | build the Qt Widgets shell |
| `VOICETYPER_BUILD_TESTS` | `ON` | register the CTest suite |
| `VOICETYPER_BUILD_ASR` | `${VOICETYPER_BUILD_GUI}` | build the pinned whisper.cpp dependency (needs the network once) |

- **VT-BLD-103.** The product version MUST live in exactly one place, the cache variable
  `VOICETYPER_VERSION` (currently `3.0.0`), injected as a compile definition into
  `voicetyper_domain` only, with the source fallback `"0.0.0-dev"`. It is what the About page
  shows and what the update check compares against the release tag, so a release build MUST pass
  the tag (`-DVOICETYPER_VERSION=<tag>`).
- **VT-BLD-104.** The Qt floor MUST be `6.9` (`find_package(Qt6 6.9 REQUIRED COMPONENTS Core Gui
  Widgets Network)`), not an exact version: CI installs the newest published Qt (6.9.3) while the
  developer machine has 6.11.2, and an `EXACT` requirement broke the release build.
- **VT-BLD-105.** Python 3 MUST be optional; when absent the `fixture-check` and
  `differential-compare-smoke` CTest entries MUST be omitted with an explicit configure status
  message rather than silently skipped.
- **VT-BLD-106.** Warnings MUST be per target: `/W4 /permissive-` on MSVC, otherwise
  `-Wall -Wextra -Wpedantic`.

---

## 2. Targets (`VT-BLD-2xx`)

- **VT-BLD-201.** `voicetyper_platform` MUST be a header-only `INTERFACE` target that links
  nothing (no Qt, no OS SDK, no third-party library).
- **VT-BLD-202.** Each layer MUST be its own static library so a test can link the real code:

| Target | Sources / purpose |
|---|---|
| `voicetyper_domain` | portable core + version injection |
| `voicetyper_settings` | settings JSON codec |
| `voicetyper_core_support` | app paths, file logger, CPU topology |
| `voicetyper_audio` | WAV I/O, DSP, trimming |
| `voicetyper_model_download` | model downloader |
| `voicetyper_vad` | segmenters, trim/chunk policy |
| `voicetyper_terms` | terms dictionary |
| `voicetyper_state` | recording state machine |
| `voicetyper_presenter` | settings presenter |
| `voicetyper_output` | text output |
| `voicetyper_asr` | engine-independent request mapping (no Qt, no native lib) |
| `voicetyper_asr_registry` | concrete registry with injected factories |
| `voicetyper_asr_lifecycle` | engine host (epochs, background load, readiness) |
| `voicetyper_capture_guard` | microphone release fuse (`dbghelp` on Windows) |
| `voicetyper_microphone_level` | input level/mute seam |
| `voicetyper_model_store` | catalog + download policy |
| `voicetyper_parakeet_runtime` | binding to the shipped `parakeet.dll` (built on every platform) |
| `voicetyper_transcribe_runtime` | binding to `libtranscribe.dll` (private include dir `native/transcribe`) |
| `voicetyper_gigaam` | GigaAM engine |
| `voicetyper_asr_whisper` (ASR only) | thin wrapper; the only TU including the real `whisper.h` |
| `voicetyper_asr_silero` (ASR only) | Silero VAD over the same whisper library |
| `voicetyper_asr_native` (ASR only) | the real engines + concrete registry |
| `voicetyper_portable_runtime` | portable fallbacks for UI seams |
| `voicetyper_update` (GUI) | release manifest + launcher bytes (QtCore only) |
| `voicetyper_ui` (GUI) | window, overlay, tray, fonts, HTTP client |
| `voicetyper_audio_capture` (WIN32) | WASAPI capture + WindowsMicrophone + `mc_wasapi.cpp` |
| `voicetyper_platform_win32` (WIN32) | clock, clipboard, executor, filesystem, logger, paste |
| `voicetyper_platform_win32_input` (WIN32) | hotkeys + low-level keyboard hook + XInput gamepad |
| `voicetyper_platform_win32_startup` (WIN32) | HKCU Run autostart |
| `voicetyper_platform_win32_update_launcher` (WIN32+GUI) | writes and starts `run-update.cmd` |
| `voicetyper_platform_linux` (UNIX AND NOT APPLE) | Qt-free Linux backends: paths, executor, startup, keymap/hotkeys, paste, microphone, level, audio capture |
| `voicetyper_platform_linux_gui` | the Qt-dependent part (clipboard over `QClipboard`) |
| `voicetyper_platform_linux_audio` | libpulse (`pkg-config libpulse-simple`) |
| `voicetyper-linux-engine-libs` | aggregate: `voicetyper_parakeet_cpp` + `voicetyper_transcribe_cpp` (ExternalProject) |

- **VT-BLD-203.** The executable MUST be `voicetyper-qt-shell` (`qt_add_executable ... WIN32`) from
  `src/app/main.cpp` plus, on Windows, `src/app/windows_application.cpp`, linking the real engines
  and backends only on `WIN32 AND VOICETYPER_BUILD_ASR`.
- **VT-BLD-204.** The product icon and the bundled Selawik family MUST travel inside the executable
  as a Qt resource (`assets/voiceTyper.png`, `spin-up.png`, `spin-down.png`, three Selawik faces at
  `:/fonts/Selawik-*.ttf`, VT-UI-1005),
  and the executable MUST additionally carry `assets/voiceTyper.rc` on Windows for the file icon.
- **VT-BLD-205.** `voicetyper-diagnostics` MUST exist as an `EXCLUDE_FROM_ALL` tool linking the
  domain, and the ASR-only smoke/probe executables (`voicetyper-asr-native-smoke`,
  `voicetyper-asr-whisper-probe`) and `voicetyper-mme-probe` MUST be available on their platform.
- **VT-BLD-206.** **Every test and tool MUST be `EXCLUDE_FROM_ALL`.** A plain build MUST produce
  exactly the application (plus `voicetyper-deploy`), not two dozen test executables; the tests are
  built on demand through the aggregate target `voicetyper-tests`.
- **VT-BLD-207.** `cmake --build <dir> --target voicetyper-tests` MUST build every registered test
  executable, and CI MUST run it before `ctest` (a plain build otherwise leaves every test
  "Not Run").
- **VT-BLD-208.** On Linux the composition MUST be `src/app/linux_application.cpp`, selected by
  `VOICETYPER_HAS_LINUX_COMPOSITION` in `src/app/main.cpp`; it MUST build the same window, tray,
  hotkey service, capture, engines, clipboard and paste as the Windows composition, and MUST
  create the XDG directories (`settings`, `models`, `logs`, `updates`) before the first write.
- **VT-BLD-209.** The Linux self-update action MUST download the release's AppImage and replace the
  running image (VT-SYS-014) instead of downloading an installer, and MUST fall back to opening the
  release page (`QDesktopServices::openUrl`) when the process was not started from an AppImage.
- **VT-BLD-209a.** The update query MUST select the asset the running platform can use: the Inno
  Setup installer on Windows (`^VoiceTyper-\d[^/]*?-Setup\.exe$`) and the AppImage on Linux
  (`^VoiceTyper-\d[^/]*?-x86_64\.AppImage$`). The expected `sha256` MUST come from the release
  asset `digest` (`sha256:<64 hex>`) when GitHub publishes it, and from the `SHA256:` marker in the
  release body otherwise (the .NET-compatible fallback).
- **VT-BLD-209b.** Replacing the AppImage MUST be atomic: the download goes to `<image>.download`
  next to the running image, the file is made executable (`0755`) and renamed over the image; a
  failed download or a failed rename MUST remove the partial file and MUST leave the running image
  untouched (`src/core/support/appimage_update.hpp`, contract `appimage-update-contract`).
- **VT-BLD-210.** A Linux build with `VOICETYPER_BUILD_ASR=ON` MUST link the real engines and MUST
  NOT require the shipped Windows DLLs; the native pin audit of VT-BLD-301 stays Windows-only.
- **VT-BLD-211.** A Linux release MUST publish `VoiceTyper-<version>-x86_64.AppImage` next to the
  Windows installer, with a `.sha256` file beside it, built by
  `packaging/appimage/build-appimage.sh` from the `runtime` install component. That component MUST
  contain the application, the desktop entry, the icon and the engine shared objects
  (`libparakeet.so`, `libtranscribe.so`, `libggml*.so`) beside the executable, and the AppImage
  MUST additionally carry the Qt platform plugins for Wayland **and** X11
  (`libqwayland.so`, `libqxcb.so`) plus LayerShellQt and libpulse. The diagnostics tool is a
  developer instrument and MUST live in its own `diagnostics` component, so the runtime install
  neither ships it nor fails when a release job did not build it.
- **VT-BLD-212.** The AppImage MUST NOT contain the Qt modules the application never loads (Qml,
  Quick, Pdf, PrintSupport), the Qt Virtual Keyboard plugin, the KDE image-format plugins or the
  network-information plugins; `packaging/appimage/prune-appdir.py` enforces that from the
  dependency graph. Models and `ydotool` MUST NOT be bundled: the application downloads models
  into `$XDG_DATA_HOME/VoiceTyper/models`, and ydotool is a system daemon (without it the
  application degrades to clipboard-only, which the log states).
- **VT-BLD-213.** The AppImage job MUST run in an Arch container on a hosted runner (the project
  needs Qt 6.9+, which Ubuntu 24.04 does not ship) and MUST publish the file to the same GitHub
  release as the Windows installer.
- **VT-BLD-214.** A full `cmake --install` of an ASR build (the CI install-smoke step, and the
  AppImage job's install step) MUST succeed without selecting a component. The vendored
  whisper.cpp contributes install rules for a `parakeet` target the build never creates, so its
  subdirectory MUST be declared with `EXCLUDE_FROM_ALL` in `FetchContent_Declare(whisper_cpp ...)`
  (`cmake/NativeAsr.cmake`): that removes both the subdirectory's install rules and the parent's
  `include` of its install script. `CMAKE_SKIP_INSTALL_RULES` MUST NOT be used for this — it stops
  the script from being generated while the parent keeps including it, and every install then
  fails with `include could not find requested file` (broken release run 2026-10-11).

---

## 3. Pinned dependencies and portable-header audit (`VT-BLD-3xx`)

- **VT-BLD-301.** Configuration MUST verify the native dependency pins **before** any target is
  built: `voicetyper_verify_native_pins()`, `voicetyper_verify_shipped_dll()` for
  `native/parakeet.dll`, `native/mc_wasapi.dll` and `native/transcribe/libtranscribe.dll`, and the
  manifest-artifact lookups that export the expected hashes.
- **VT-BLD-302.** The machine-readable source of truth MUST be
  `docs/migration/cpp/native-dependencies.json`, read by `cmake/NativeAsr.cmake`; the Parakeet
  pin and ABI MUST be cross-checked against `src/platform/api/engine_registry.hpp`.
- **VT-BLD-303.** whisper.cpp MUST be fetched from the commit archive with `URL_HASH` (not a git
  clone: CMake documents `GIT_SHALLOW` as incompatible with a hash `GIT_TAG`), its commit MUST be
  validated in the URL, and three files inside MUST be re-hashed against independent pins. A
  configure MAY instead use `VOICETYPER_WHISPER_PREFETCHED_DIR` with the same content pins.
- **VT-BLD-304.** The vendored whisper/ggml sub-build MUST be forced offline and minimal:
  `BUILD_SHARED_LIBS=OFF`, tests/examples/server/tools off, `WHISPER_CURL=OFF`, `GGML_NATIVE=OFF`,
  `GGML_OPENMP=OFF`, all accelerators off, `GGML_CPU_ALL_VARIANTS=OFF`, with the generic x86-64
  AVX2 baseline pinned because `CMAKE_CROSSCOMPILING` is true for any toolchain-file build.
- **VT-BLD-305.** The portable-header audit MUST run at configure time
  (`voicetyper_check_portable_headers`) and again as the `portable-headers` CTest, so a header that
  starts including `windows.h`, X11/ALSA or Qt fails the suite.
- **VT-BLD-306.** The `_WIN32_WINNT=0x0601` deviation for the vendored targets MUST be recorded and
  overridable (`-DVOICETYPER_WHISPER_WINDOWS_API_LEVEL=default`).
- **VT-BLD-307.** The Linux engine libraries MUST be built as `ExternalProject` targets with
  pinned tags: `voicetyper_parakeet_cpp` MUST use `GIT_REPOSITORY
  https://github.com/mudler/parakeet.cpp.git` with `GIT_SUBMODULES third_party/ggml` (its
  `scripts/apply_ggml_patches.sh` requires a git checkout, so the codeload archive cannot be
  used), `PARAKEET_SHARED=ON`, `BUILD_SHARED_LIBS=OFF` and `CMAKE_POSITION_INDEPENDENT_CODE=ON`
  (static ggml, VT-ASR-823), and MUST copy `libparakeet.so` in `INSTALL_COMMAND` because the
  project has no install rules. `voicetyper_transcribe_cpp` MUST use the codeload archive with
  `URL_HASH` (`TRANSCRIBE_BUILD_SHARED=ON`, `TRANSCRIBE_INSTALL=ON`, tests/examples/tools off,
  system BLAS off, Vulkan off). Both MUST land in `<binary_dir>/engine-libs`.
- **VT-BLD-308.** The `voicetyper-linux-engine-libs` target MUST depend on both projects so a
  single build produces every shared library the runtime loaders look for.

---

## 4. Presets and toolchain (`VT-BLD-4xx`)

- **VT-BLD-401.** Configure/build/test presets MUST include:
  `linux-arch-debug`, `linux-arch-release`, `linux-arch-debug-asan`, `linux-arch-debug-ubsan`,
  `linux-arch-debug-tsan`, `linux-arch-native-gui-off` (Release, GUI off, ASR on),
  `windows-mingw-debug`, `windows-mingw-release`.
- **VT-BLD-402.** `VOICETYPER_BUILD_GUI=OFF` MUST produce a build that does not see Qt at all and
  stays offline (`VOICETYPER_BUILD_ASR` on only when explicitly requested).
- **VT-BLD-403.** Sanitizer presets MUST halt on error
  (`ASAN_OPTIONS`/`UBSAN_OPTIONS`/`TSAN_OPTIONS` with halt/abort on error).
- **VT-BLD-404.** The Windows test preset MUST export `PATH` with the Qt `bin` first,
  `QT_PLUGIN_PATH` and `QT_QPA_PLATFORM=offscreen`.
- **VT-BLD-405.** The MinGW toolchain file MUST require `VOICETYPER_MINGW_ROOT` (or `MINGW_ROOT`)
  and verify `gcc/g++/windres`, and MUST require `VOICETYPER_QT_ROOT` (or `-DQt6_DIR`) containing
  `lib/cmake/Qt6/Qt6Config.cmake`; it MUST print the compiler version.
- **VT-BLD-406.** On Windows the MinGW 13.1 `bin` directory MUST precede the WinLibs `bin` on
  `PATH`; the inverted order makes GCC-13-linked executables load a newer `libstdc++-6.dll` and
  die with `0xC0000139 STATUS_ENTRYPOINT_NOT_FOUND`, which looks like a tree regression. This MUST
  be stated in the smoke procedure (VT-BLD-607).

---

## 5. Deploy tree, installer and release (`VT-BLD-5xx`)

### Deploy tree

- **VT-BLD-501.** `voicetyper-deploy` (Windows + GUI) MUST write a runnable tree into
  `VOICETYPER_DEPLOY_DIR` (default `<build>/deploy`) containing:
  1. `voicetyper-qt-shell.exe`;
  2. `mc_wasapi.dll` copied from the tracked `native/` location the manifest verifies (`dist/` is
     a build output and is not in the repository);
  3. Qt6 `Core/Gui/Widgets/Network` DLLs from `VOICETYPER_QT_ROOT/bin`;
  4. `platforms/qwindows.dll`;
  5. `tls/qschannelbackend.dll` and `tls/qcertonlybackend.dll` (required for every HTTPS request;
     the OpenSSL backend is deliberately excluded because the product does not ship
     `libssl`/`libcrypto`);
  6. the MinGW 13 runtime `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll` from
     `VOICETYPER_MINGW_ROOT`, never the WinLibs copies;
  7. `native/parakeet.dll`;
  8. `transcribe/` with `libtranscribe.dll`, `ggml.dll`, `ggml-base.dll`, `ggml-cpu.dll`.
- **VT-BLD-502.** With `VOICETYPER_QT_ROOT` or `VOICETYPER_MINGW_ROOT` missing, deploy support MUST
  be reported OFF instead of producing a broken tree.
- **VT-BLD-503.** `install()` MUST install the diagnostics tool and (with the GUI) the shell into
  `bin`.

### Installer

- **VT-BLD-510.** The installer MUST be Inno Setup 6 (`installer/installer-native.iss`), driven by
  `installer/build-native-installer.ps1 [-Version X] [-Preset windows-mingw-release]`.
- **VT-BLD-511.** Installation MUST be **per user**:
  `DefaultDirName={localappdata}\Programs\VoiceTyper`, `PrivilegesRequired=lowest`,
  `PrivilegesRequiredOverridesAllowed=dialog`, `ArchitecturesAllowed=x64compatible`.
- **VT-BLD-512.** The AppId MUST be the same as the legacy .NET installer
  (`{{9F22F58D-8CFB-4E7C-9D85-0B6B12D9A5E0}`), so an earlier installation is removed first.
- **VT-BLD-513.** `AppMutex` MUST be `Global\VoiceTyper_SingleInstance` (the same name the
  application holds), with `CloseApplications=yes` and `RestartApplications=no`.
- **VT-BLD-514.** `OutputBaseFilename` MUST be `VoiceTyper-<version>-win64-Setup`; the `win64`
  marker is what the application's updater checks before running a downloaded installer.
- **VT-BLD-515.** `[Files]` MUST install the shell as `{app}\VoiceTyper.exe` plus the rest of the
  deploy tree; `[Run]` MUST start the application post-install unless `/AutoUpdate` (the switch is
  detected by scanning the whole command tail, because Inno has no built-in
  `CmdLineParamExists`).
- **VT-BLD-516.** Settings and models MUST live outside `{app}` (`%LOCALAPPDATA%\VoiceTyper`), so
  an upgrade never touches them.
- **VT-BLD-517.** The installer language MUST remain Russian-only.

### Release pipeline

- **VT-BLD-520.** `.github/workflows/release.yml` MUST trigger on `v*` tags (and manual dispatch
  with a version), derive the version from the tag without the leading `v`, install Qt 6.9.3 with
  MinGW 13.1, install Inno Setup, configure with `cmake --preset windows-mingw-release
  "-DVOICETYPER_VERSION=<tag>"` (the quotes are load-bearing: PowerShell otherwise splits a
  version such as `2.0.0`), build `voicetyper-deploy`, run the installer script, and publish
  exactly `build/windows-mingw-release/VoiceTyper-<version>-win64-Setup.exe` with generated
  release notes.
- **VT-BLD-521.** `.github/workflows/cpp-spike.yml` MUST build on Windows with the same preset and
  explicitly build `voicetyper-tests` before running `ctest`; the Arch job MUST be manual-only.
- **VT-BLD-522.** A release MUST be reproducible from the tag: the tag version reaches the binary
  through `VOICETYPER_VERSION`, so the About page and the update check always agree with the
  release.
- **VT-BLD-523.** The build directory MUST NOT be a deliverable: `dist/` is stale, untracked
  legacy output and `packaging/` is an empty placeholder; the deliverables are
  `build/<preset>/deploy` and the Setup executable.

---

## 6. Test suite (`VT-BLD-6xx`)

- **VT-BLD-601.** Every contract test MUST print a greppable success banner (mostly
  `"<name>: OK"`) that CTest matches with `PASS_REGULAR_EXPRESSION`; the UI tests use
  `ui-settings-test: result=passed` / `ui-status-overlay-test: result=passed`.
- **VT-BLD-602.** Hardware-dependent and environment-dependent tests MUST skip explicitly rather
  than pass: `windows-audio-capture-contract` uses `SKIP_RETURN_CODE 77`,
  `asr-native-smoke` skips with a reason and exit 0 without `VOICETYPER_WHISPER_MODEL` /
  `VOICETYPER_GIGAAM_MODEL` / fixture, `silero-vad-contract` skips its model-dependent checks
  without `VOICETYPER_VAD_MODEL`, and `native-dependency-contract` treats an absent shipped DLL in
  a source-only checkout as SKIP, never as a pass. A green suite therefore does not prove capture,
  real inference or real VAD in a given environment.
- **VT-BLD-603.** The suite MUST cover at least: settings JSON defaults/all-fields/legacy/numeric
  enums/round-trip/atomic failure; the settings presenter debounce; the recording state machine;
  VAD and Silero VAD; speech segments; silence trimming; the terms dictionary; text output; audio
  DSP and noise suppression; core support (paths, Windows roots, logger, CPU); capture guard;
  microphone level; model store and model download; update manifest and update service; the ASR
  parameters and lifecycle; the native engine registry; the GigaAM engine; the whisper native pin;
  the native dependency pin audit; the platform contract smoke; the executor contract; both UI
  suites; the diagnostics CLI; and the Python fixture/differential checks.
- **VT-BLD-604.** The diagnostics CLI tests MUST derive the expected version from
  `VOICETYPER_VERSION` (a literal version string in a test becomes stale and broke the suite at
  v2.0.0).
- **VT-BLD-605.** The fixture check MUST verify every materialized fixture in
  `tests/fixtures/migration/manifest.json` by byte count and SHA-256 and validate JSON and the WAV
  container (`RIFF size + 8 == file size`, chunk padding `size + (size & 1)`, `fmt` + `data`
  present, declared `audioFormat`).
- **VT-BLD-606.** There MUST be no test or script that downloads a model or loads a native model,
  because that would break the offline contract (VT-ASR-804).
- **VT-BLD-606a.** On Linux the suite MUST include the seven Linux contract tests —
  `linux-platform-contract` (paths, `/proc/self/exe`, executor, freedesktop autostart),
  `linux-clipboard-contract`, `linux-audio-contract` (libpulse, skipping with 77 when there is no
  sound server), `linux-hotkeys-contract` (evdev key map and capture grammar, skipping without
  readable devices), `linux-kglobalaccel-contract` (the WPF→Qt key map, the unreachable-bus
  refusal, the press-only release model; the real registration runs only with
  `VOICETYPER_KGLOBALACCEL_LIVE=1`), `linux-gamepad-contract` (the evdev poll loop through the
  injected reader seam) and `linux-engines-contract` (the `dlopen` search path and the
  `native_library_missing` diagnostic) — plus the shared UI suites.
- **VT-BLD-606b.** The Linux contract tests MUST NOT require the `input` group or a running sound
  server: they skip explicitly (VT-BLD-602), so the suite stays green on a CI container.
- **VT-BLD-606c.** The Linux job of the C++ Spike workflow MUST run on a hosted runner inside an
  Arch container (`ubuntu-latest` + `archlinux:base-devel`) and MUST run on every push — not
  `workflow_dispatch`-only, not `continue-on-error`, and not dependent on a self-hosted runner.
  It MUST configure, build, build and run the tests with `QT_QPA_PLATFORM=offscreen` (a container
  has no display) and run the install smoke without selecting a component (VT-BLD-214).

### Manual Windows smoke

- **VT-BLD-607.** The physical procedure in `docs/migration/cpp/windows-smoke.md` MUST be followed
  for release acceptance and MUST include: the MinGW-13-before-WinLibs `PATH` rule, the prefetched
  whisper directory when the Windows build cannot verify github.com, the runtime DLLs beside the
  executable, the `--selftest` JSON start proof, and the scenarios with their parity rows.
- **VT-BLD-608.** The following MUST be verified on real hardware and MUST NOT be replaced by a
  unit test: microphone capture (including the Intel Smart Sound array), hotkey registration and
  capture, VAD behaviour, both engines end to end, model switch/dispose, clipboard/paste, tray and
  overlay over a focused window, settings persistence, update, and clean install/upgrade/uninstall.
- **VT-BLD-609.** `--selftest` MUST be the automated start proof and MUST NOT write the Run value
  or take the single-instance lock.
- **VT-BLD-610.** The release gates from `docs/migration/cpp/release-gates.md` MUST be recorded as
  proposals (not accepted budgets) with the measurement method: same machine/power plan/microphone/
  fixtures, ≥5 cold and 20 warm launches, 30 cycles per mode, p50 **and** p95, CPU time, working
  set, peak private bytes, model-load time, package size. ASR-relevant budgets: p95 latency
  ≤ measured .NET × 1.25 and normalized WER/CER not worse by more than **0.02 absolute** on the
  frozen corpus — which currently does not exist (see [asr.spec.md](asr.spec.md) §G-6).
- **VT-BLD-611.** Arch Linux acceptance requires a physical Arch machine with KDE Plasma 6;
  WSL may compile and run portable unit tests only and MUST NOT be reported as acceptance.

---

## 7. Tools and assets (`VT-BLD-7xx`)

| Tool | Purpose |
|---|---|
| `tools/voicetyper-diagnostics.cpp` | `--help`, `--version`, `--json`, `--wav-info <file>`; usage errors exit 2 |
| `tools/ui_snapshot.cpp` | renders all settings pages to `page-<index>.png` offscreen (1220×800) for visual review |
| `tools/mme_probe.cpp` | scratch MME (`waveIn`) capture probe |
| `tools/compare-contract-json.py` | deterministic two-document JSON comparator with `--normalize-enums`; exit 1 on any difference |
| `tools/verify-migration-fixtures.py` | manifest-driven byte/hash/container verification |
| `tools/generate-wav-fixtures/` | writes the WAV input fixtures with `decimal.Decimal` sine recurrence for bit-exact determinism |
| `tools/generate-settings-fixtures/` | the .NET generator that wrote the settings fixtures with the real `SettingsService.JsonOptions` |
| `tools/build-parakeet-native.ps1` | rebuilds `parakeet.dll` from the pinned upstream, applies the local `cstdint` patch, writes `BUILD.txt` |
| `tools/generate-icons.ps1` | rebuilds `assets/icon-256.png` (README header) and the multi-frame `assets/voiceTyper.ico` from the master `assets/voiceTyper.png` |

- **VT-BLD-701.** `tools/compare-contract-json.py` MUST stay in step with the settings schema: it
  currently knows only `whisper|parakeet` for `transcriptionEngine` and would flag a GigaAM
  document as out of range (⚠ gap G-3).
- **VT-BLD-702.** The icon generator MUST take the current product artwork
  (`assets/voiceTyper.png`, 1536×1536) as its master, MUST write exactly two outputs —
  `assets/icon-256.png` (256×256, the README header) and `assets/voiceTyper.ico`
  (multi-frame: 256/64/48/32/16) — and MUST NOT overwrite the master, which is the Qt
  resource of the window and the tray. It MUST NOT reference the removed
  `VoiceTyper.App/` tree nor the obsolete root `icon.png` (the .NET-era blue tile of
  commit `385a0f2`). There are no light/dark icon variants: one artwork serves every size.
  The tool is Windows-only (`System.Drawing`).
- **VT-BLD-703.** The WAV fixture generator MUST NOT reimplement the .NET conversion; it writes
  inputs only.

---

## 8. Known gaps

- **G-1.** No CI job runs the real-model smoke; no checked-in speech corpus exists, so ASR quality
  and latency are unmeasured.
- **G-2.** `fixture-check` and `differential-compare-smoke` are absent from a machine without
  Python 3 (the configure message says so, but a green suite without them is weaker).
- **G-3.** The JSON comparator does not know `gigaam`.
- **G-4.** `docs/migration/cpp/windows-smoke.md` is stale on VAD ("energy heuristic, not Silero")
  and `parity-ledger.md` says "22 properties" while the schema has 23.
- **G-5.** `packaging/` is an empty placeholder and `dist/` holds stale untracked output; neither
  is part of the C++ delivery.
- **G-6.** The `windows-mingw-release` preset depends on environment variables
  (`VOICETYPER_QT_ROOT`, `VOICETYPER_MINGW_ROOT`) that a fresh checkout does not have; a
  hand-written `cmake` invocation without the toolchain fails with an explicit message, which is
  correct but not friendly.
- **G-7.** The Arch jobs are manual-only, so a portable regression can land without CI noticing.
- **G-8.** Icon assets are not verified by anything. Specifically: the shipped
  `assets/voiceTyper.ico` is a **single** 256×256 uncompressed (BMP) frame, while
  `tools/generate-icons.ps1` emits **five** PNG-compressed frames (256/64/48/32/16), so running
  the tool rewrites a tracked binary rather than reproducing it, and no test or CI step notices
  the difference; the root `icon.png` (213×212, the .NET-era blue tile) is now referenced by no
  pipeline at all; and the comment in `assets/voiceTyper.rc` claims the icon is "the icon the
  .NET build shipped, restored from the repository history", which contradicts the actual
  provenance (it was built from `assets/voiceTyper.png`) and the recorded owner's rule that the
  icon MUST NOT be taken from the repository history. Re-generating the shipped icon is therefore
  a separate, deliberate decision, not a side effect of running the tool.
