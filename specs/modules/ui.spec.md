# VoiceTyper — UI Module Specification

**Covers:** `src/app/` — entry points, the settings window (`main_window.*`), the tray
(`tray_controller.*`), the status overlay (`status_overlay.*`), localization (`ui_text.*`),
the settings presenter, the toggle switch, the fonts and the Qt HTTP client.
**Baseline:** `main` @ `3cc4c5a9a602afb96c2f478a562005f5eb850068` (v2.2.1).
**Requirement keywords:** RFC 2119. Parent document: [index.spec.md](../index.spec.md).
**Prefix:** `VT-UI-*`.

---

## 1. Entry points and lifecycle (`VT-UI-1xx`)

- **VT-UI-101.** Windows MUST enter through `voicetyper::app::run()` from
  [src/app/main.cpp:21-25](../../src/app/main.cpp#L21-L25) and assemble the real backends in
  `src/app/windows_application.cpp`. Non-Windows MUST build the portable shell
  ([src/app/main.cpp:52-88](../../src/app/main.cpp#L52-L88)): a window over portable
  fallbacks, no tray, no hotkeys, no engine, no single-instance.
- **VT-UI-102.** The portable shell's settings path MUST resolve in this order:
  `VOICETYPER_SETTINGS_PATH`, `$XDG_DATA_HOME/VoiceTyper/settings.json`,
  `$HOME/.local/share/VoiceTyper/settings.json`, `./settings.json`.
- **VT-UI-103.** `run()` MUST perform, in this order: argument parsing → `QApplication` → bundled
  font → product icon from the Qt resource `:/assets/voiceTyper.png` → single-instance claim →
  application name/version → `setQuitOnLastWindowClosed(false)` → `AppPaths` → `Win32Clock`,
  `Win32FileSystem`, `FileLogger` → filesystem write probe → settings load → autostart service →
  output chain → microphone enumeration and capture warm-up → overlay → VAD segmenter → engine
  registry and host → engine readiness timer → recording machine and decorators → hotkeys →
  `WindowServices` → Qt message handler → silent startup update check → autostart reconciliation →
  window/tray → `exec()`.
- **VT-UI-104.** Recognized arguments:
  `--selftest` (assemble every service, print JSON diagnostics, exit without `exec()`, take no
  single-instance lock and never write the Run value);
  `--start-minimized` (also accepted as `-start-minimized`; MUST be read **before**
  `QApplication` because it is the switch the autostart command line carries).
- **VT-UI-105.** Recognized environment overrides: `VOICETYPER_SETTINGS_PATH`,
  `VOICETYPER_LOG_DIR`, `VOICETYPER_MODELS_DIR`, `VOICETYPER_WHISPER_MODEL` (only while Whisper
  is selected), `VOICETYPER_START_PAGE=<pageKey>`, plus the diagnostic-only
  `VOICETYPER_VAD_MODEL` and `VOICETYPER_CRASH_SELFTEST` (test/diagnostic use).
- **VT-UI-106.** Before loading settings the composition MUST probe the filesystem with a
  temporary write in the log directory and, on failure, log
  `"filesystem backend is not functional; settings will NOT be saved"`.
- **VT-UI-107.** On first run (`used_defaults`) the UI language MUST be taken from
  `QLocale::system()` (Russian → `ru`, otherwise `en`) and MUST be persisted immediately.
- **VT-UI-108.** Graceful shutdown after `exec()` MUST run: log `"shutting down"` →
  `hotkeys.unregister_all()` → `machine.cancel()` → `engine_host.shutdown(5000 ms)` →
  `status_overlay.destroy()`. The window destructor MUST `save_now()` so debounced settings are
  flushed.
- **VT-UI-109.** `--selftest` MUST unregister hotkeys and shut the engine host down before
  exiting.
- **VT-UI-110.** If the system tray is unavailable the icon MUST NOT be shown; this state leaves
  the user without a way back to a hidden window other than launching the application again
  (⚠ gap G-3).
- **VT-UI-111.** The window MUST NOT define a `closeEvent`; the only close path is the title-bar
  button calling `hide()`, and Alt+F4 therefore hides the window because the process does not
  quit on the last window (⚠ gap G-4: not covered by a test).

### Single instance (see also VT-SYS-040…043)

- **VT-UI-112.** The gate MUST be `CreateMutexW` on `Global\VoiceTyper_SingleInstance` with
  `::SetLastError(ERROR_SUCCESS)` before the call, then a local-socket probe
  (`QLocalServer` name `VoiceTyper-single-instance`, `UserAccessOption`, 200 ms timeout), with
  the socket cleanup only after nobody answered.
- **VT-UI-113.** A second launch MUST write `"show"` to the channel and exit; the owner MUST
  bring its window to the front through an activation hook, and a request arriving before the
  window exists MUST be remembered and consumed afterwards.

### Autostart reconciliation

- **VT-UI-114.** On a real launch with `startWithWindows` enabled the composition MUST reconcile
  the Run value (`reconcile_on_launch = true`) and MUST NOT overwrite an entry belonging to
  another installation (it logs
  `"autostart entry belongs to another installation and was left alone"`).
- **VT-UI-115.** A settings change MUST reconcile with `reconcile_on_launch = false`, so toggling
  `startMinimized` rewrites the command line of an already registered entry.
- **VT-UI-116.** `start_hidden` MUST be `--start-minimized` OR `settings.start_minimized`, and
  the launch source MUST be written to the log.

---

## 2. Settings window: frame (`VT-UI-2xx`)

- **VT-UI-201.** The window MUST be a frameless `QMainWindow`
  (`Qt::Window | Qt::FramelessWindowHint`), sized `min(1220, 96% available) × min(800, 94%
  available)`, minimum `520 × qBound(460, 650, available.height() - 80)`, and centred on the
  screen.
- **VT-UI-202.** Construction order MUST be `build_tabs()` → `bind_settings_to_controls()` →
  `bind_microphone_controls()` → `bind_update_controls()` → `apply_theme()` →
  `refresh_status()` → `refresh_devices()`.
- **VT-UI-203.** The window MUST adopt a composition-provided `StatusChannel` when one is given
  and create its own otherwise; `StatusChannel::post()` MUST be thread-safe and a no-op after the
  window dies (it stores an `std::atomic<MainWindow*>` and posts a queued invocation).
- **VT-UI-204.** The window MUST run an autosave timer (100 ms, repeating) and a refresh timer
  (500 ms).
- **VT-UI-205.** The frame MUST contain: a title bar of fixed height **40 px** with the product
  name (12 px DemiBold) and exactly **two** buttons 46×40 (minimize, close, icons drawn
  geometrically); a sidebar of width **300** (minimum 210, resized as
  `qBound(180, width()*30/100, 300)`) with the brand block (34×34 icon, name, tagline) and the
  navigation list (spacing 3, 13 pt, `NoFocus`, accent bar 3×19, row height 43 px);
  a page header (`min/max-height 80 px`) with the breadcrumb «Настройки»/«Settings», a 23 px
  page title and the version chip `VoiceTyper <version>`; a `QStackedWidget` of pages; and a
  footer of fixed height **56 px** in the status bar.
- **VT-UI-206.** The footer MUST contain a status dot (7×7), a quiet prefix «Запись»/«Record», a
  status value, an engine dot (6×6) with the engine state, a hotkey label with chips plus the
  hotkey state, the record button and a `QSizeGrip`. The status text MUST use plain labels (no
  rich text): an HTML footer label created a `QTextDocument` owned by another thread and produced
  real Qt warnings/faults (memory `m_d1bde9449957`).
- **VT-UI-207.** Pages MUST be built first and then ordered by the `kNavigation` table; each page
  MUST be wrapped in a `QScrollArea` named `pageScroll_<key>` with horizontal scrolling off, the
  page named `page_<key>`, and the navigation item MUST carry the Russian title in `Qt::UserRole`,
  the glyph in `+1` and the page key in `+2`.
- **VT-UI-208.** The content column width MUST be `qBound(240, stackWidth - 72, 820)` and MUST be
  recomputed on resize.
- **VT-UI-209.** Window dragging MUST be allowed from the title bar only.
- **VT-UI-210.** `bring_to_front()` MUST clear the minimized state, `show()`, `raise()`,
  `activateWindow()` and `requestActivate()` (a bare `show()+raise()` left the window behind
  another application, commit `8a574c1`).

### The 8 pages

- **VT-UI-211.** The navigation MUST have exactly these 8 entries in this order:

| # | RU | EN | key |
|---|---|---|---|
| 0 | Общие | General | `Main` |
| 1 | Внешний вид | Appearance | `Appearance` |
| 2 | Модели | Models | `Models` |
| 3 | Хоткеи | Hotkeys | `Hotkeys` |
| 4 | Микрофон | Microphone | `Microphone` |
| 5 | Запуск | Startup | `Startup` |
| 6 | Журнал | Log | `Log` |
| 7 | О программе | About | `About` |

- **VT-UI-212.** **General** MUST offer: interface language (Русский/English), recognition
  language (Авто/Русский/English), recording mode (Push-to-talk/Toggle/VAD), auto-paste toggle,
  "consider previous text" toggle, and the VAD silence threshold spin box `300..10000` ms with
  the ` мс` suffix and a tooltip — plus a hint bar that substitutes the current record hotkey.
- **VT-UI-213.** **Appearance** MUST offer three theme tiles in the order system/light/dark, a
  hide-on-focus-loss toggle, and a reset button that restores `theme` and `hide_on_focus_loss`
  from `AppSettings::defaults()`.
- **VT-UI-214.** **Models** MUST offer an engine combo (Whisper/Parakeet/GigaAM, maximum width
  220), exactly one visible model list for the selected engine (Whisper 5 rows, Parakeet 4,
  GigaAM 4), hidden size combos that keep the values, a temperature spin box `0..1` step 0.05 and
  a "recognition candidates" spin box `1..8` shown **only** for Whisper, and the terms dictionary
  editor with its hint. Each model row MUST contain a toggle, the name, an "Активна" badge, meta
  (`%1 · Скорость: %2 · Качество: %3`), a description, a 34×34 action button and a 90 px progress
  bar with a separate percentage label.
- **VT-UI-215.** Model selection MUST be exclusive: selecting a row MUST set the hidden size combo
  for that engine and clear the other toggles under `QSignalBlocker`.
- **VT-UI-216.** A model row's buttons MUST follow the download state machine: not downloaded →
  download (disabled without the service), downloaded → delete (with a modal confirmation
  `QMessageBox`), downloading → cancel.
- **VT-UI-217.** **Hotkeys** MUST offer a read-only, non-focusable, no-selection record field and
  cancel field (minimum width 146, maximum 240) with a capture button each. The capture button
  MUST be disabled without `services.capture_hotkey` and MUST carry the explanatory tooltip;
  during capture it MUST be disabled and relabelled «Нажмите клавиши…», the field MUST show the
  waiting placeholder and the status line «Ожидание комбинации…».
- **VT-UI-218.** **Microphone** MUST offer the device combo (elided, empty data = default), the
  Windows-level sensitivity slider with a percentage label, the microphone test block (state,
  button, verdict, 22-band level meter), and the noise reduction toggle.
- **VT-UI-219.** **Startup** MUST offer start-with-Windows and start-minimized toggles plus the
  informational "runs in the background" card.
- **VT-UI-220.** **Log** MUST show the log tail in a read-only `QTextEdit` (minimum height 320,
  card minimum 430) rendered as time/level/message columns with a monospace font, a "Журнал
  очищен" empty state, and header buttons «Копировать» (copies the visible text) and «Очистить»
  which clears only the **view** — the log file MUST NOT be touched.
- **VT-UI-221.** **About** MUST offer the version chip, the description, the privacy note, three
  links (Licences, Privacy policy, Report an issue), and the update block: check button, status
  line, notes, install button and progress bar (the last three hidden until an update is found).

---

## 3. Behavior rules of the settings layer (`VT-UI-3xx`)

### Persistence

- **VT-UI-301.** Edits MUST reach the presenter immediately and the file MUST be written after
  **700 ms** of quiet (`kAutosaveDebounce`, `kSettingsAutosaveDebounceMs`); there MUST be no Save
  button.
- **VT-UI-302.** `SettingsPresenter::update(change, mutate)` MUST apply the mutation immediately,
  mark dirty, increase the dirty generation and restart the debounce.
- **VT-UI-303.** `flush()` MUST serialize and write atomically; on failure the dirty flag MUST
  stay set.
- **VT-UI-304.** A missing settings file MUST load defaults without a diagnostic; any other read
  failure MUST load defaults with an `error` diagnostic.
- **VT-UI-305.** Settings that affect services MUST be re-applied **once** after the debounce:
  `engine`, `model`, `hotkeys`, `microphone`, `behaviour` and `language` send
  `settings_applied`; `appearance` and `startup` do not.
- **VT-UI-306.** An interface language change MUST be the only edit applied in place without
  waiting for the debounce (`retranslate()`).
- **VT-UI-307.** The window MUST NOT check the return value of `flush()`; a failed settings write
  is therefore invisible to the user (⚠ gap G-5).

### Validation and availability

- **VT-UI-308.** Control ranges MUST be `silenceThresholdMs 300..10000`,
  `temperature 0..1` step 0.05, `bestOf 1..8`, microphone slider `0..100`.
- **VT-UI-309.** `AppSettings::validate()` MUST NOT be called from the UI path: a hand-edited
  out-of-range value in `settings.json` is written back verbatim and only the engine entry point
  clamps `best_of` (⚠ gap G-6).
- **VT-UI-310.** A control whose service is missing MUST be disabled with an explanatory tooltip,
  never silently inert:

| Missing service | Effect |
|---|---|
| `start_recording` | record button disabled, tooltip "engine not ready yet" |
| `capture_hotkey` | both capture buttons disabled with the capture-unavailable tooltip |
| `microphone_level_get`/`_set` | slider disabled, tooltip "the system does not allow level control", value "—" |
| `microphone_probe` | test button disabled |
| `microphones` | device combo disabled with a single "microphone unavailable on this platform" item |
| `update_check` | check button disabled |
| `update_install` | install button disabled |
| `model_download_cancel` while downloading | row button disabled with a tooltip |

- **VT-UI-311.** The device list MUST be rebuilt under `QSignalBlocker` so populating it is never
  mistaken for a user edit (VT-RULE-004).
- **VT-UI-312.** A remembered microphone that is not enumerated MUST be kept in settings and shown
  as `%1 (недоступен)`; choosing "По умолчанию" is the only explicit way to clear it.
- **VT-UI-313.** Controls MUST stay enabled during a recording session: nothing is locked while
  recording and the record button works as "Stop".
- **VT-UI-314.** Temperature and best-of rows MUST be visible only for Whisper, because Parakeet
  and GigaAM ignore those parameters (VT-ASR-111).

### Status, recording and the footer

- **VT-UI-320.** A recording-state change MUST be logged as `recording state=<idle|recording|
  processing>`, posted to the footer and reflected in the overlay (`recording`, `processing`,
  `idle`).
- **VT-UI-321.** A recording failure MUST be logged; `io_failure` MUST be shown as
  «микрофон недоступен: устройство отключено или занято» (the raw WASAPI detail stays in the
  log) and any other code as «ошибка: <code_name>»; the overlay MUST show the error state with
  the detail.
- **VT-UI-322.** On success the composition MUST log the trimming numbers and
  `"text delivered"` with the **length only** — never the transcript (VT-SYS-073).
- **VT-UI-323.** The record button and the footer MUST refresh on a 400 ms timer so a session
  that ended by itself (silence, hotkey, failure) returns the button to "Record".
- **VT-UI-324.** The engine dot MUST be green only when the engine is ready and red otherwise.
- **VT-UI-325.** A non-empty status message from a service MUST take priority over the summary
  line and MUST stay until it is explicitly cleared.
- **VT-UI-326.** The overlay's terminal engine states (`failed`, `unavailable`, `model_missing`)
  MUST be posted as the overlay error state.

---

## 4. Microphone test (`VT-UI-4xx`)

- **VT-UI-401.** The test MUST run its own capture session (a separate `WindowsAudioCapture` on
  the capture executor) and MUST NOT touch the recording machine's session: the native library
  allows one capture at a time.
- **VT-UI-402.** The test MUST run for at most **8 s**, publishing the live peak every 50 ms and
  logging `"microphone level"` every 0.5 s; cancellation MUST be possible at any time.
- **VT-UI-403.** The verdict MUST be "heard" when `peak > 0.005`, otherwise
  «ничего не слышно: проверьте, что микрофон подключён и уровень не на нуле»; the diagnostic
  detail MUST be appended only on failure.
- **VT-UI-404.** The meter MUST map the peak to a 60 dB scale and render 22 bands; the indicator
  timer MUST tick every 80 ms.
- **VT-UI-405.** Callbacks MUST hold a `QPointer<MainWindow>` and a probe generation counter; the
  generation MUST NOT be incremented on stop, so the still-running worker's verdict is accepted.
- **VT-UI-406.** Starting the test MUST send `paste.set_suspended(true)`-equivalent protection
  only for hotkey capture, not for the test (the test is read-only).

---

## 5. Model download UI (`VT-UI-5xx`)

- **VT-UI-501.** Starting a download MUST show the progress bar, set the caption to `0%`, clear
  the bar text (the separate percentage label is the readout) and set the status «Скачивание…».
- **VT-UI-502.** Progress, completion, cancellation and errors MUST be marshalled to the UI thread
  with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`; painting widgets from the download
  thread crashed the application (commit `eda7312`).
- **VT-UI-503.** A second click while downloading MUST cancel: the button becomes a red cancel
  icon, and the cancellation MUST be reported as «Загрузка отменена» (`ErrorCode::cancelled`),
  not as a connection error (commit `24d3b56`).
- **VT-UI-504.** On error the status MUST read «Не удалось скачать модель: <error>»; on success
  the row MUST become a delete action and the status MUST be cleared.
- **VT-UI-505.** The file name MUST be chosen from the (engine, size) pair, so a GigaAM row can
  never download or delete a Parakeet file.
- **VT-UI-506.** Deleting a model MUST ask «Удалить файл модели «%1» с диска?» and MUST do nothing
  on refusal.

---

## 6. Updates UI (`VT-UI-6xx`)

- **VT-UI-601.** The check button MUST be disabled during the request and show
  «Проверка обновлений…»; the answer MUST be delivered on the UI thread guarded by a `QPointer`.
- **VT-UI-602.** The three outcomes MUST render as: error → «Не удалось проверить обновления:
  <error>»; up to date → «Обновлений нет» with notes/progress/install row hidden; available →
  «Доступна версия %1» plus notes (the markdown `**` markers stripped) and the install button.
- **VT-UI-603.** Install MUST switch the bar to an indeterminate range while starting, then show
  0..100 progress, and on the `"done"` stage MUST show «Обновление установлено, приложение
  перезапустится».
- **VT-UI-604.** The install path MUST refuse an installer URL without the `win64` marker with the
  dedicated message (VT-SYS-085).
- **VT-UI-605.** The startup check MUST be silent: the result goes only to the log.

---

## 7. Tray (`VT-UI-7xx`)

- **VT-UI-701.** The tray menu MUST have exactly: «Открыть настройки» (separator after),
  «Записать»/«Остановить» (separator after), «Выход».
- **VT-UI-702.** Clicking the icon MUST handle `QSystemTrayIcon::Trigger` only and MUST result in
  `bring_to_front()`; double/middle clicks are not handled.
- **VT-UI-703.** The record item MUST invoke the real record button through a `weak_ptr` so a
  destroyed window cannot be touched.
- **VT-UI-704.** Quit MUST call `QCoreApplication::quit()`.
- **VT-UI-705.** The tray icon MUST come from the same Qt resource as the window; a drawn
  microphone is the fallback when the resource is unavailable.
- **VT-UI-706.** `set_language()` MUST rewrite all three items (the record item according to the
  current recording state), and it MUST be called at startup and on every language change.
- **VT-UI-707.** The menu style MUST include right padding on items so the text does not touch the
  right edge.
- **VT-UI-708.** ⚠ gap G-1: `set_recording()` / `set_status()` are **never called in production**,
  so the tooltip is empty and the menu item does not change to "Stop" for a recording started by
  hotkey. The requirement "the tray shows readiness and offers the right action" is therefore only
  partially met.

---

## 8. Status overlay (`VT-UI-8xx`)

The normative port requirements are in [platform.spec.md](platform.spec.md) §VT-PLT-6xx; the UI
consequences are:

- **VT-UI-801.** The overlay MUST be created before the recording machine and destroyed after it,
  so a queued worker state cannot create a window during shutdown.
- **VT-UI-802.** `set_language()` MUST store the language and repeat the current state with its
  detail, so a language switch does not reset what is shown.
- **VT-UI-803.** `set_theme()` MUST resolve `system` from the palette lightness and MUST style the
  pill itself, because the global application stylesheet would otherwise override its colors.
- **VT-UI-804.** The overlay MUST NOT show recognized text or a level meter, and a `post_state()`
  call after `destroy()` MUST be ignored.

---

## 9. Localization (`VT-UI-9xx`)

- **VT-UI-901.** Supported languages MUST be exactly `ru` and `en`; any other value MUST fall back
  to Russian.
- **VT-UI-902.** All user-facing strings MUST live in one table: the `UiKey` enum plus a
  `constexpr std::array<Entry, kCount>` with `ru`/`en` columns, guarded by a compile-time
  `static_assert(kTexts.size() == UiKey::kCount)`.
- **VT-UI-903.** The current language MUST be a global read by every UI lambda
  (`set_current_language` / `current_language`), so service callbacks that return text follow a
  language change without capturing the presenter.
- **VT-UI-904.** `retranslate()` MUST update the window in place without rebuilding it: labels and
  buttons via their `uiKey`/`uiTextKey` properties, tooltips via `uiTooltipKey`, the hotkey hint
  via `hotkeyHintKey`, the About paragraph via `aboutCopyKeys`, the version chip via
  `versionChipKey`, uppercase texts, the navigation entries (by `Qt::UserRole + 2`), the
  breadcrumb, the spin-box suffix, the combo item texts, the model card labels and the status.
- **VT-UI-905.** Tray and overlay MUST be retranslated through their own `set_language()`.
- **VT-UI-906.** Settings MUST store wire values only (`ru`/`en`, `system`/`light`/`dark`,
  `q4K`/`q8_0`, …), so a language change never rewrites the settings.
- **VT-UI-907.** Every key MUST be non-empty in both languages (covered by
  `every_ui_key_has_a_text_in_both_languages`).
- **VT-UI-908.** The model card labels MUST be bilingual; the file names themselves are not
  translated.
- **VT-UI-909.** ⚠ gap G-2: the footer text for the `processing` state uses
  `UiKey::k7` («Движок распознавания»/«Recognition engine») instead of `k78`
  («Распознавание»/«Recognizing»), so the footer reads "запись: Движок распознавания" while
  processing. No test covers this string; the specification must decide whether it is a defect.

---

## 10. Theming and fonts (`VT-UI-10xx`)

- **VT-UI-1001.** Themes MUST be `system | light | dark`; `system` MUST resolve through
  `QGuiApplication::styleHints()->colorScheme()`.
- **VT-UI-1002.** Applying a theme MUST set the global stylesheet, repaint the drawn icons, the
  toggle switches, the theme tiles and the launch card, and MUST push the theme to the overlay.
- **VT-UI-1003.** The palette roles MUST be those of the Win11-style palettes in the code:
  dark window/control/header `#202020`, hover/selection `#2D2D2D`, text `#FFFFFF`, muted
  `#C5C5C5`, border `#3D3D3D`, accent `#4CC2FF`; light window/control `#FFFFFF`, hover/selection
  `#E8EFF6`, text `#333333`, muted `#6A6A6A`, border `#EFEFEF`, accent `#0067C0`, header
  `#EFF4F9`.
- **VT-UI-1004.** Icons MUST be drawn geometrically rather than taken from a font: the
  Segoe MDL2 glyph variant does not render on the reference machine, and the font path clears the
  icons when the font is absent (⚠ gap G-7: `refresh_nav_icons()` currently overwrites the drawn
  icons in `apply_theme()`).
- **VT-UI-1005.** The bundled Inter family (Regular/Medium/SemiBold/Bold) MUST be registered from
  the Qt resource before any widget is created, and its resource paths MUST stay in step with
  `application_font.cpp`.
- **VT-UI-1006.** The executable icon (`assets/voiceTyper.ico` via `assets/voiceTyper.rc`) and the
  resource icon `:/assets/voiceTyper.png` MUST be the same product artwork, both derived from the
  single master `assets/voiceTyper.png`. The README header image `assets/icon-256.png` MUST be a
  256×256 rendering of that same master. `tools/generate-icons.ps1` is the tool that reproduces
  both derived files (see [build-release.spec.md](build-release.spec.md) §VT-BLD-702); it MUST NOT
  overwrite the master, and its output MUST NOT be sourced from the removed `VoiceTyper.App/` tree
  or from the obsolete root `icon.png`.

---

## 11. Qt HTTP client (`VT-UI-11xx`)

- **VT-UI-1101.** `QtHttpClient` MUST implement `platform::HttpClient` over `QNetworkAccessManager`
  and MUST be blocking; it MUST be called only from a worker thread that has an event loop.
- **VT-UI-1102.** It MUST apply `NoLessSafeRedirectPolicy`, MUST clear the Qt default User-Agent
  when the caller did not set one, and MUST abort on the caller's timeout or cancellation.
- **VT-UI-1103.** `open()` MUST ignore intermediate 3xx responses and wait for the settled
  response, so a Hugging Face 302 is not reported as a failed download (commit `98e4732`).
- **VT-UI-1104.** Streaming MUST copy in 128 KiB chunks, MUST pause the event loop while waiting
  and MUST return `cancelled` on cancellation and `io_failure` when the sink stops the transfer.
- **VT-UI-1105.** Network errors MUST be classified: timeout, cancelled, unavailable
  (host not found, connection refused, remote closed, unknown, TLS handshake), otherwise
  `io_failure`; a non-2xx status is not a transport error.
- **VT-UI-1106.** ⚠ gap G-8: the port contract's "do not silently follow a redirect to another
  host" rule for the release API is not implemented (no host comparison).

---

## 12. Test surface

| Suite | What it pins |
|---|---|
| `ui-settings-test` (31 slots) | edit → file → restart round-trips; theme tiles and reset; engine/threshold/best-of reaching the services; microphone persistence across device-less and different-device launches; per-page navigation binding in both languages; footer and control fit at 980×640 and 820×560; hotkey capture; model list exclusivity and per-engine keys; noise reduction placement; downloaded models offer delete; log scroll retention; in-place language switch; every key non-empty; About update controls; hide-on-focus-loss; status pill theme; tray show restores the window; tray menu padding; overlay and tray language; model page language; exact tail of the string table; percentage + cancel; navigation icons; every navigation entry opens its own page; record button disabled without a backend |
| `ui-status-overlay-test` (8 slots) | idempotent create; frameless/topmost/no-input pill; frozen 350 ms pulse; processing accent and stopped pulse; error persistence; hide/idle; geometry (centred, 26 px above the bottom); worker-thread `post_state()` and terminal `destroy()` |
| `ui-snapshot` (tool) | renders all 8 pages to PNG offscreen for visual review (not an assertion) |

---

## 13. Known gaps

- **G-1.** `TrayController::set_recording()` / `set_status()` are never called in production, so
  the tray tooltip and the record menu item are not updated at runtime.
- **G-2.** The `processing` footer text uses the "Recognition engine" string instead of
  "Recognizing" (see VT-UI-909).
- **G-3.** With no system tray available the window can be hidden with no way back except a second
  launch.
- **G-4.** No `closeEvent` is defined; Alt+F4 behavior relies on Qt defaults and is untested.
- **G-5.** The result of `presenter.flush()` is discarded, so a failed settings write is invisible.
- **G-6.** `AppSettings::validate()` is never called from the UI path.
- **G-7.** Two competing icon implementations exist for the navigation (`refresh_navigation_icons`
  drawn geometry vs `refresh_nav_icons` font glyphs/clear); the final source depends on the
  presence of "Segoe MDL2 Assets", and the test does not distinguish a null icon from a valid one.
- **G-8.** The "no silent cross-host redirect" rule for the release API is not implemented.
- **G-9.** `section_title_for()` and `UiKey::k34`, `k41`, `k104` are dead: their UI is not rendered.
- **G-10.** Comments in `main_window.hpp:301-302` and inside `needs_service_apply()` still claim a
  language change rebuilds the window, which the current in-place `retranslate()` contradicts.
- **G-11.** `docs/design/figma-mockup-audit.md` §1.1/§7 is stale: it describes three title-bar
  buttons, a 980×640 window, a 32 px title bar, a 240 px sidebar and a 44 px footer; the code has
  two buttons, 1220×800, 40 px, 300 px and 56 px.
