# VoiceTyper — Core Support Module Specification

**Covers:** `src/core/support/` (SHA-256, model download service, update manifest, update
service, update launcher) plus the portable path/logger/CPU contracts implemented in
`src/domain/app_paths.*`, `src/domain/file_logger.*` and `src/domain/cpu_topology.*`.
**Baseline:** `main` @ `3cc4c5a9a602afb96c2f478a562005f5eb850068` (v2.2.1).
**Requirement keywords:** RFC 2119. Parent document: [index.spec.md](../index.spec.md).
**Prefix:** `VT-COR-*`.

- **VT-COR-001.** This layer MUST be the only place that talks to the network on behalf of the
  product (models, release metadata, installer asset) and MUST NOT add any other endpoint
  (VT-SYS-001, VT-PLT-806).
- **VT-COR-002.** Only `update_manifest.cpp` may include Qt (QtCore, for `QJsonDocument` and
  `QRegularExpression`); the rest of the layer MUST stay Qt-free, which is why it cannot live in
  `src/domain/` and must not live in `src/app/` either ([CMakeLists.txt:365-383](../../CMakeLists.txt#L365-L383)).

---

## 1. Paths, logger, CPU (`VT-COR-1xx`)

These three portable contracts live in `src/domain/` but form the "core support" target
`voicetyper_core_support` ([CMakeLists.txt:107-113](../../CMakeLists.txt#L107-L113)). Their
behavioral requirements are stated in [domain.spec.md](domain.spec.md) §VT-DOM-9xx; the
product-level consequences are:

- **VT-COR-101.** `domain::AppPaths` MUST be constructed once from
  `domain::windows_app_path_roots(environment, executable_directory)` and MUST be the single
  resolver for settings (Roaming), models/logs/updates (Local) and native libraries
  (application directory). A second, locally assembled path is forbidden (VT-SYS-060…067).
- **VT-COR-102.** `domain::FileLogger` MUST keep the .NET-compatible line format and rotation so
  that support tooling keeps working: `yyyy-MM-dd HH:mm:ss.fff [LEVEL] message` in local time,
  rotate before an append at `1,000,000` bytes, five archives, `clear_on_start` default true
  (VT-DOM-903).
- **VT-COR-103.** `Win32Logger::for_paths(paths, options, clock)` MUST wrap the same
  `FileLogger`, MUST use CRLF on Windows and MUST include the `ErrorCode` name in `write_status`
  output.
- **VT-COR-104.** The composition MUST make the log directory available to the crash guard and
  the wrong-thread diagnostics (`%VOICETYPER_LOG_DIR%` when set, otherwise the executable's
  directory).
- **VT-COR-105.** On Linux `domain::AppPaths` MUST be constructed from
  `domain::linux_app_path_roots(environment, executable_directory)` (added 2026-10-10):
  settings under `$XDG_CONFIG_HOME/VoiceTyper` with the `$HOME/.config` fallback,
  models/logs/updates under `$XDG_DATA_HOME/VoiceTyper` with the `$HOME/.local/share` fallback,
  native libraries in the executable's directory. An unset or empty variable MUST be treated as
  unset, and a fully stripped environment MUST fall back to the executable's directory — never
  to an empty path ([src/domain/app_paths.cpp](../../src/domain/app_paths.cpp), VT-PLT-1101).
- **VT-COR-106.** The Linux composition MUST create the settings, models, logs and updates
  directories before the first write, and `%VOICETYPER_LOG_DIR%` MUST keep overriding the log
  directory on Linux as well.

---

## 2. SHA-256 (`VT-COR-2xx`)

`standard: FIPS 180-4`, own implementation, no third-party crypto.

- **VT-COR-201.** The product MUST provide `sha256_hex(span<uint8_t>)` / `sha256_hex(string_view)`
  returning **lowercase** hex, and a streaming `Sha256::update()` / `finish_hex()` for
  hashing a download while it is being written
  ([src/core/support/sha256.hpp](../../src/core/support/sha256.hpp)).
- **VT-COR-202.** `finish_hex()` MUST finalize; calling it and continuing to `update()` is
  forbidden by contract (there is no runtime guard).
- **VT-COR-203.** The digest MUST be computed from the byte stream, not from a second read of
  the file, so a large installer is hashed in one pass (VT-COR-203).

---

## 3. Model download service (`VT-COR-3xx`)

`src/core/support/model_download_service.hpp`, implementation of the frozen addresses shared
with the catalog.

- **VT-COR-301.** The engine enum MUST be `whisper | parakeet | gigaam`; the VAD model is not
  downloaded by this service.
- **VT-COR-302.** File names and download URLs MUST come from one place (no local duplicates in
  the composition): whisper `ggml-<size>-q8_0.bin` (plus `ggml-large-v3-turbo-q8_0.bin`),
  parakeet `tdt-0.6b-v3-{q4_k,q5_k,q6_k,q8_0}.gguf`,
  gigaam `gigaam-v3-e2e-rnnt-{Q4_K_M,Q5_K_M,Q6_K,Q8_0}.gguf`.
- **VT-COR-303.** Base URLs MUST be the frozen Hugging Face locations (VT-PLT-904).
- **VT-COR-304.** The download timeout MUST be `7200 s` (`kModelDownloadTimeout`).
- **VT-COR-305.** An already-cancelled token MUST return `cancelled` before the request opens.
- **VT-COR-306.** Before starting, the service MUST remove `<name>.part` **and** the existing
  target file, so a failed download never leaves a file that looks like a ready model.
- **VT-COR-307.** A non-2xx response MUST fail with `unavailable` and the message
  `"the model host answered with status N"`.
- **VT-COR-308.** On completion, if `total > 0` and `downloaded != total`, the download MUST
  fail with `io_failure "the download stopped before the whole file arrived"` and the `.part`
  file MUST be removed.
- **VT-COR-309.** On a read error the `.part` file MUST be removed and `cancelled` MUST be
  returned when the token was requested.
- **VT-COR-310.** The final step MUST be an atomic rename of `.part` onto the target
  (`domain::FileSystem::replace_file`).
- **VT-COR-311.** Progress MUST be reported at most every `120 ms` and once at completion; the
  sink runs on the download thread and the caller MUST marshal to the UI thread
  (VT-PLT-805).
- **VT-COR-312.** `ModelDownloadProgress` MUST compute `fraction()` clamped to `[0, 1]`
  (`0` when the total is unknown) and `remaining_seconds()` as `nullopt` when the total is
  unknown, the download is complete, or the rate is non-positive.
- **VT-COR-313.** A redirect (HTTP 3xx) MUST NOT be treated as the final answer; the client
  follows it and the service sees the real 200 (commit `98e4732`).
- **VT-COR-314.** Cancellation MUST delete the `.part` file and MUST return
  `ErrorCode::cancelled` — not `io_failure` — so the UI can say "download cancelled" instead of
  "connection error" (commit `24d3b56`).
- **VT-COR-315.** Progress and completion MUST reach the UI through a queued invocation; touching
  `QProgressBar`, icons or button roles from the download thread crashed the application with
  `QWidget::repaint: Recursive repaint detected` (commit `eda7312`).

---

## 4. Update manifest (`VT-COR-4xx`)

`src/core/support/update_manifest.{hpp,cpp}`, QtCore-only.

- **VT-COR-401.** The installer asset MUST be selected with the frozen pattern
  `^VoiceTyper-\d[^/]*?-Setup\.exe$` (`kSetupAssetRegex`), compiled once, and the **first**
  match in API order MUST win.
- **VT-COR-402.** Parsing order is observable and MUST be preserved: asset selection first,
  then the download URL, then the SHA (extracted from the body), then the version comparison.
- **VT-COR-403.** The parse MUST fail with `failed` and a specific message for: an empty or
  whitespace-only body, invalid JSON, a non-object root, a non-array `assets`, no matching asset,
  and an asset without a download URL. The required texts are:

| Situation | Message |
|---|---|
| release absent | `Release not found. The repository is unavailable or private.` |
| HTTP 403 | `GitHub access denied (403).` |
| HTTP 429 | `GitHub rate limit exceeded.` |
| malformed body | `Invalid server response.` |
| no matching asset | `Installer not found in the release.` |
| asset without URL | `The installer has no download URL.` |

- **VT-COR-404.** The version MUST come from `tag_name` with **all** leading `v` characters
  stripped (`TrimStart('v')` semantics) — not a strict SemVer parse.
- **VT-COR-405.** `prerelease` MUST be stored but MUST NOT filter the release.
- **VT-COR-406.** An asset `size` given as a JSON number in `[0, 2^64)` MUST be converted to
  `uint64_t`; other shapes MUST be ignored rather than fail the parse.
- **VT-COR-407.** `release_notes` MUST be set only when `body` was a string.
- **VT-COR-408.** `failed_for_http_status(status)` MUST return `nullopt` for 2xx, the specific
  message for 404/403/429, and `"Request failed with HTTP status N."` otherwise.

### Version comparison

- **VT-COR-420.** `compare_update_versions(a, b)` MUST reproduce the legacy comparison, which is
  **not** strict SemVer:
  1. build metadata after `+` is ignored;
  2. missing numeric segments are treated as zero;
  3. a non-numeric or overflowing segment becomes zero;
  4. a stable release ranks above any prerelease;
  5. two prereleases with equal numeric cores compare equal regardless of their identifiers;
  6. whitespace is not trimmed.
- **VT-COR-421.** A strict-SemVer implementation is allowed only as an explicit, versioned
  migration decision; otherwise this comparison is what ships.
- **VT-COR-422.** `strip_version_tag_prefix()` MUST strip leading `v` characters only.

### SHA-256 marker

- **VT-COR-430.** `extract_release_sha256(body)` MUST accept a whole line matching
  `SHA256:` or `SHA-256:` case-insensitively followed by exactly 64 hex characters, and MUST
  return `nullopt` when absent. A missing SHA MUST be accepted (installation is allowed).
- **VT-COR-431.** The marker search MUST be whole-line, so an incidental 64-hex run inside prose
  is not mistaken for a checksum.

---

## 5. Update service and launcher (`VT-COR-5xx`)

`src/core/support/update_service.{hpp,cpp}`, `src/core/support/update_launcher.{hpp,cpp}`.

- **VT-COR-501.** The release endpoint MUST be
  `https://api.github.com/repos/mops1k/VoiceTyper/releases/latest`
  (`kUpdateLatestReleaseUrl`) and the request MUST carry `Accept: application/vnd.github+json`
  and the product user agent (VT-PLT-802).
- **VT-COR-502.** `check(cancellation)` MUST return an `UpdateCheckResult` whose kind is
  `up_to_date | update_available | failed` (ordinals 0/1/2 preserved from the .NET enum).
- **VT-COR-503.** `download(update_info, target, progress, cancellation)` MUST stream the
  installer, hash it on the fly and compare with `update_info.sha256` when present.
- **VT-COR-504.** A SHA mismatch MUST delete the downloaded file and MUST return `io_failure`
  with the message `"the downloaded installer does not match its SHA-256: expected …, got …"`,
  and the previously downloaded installer MUST be preserved (VT-SYS-083).
- **VT-COR-505.** The download target MUST be `<updates>/VoiceTyper-<version>-Setup.exe`
  (VT-SYS-063, VT-PLT-909).
- **VT-COR-506.** The launcher script MUST be exactly (CRLF line endings):

```
@echo off
start "" /wait "<installer>" /AutoUpdate
start "" "<app>"
```

  and MUST be started as `cmd.exe` with arguments `"/d /c "<runner>""`
  (`build_update_runner_script`, `build_update_runner_arguments`,
  `write_update_runner_script`).
- **VT-COR-507.** The script MUST be written in binary mode (`create_update_runner_script`
  writes the bytes directly) so the CRLF endings survive.
- **VT-COR-508.** The launcher MUST relaunch the application regardless of the installer's exit
  status, and the application MUST quit before the installer runs.
- **VT-COR-509.** The composition MUST refuse to run an installer whose URL lacks the `win64`
  marker (`kNativeInstallerMarker`) — this is what keeps an old .NET asset from being installed
  over the native build (VT-SYS-085).
- **VT-COR-510.** An update MUST NOT be started while a recording or a processing session is in
  progress.
- **VT-COR-511.** The startup check MUST be silent: the result goes to the log only, and the UI
  MUST NOT be touched from that path.
- **VT-COR-512.** A non-2xx HTTP status is not a transport error: the caller inspects
  `status_code` and `failed_for_http_status` maps it to the user-facing message.
- **VT-COR-513.** `UpdateInfo` MUST carry `version`, optional `installer_url`, optional `sha256`,
  optional `size_bytes`, optional `release_notes` and `is_prerelease`.

---

## 6. Data model (`VT-COR-6xx`)

### 6.1 Files written

| Artifact | Path | Content |
|---|---|---|
| Installer | `<updates>/VoiceTyper-<version>-Setup.exe` | downloaded asset |
| Update runner | `<updates>/run-update.cmd` | the three lines of VT-COR-506, CRLF |
| Model | `<models>/<catalog file name>` | downloaded model |
| Model temp | `<models>/<file name>.part` (service) / `.download` (store) | partial download, removed on failure |

- **VT-COR-601.** Two temp suffixes are in use: the model download service writes `.part`, while
  the model store and the updater contract use `.download`. A specification MUST name the actual
  file for each path (⚠ gap G-6 in [platform.spec.md](platform.spec.md)).

### 6.2 GitHub release document (consumed)

```
{
  "tag_name": "<string>",              // leading 'v' stripped
  "body": "<string>",                  // release notes; also the SHA-256 marker source
  "prerelease": <bool>,
  "assets": [                          // required to be an array
    { "name": "<string>",              // must match ^VoiceTyper-\d[^/]*?-Setup\.exe$
      "browser_download_url": "<string>",
      "size": <number in [0, 2^64)> }
  ]
}
```

### 6.3 Result types

| Type | Fields |
|---|---|
| `UpdateCheckResultKind` | `up_to_date = 0`, `update_available = 1`, `failed = 2` |
| `UpdateInfo` | `version`, `installer_url?`, `sha256?`, `size_bytes?`, `release_notes?`, `is_prerelease` |
| `UpdateCheckResult` | `kind`, `update?` (only when available), `error` (only when failed) |
| `ModelDownloadProgress` | `downloaded`, `total`, `bytes_per_second`, `fraction()`, `remaining_seconds()` |
| `Sha256` | `update()`, `finish_hex()` |

---

## 7. Requirement-to-evidence map

| Requirement block | Contract test |
|---|---|
| paths, logger, CPU | `core-support-contract`, `win32-platform-contract` |
| SHA-256, model addresses, atomic `.part` rename, progress arithmetic | `model-download-contract` (24 checks), `model-store-contract` |
| release parsing, asset selection, version comparison, SHA extraction, runner bytes | `update-contract`, `update-service-contract` |
| provider seam (catalog + download with injected HTTP/FS) | `model-store-contract` |

---

## 8. Known gaps

- **G-1.** The updater header contract (`platform/api/updater.hpp`) describes a `.download` temp
  file that is renamed onto the target and deleted on mismatch, while the implementation writes
  the target directly and deletes the target on error. The implementation is what ships; the
  header and the code must be reconciled before either is treated as normative.
- **G-2.** `ModelDownloadService` deletes an existing target before starting, so a failed
  re-download leaves no old file; resume/skip behavior is unspecified.
- **G-3.** There is no startup cleanup of stale `.part`/`.download` files (the model store has an
  explicit `cleanup_stale_downloads()`, the download service does not).
- **G-4.** An existing model file was historically accepted by existence alone; the product
  relies on the `.part`+rename protocol and does not verify a restored/legacy file
  (risk R11).
- **G-5.** The `run-update.cmd` script is written non-atomically and is not quoted against a path
  containing `"` (the legacy launcher had the same property; hardening is a staged policy, not
  current parity).
