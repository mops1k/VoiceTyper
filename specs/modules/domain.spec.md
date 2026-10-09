# VoiceTyper — Domain Module Specification

**Covers:** `src/domain/` (portable core: settings, recording state machine, VAD,
speech segmentation, terms dictionary, text output, audio, errors, paths, logger, CPU).
**Baseline:** `main` @ `3cc4c5a9a602afb96c2f478a562005f5eb850068` (v2.2.1).
**Requirement keywords:** RFC 2119. Parent document: [index.spec.md](../index.spec.md).
**Prefix:** `VT-DOM-*`.

The domain layer MUST remain free of Qt and of OS SDKs (`cmake/PortableHeaders.cmake`,
re-checked by the `portable-headers` CTest) and MUST NOT depend on `src/asr/`
(VT-SYS-031, VT-SYS-032).

---

## 1. Error model and cancellation

### 1.1 `ErrorCode`

`ErrorCode` is a 19-value enum ([src/domain/error.hpp:30-69](../../src/domain/error.hpp#L30-L69)):

`ok=0, cancelled=1, invalid_argument=2, not_found=3, already_exists=4, not_ready=5,
unsupported=6, permission_denied=7, unavailable=8, device_disconnected=9, timeout=10,
io_failure=11, corrupt_data=12, out_of_range=13, resource_exhausted=14,
engine_unavailable=15, model_not_ready=16, invalid_state=17, internal=18`.

- **VT-DOM-101.** Every failure crossing a module boundary MUST be expressed as
  `Error`/`Status`/`Result<T>`; the code MUST NOT throw across a port boundary.
- **VT-DOM-102.** `error_code_name()` MUST be the stable spelling of a code
  (`"unknown"` outside the range) and `is_error(code)` MUST be true for every value but `ok`.
- **VT-DOM-103.** `Error` MUST carry a code plus optional message and detail;
  `to_string()` MUST render `"<code_name>: <message> (<detail>)"`.
- **VT-DOM-104.** `Status::with_context(detail)` MUST preserve the original code and message.
- **VT-DOM-105.** `Result<T>` MUST hold either a value or an `Error`. The ordinal values of
  `ErrorCode` are **not** a wire format and MUST NOT be treated as stable across versions
  (⚠ gap G-8: nothing pins them with a `static_assert`).

### 1.2 Cancellation

- **VT-DOM-110.** Cancellation MUST be cooperative through `CancellationToken` /
  `CancellationSource` (a product-owned replacement for `std::stop_token`,
  [src/domain/cancellation.hpp](../../src/domain/cancellation.hpp)), observed with acquire semantics.
- **VT-DOM-111.** A default-constructed `CancellationToken` MUST mean "can never be cancelled";
  `can_be_cancelled()` MUST reflect that.
- **VT-DOM-112.** A cancellation request MUST be sticky until `reset()`; `reset()` on a shared
  state is documented as racy and is the caller's responsibility.
- **VT-DOM-113.** `check_cancelled()` MUST return `cancelled` with message `"operation cancelled"`.

---

## 2. Settings schema (`VT-DOM-2xx`)

### 2.1 The persisted root

- **VT-DOM-201.** `AppSettings` MUST persist exactly **23** properties in the declaration
  order that is also the JSON output order; the count is pinned as
  `kAppSettingsPropertyCount = 23` ([src/domain/settings.hpp:210](../../src/domain/settings.hpp#L210)).
  Changing the count or the order is a schema change and MUST come with a new golden fixture.
- **VT-DOM-202.** Every enum MUST have a camelCase wire spelling produced by `to_wire()` and a
  case-insensitive reverse lookup returning `std::nullopt` for an unknown name
  ([src/domain/settings.hpp:113-206](../../src/domain/settings.hpp#L113-L206)).

| Enum | Wire values (ordinal order) | Fallback in `to_wire` |
|---|---|---|
| `RecordingMode` | `pushToTalk`, `toggle`, `vad` | `pushToTalk` |
| `AppTheme` | `system`, `light`, `dark` | `system` |
| `ModelSize` | `tiny`, `base`, `small`, `medium`, `large` | `small` |
| `TranscriptionEngine` | `whisper`, `parakeet`, `gigaam` | `whisper` |
| `ParakeetModelSize` | `q4K`, `q5K`, `q6K`, `q8_0` | `q8_0` |
| `GigaamModelSize` | `q4_k_m`, `q5_k_m`, `q6_k`, `q8_0` | `q8_0` |
| `RecognitionLanguage` | `auto` (C++ name `automatic`), `ru`, `en` | `ru` |
| `AppLanguage` | `ru`, `en` | `ru` |

- **VT-DOM-203.** The persisted property list, in output order, is:

| # | JSON key | Type | Default |
|---|---|---|---|
| 1 | `recordingMode` | enum string | `pushToTalk` |
| 2 | `recordHotkey` | string | `Ctrl+Alt+Space` |
| 3 | `cancelHotkey` | string | `Ctrl+Alt+Escape` |
| 4 | `recordGamepadButton` | string \| null | null |
| 5 | `cancelGamepadButton` | string \| null | null |
| 6 | `language` | enum string | `ru` |
| 7 | `modelSize` | enum string | `small` |
| 8 | `transcriptionEngine` | enum string | `whisper` |
| 9 | `parakeetModelSize` | enum string | `q8_0` |
| 10 | `gigaamModelSize` | enum string | `q8_0` *(C++-only)* |
| 11 | `autoPasteEnabled` | bool | `true` |
| 12 | `termsDictionary` | string | `API,CPU,GPU,ASR,STT,TTS,LLM,JSON,IDE,SQL` |
| 13 | `silenceThresholdMs` | int | `1200` |
| 14 | `startWithWindows` | bool | `false` |
| 15 | `startMinimized` | bool | `false` |
| 16 | `theme` | enum string | `system` |
| 17 | `hideOnFocusLoss` | bool | `false` |
| 18 | `appLanguage` | enum string | `ru` |
| 19 | `noiseReductionEnabled` | bool | `false` |
| 20 | `temperature` | number | `0.0` |
| 21 | `bestOf` | int | `3` *(C++-only)* |
| 22 | `conditionOnPreviousText` | bool | `false` |
| 23 | `microphoneDeviceId` | string \| null | null |

- **VT-DOM-204.** `gigaamModelSize` and `bestOf` MUST be treated as C++-only extensions: the
  legacy .NET reader ignores them, so a rollback MUST keep working while
  `transcriptionEngine != "gigaam"`.
- **VT-DOM-205.** `AppSettings::defaults()` MUST be the value used whenever the file is
  missing, corrupt, unreadable or rejected; `clone()` MUST deep-copy the optionals and strings.
- **VT-DOM-206.** `validate()` MUST reject (with `out_of_range`) `silence_threshold_ms`
  outside `[300, 10000]`, negative `temperature`, and `best_of` outside `[1, 8]`.
  `validate()` MUST NOT modify anything — clamping is a separate, explicit step performed by
  the settings/UI layer.
- **VT-DOM-207.** `record_hotkey_gesture()` / `cancel_hotkey_gesture()` MUST parse the stored
  strings and MUST return `invalid_argument` for a bad string instead of silently replacing it.
- **VT-DOM-208.** `record_gamepad_binding()` / `cancel_gamepad_binding()` MUST treat
  "unassigned" (`nullopt` or empty) as success, not as an error.

### 2.2 JSON codec (`SettingsCodec`)

Public surface ([src/domain/settings_json.hpp](../../src/domain/settings_json.hpp)):
`SettingsLoadResult load(std::string_view)`, `load_file(path)`,
`std::string serialize(const AppSettings&, line_ending = "\r\n")`,
`Status save_file(path, const AppSettings&, line_ending = "\r\n")`.

- **VT-DOM-210.** Serialization MUST be indented UTF-8 JSON: two-space indent, `": "` separator,
  camelCase keys, camelCase enum strings, declaration order, CRLF by default, **no trailing
  newline**.
- **VT-DOM-211.** Nullable properties MUST always be emitted as `null` and never omitted.
- **VT-DOM-212.** Reading MUST be case-insensitive for property names; unknown properties MUST
  be ignored.
- **VT-DOM-213.** Duplicate properties (including case-only duplicates) MUST be de-duplicated
  **before** validation, last occurrence wins, and superseded values MUST NOT be validated.
- **VT-DOM-214.** A wrong type or an unknown enum string MUST put the **whole document** into
  defaults with `used_defaults = true` and an `error` diagnostic naming the field.
- **VT-DOM-215.** A missing file MUST yield defaults, `used_defaults = true` and **no**
  diagnostic (first run is normal). A file that cannot be opened/read MUST yield defaults plus
  an `error` diagnostic.
- **VT-DOM-216.** `load`, `load_file` and `save_file` MUST NOT throw.
- **VT-DOM-217.** A numeric enum value MUST be accepted in range (with a `warning` that the
  canonical string will be written back), and an out-of-range numeric enum MUST fall back to
  the property default with a `warning`; a non-integer numeric enum MUST be an `error`.
- **VT-DOM-218.** `null` for a **nullable** string MUST set it unset silently; `null` for a
  **non-nullable** string (`recordHotkey`, `cancelHotkey`, `termsDictionary`) MUST yield an
  empty string plus a `warning`, and the document MUST still load.
- **VT-DOM-219.** `silenceThresholdMs` and `bestOf` MUST be read verbatim — reading MUST NOT
  clamp them and MUST NOT turn the document into defaults for an out-of-range value.
- **VT-DOM-220.** `save_file` MUST `create_directories(parent)`, write `<path>.tmp`, flush,
  then replace atomically (`MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`
  on Windows, `std::filesystem::rename` elsewhere). On any failure the temporary file MUST be
  removed and the previous file MUST stay untouched; failures MUST be `io_failure`.
- **VT-DOM-221.** `save_file` MUST NOT call `validate()`: it writes exactly what it was given,
  so a document with out-of-range values round-trips verbatim.
- **VT-DOM-222.** The JSON parser MUST reject: nesting deeper than 64, control characters
  (`< 0x20`) inside strings, unpaired surrogates, and trailing characters after the document.
  Integers beyond `int64` MUST degrade to a double rather than abort the parse.
- **VT-DOM-223.** Serialization MUST escape `" \ b f n r t` plus `+ & < > '` and every other
  `< 0x20` as `\uXXXX`; non-ASCII MUST be emitted as `\uXXXX` (astral characters as a surrogate
  pair). Non-finite `temperature` MUST be normalized to `0` instead of throwing (documented
  divergence from the .NET serializer, which throws) and `0.0` MUST be written as `0`.
- **VT-DOM-224.** There is **no** `schemaVersion` and no migration code (⚠ gap G-10); any future
  schema version MUST be optional and backward compatible.

### 2.3 Diagnostics

| Kind | Field | Trigger | Effect |
|---|---|---|---|
| `error` | `$` | not JSON / root not an object / trailing chars / parser exception | document → defaults |
| `error` | property name | wrong type, unknown enum string, non-integer numeric enum | document → defaults |
| `warning` | property name | numeric enum (in or out of range) | document loads; canonical string is written on save |
| `warning` | property name | `null` for a non-nullable string | document loads; value becomes `""` |
| `error` | `$` | file could not be opened/read | document → defaults |
| — | — | file missing | defaults, no diagnostics |

---

## 3. Recording state machine (`VT-DOM-3xx`)

Contract: [src/domain/recording_state_machine.hpp](../../src/domain/recording_state_machine.hpp);
evidence: `recording-state-machine-contract`.

### 3.1 States, modes, ports

- **VT-DOM-301.** The observable state MUST be `idle | recording | processing`
  ([src/domain/recording_state_machine.hpp:63](../../src/domain/recording_state_machine.hpp#L63)).
- **VT-DOM-302.** The machine MUST be constructed over abstract ports and MUST NOT know about
  WASAPI, COM, Qt or a native engine:
  `RecordingStateMachine(RecordingPort&, TranscriptionPort&, TextOutputPort&, RecordingWorker&,
  RecordingStateMachineOptions = {}, SpeechSegmenter* = nullptr)`.
- **VT-DOM-303.** `RecordingPort` MUST be device I/O only and MUST return normalized 16 kHz mono
  float samples; the machine MUST NOT resample. `stop()` MUST return the whole session buffered
  so far. `cancel()` MUST be idempotent. `drain()` MUST be usable without stopping and an empty
  result MUST NOT be an error.
- **VT-DOM-304.** `TranscriptionPort` MUST expose one blocking call per session — no partial or
  streaming entry point. An empty string MUST be a **successful** result; a whitespace-only
  transcript MUST mean "nothing to deliver".
- **VT-DOM-305.** `TextOutputPort::output(text, auto_paste, cancellation)` MUST return `true`
  for "delivered" (`pasted` or `clipboard_only`) and `false` for a deliberate skip of an empty
  transcript — not an error.

### 3.2 Transitions

```
      press_record() [any mode]
Idle ────────────────────────────► Recording
  ▲                                   │  release_record() [push_to_talk]
  │                                   │  press_record()  [toggle]
  │                                   │  VAD auto-stop   [vad]
  │                                   ▼
  │                              (stop-before-process)
  │                                   │
  │        buffer empty ──────────────┤
  └───────────────────────────────────┘
                                      │ buffer non-empty
                                      ▼
                                 Processing ──► (text/output) ──► Idle
```

- **VT-DOM-310.** `press_record()` MUST start a session from `idle` in **every** mode; in
  `toggle` mode while recording without a pending stop it MUST stop the session; in every other
  combination it MUST be a no-op success (a repeated press in `vad` mode MUST NOT restart).
- **VT-DOM-311.** `release_record()` MUST stop only when the state is `recording`, the mode is
  `pushToTalk` and no stop is already pending; otherwise it MUST be a no-op success.
- **VT-DOM-312.** The mode and the VAD silence threshold MUST be read from the live settings
  provider **at session start**; a provider returning `vad_silence_threshold_seconds <= 0` MUST
  be ignored and the configured option used.
- **VT-DOM-313.** `stop()` on the capture port MUST complete **before** transcription starts
  (stop-before-process), and the buffer MUST be moved into the processing task so no audio tail
  can be lost.
- **VT-DOM-314.** `processing` MUST be entered only for a non-empty buffer; an empty capture
  MUST return to `idle` without a transcription.
- **VT-DOM-315.** Exactly one transcription and at most one text delivery MUST happen per
  session (final-only, VT-RULE-003).
- **VT-DOM-316.** No port call, state publication or handler invocation may happen while the
  machine's mutex is held; handlers MUST be invoked after the lock is released, in queue order,
  and their exceptions MUST be swallowed.
- **VT-DOM-317.** `cancel()` MUST be idempotent: with no active session and state `idle` it MUST
  touch neither the ports nor the events.
- **VT-DOM-318.** Cancellation MUST be epoch-scoped: a step that was in flight when the session
  was superseded MUST publish nothing — no text, no state resurrection.
- **VT-DOM-319.** `dispose()` and `reset_session()` MUST NEVER destroy a cached `SpeechSegmenter`;
  they MAY only call `reset()` on it. `dispose()` MUST be idempotent and MUST drop a pending
  segmenter reset.
- **VT-DOM-320.** `wait_idle(timeout = 5000 ms)` MUST join the worker first and then wait for
  processing to finish; it MUST return `false` on timeout rather than block forever.
- **VT-DOM-321.** `is_blank_text()` MUST treat ASCII whitespace (`" \t\r\n\v\f"`) as blank; the
  text-output layer MUST additionally treat Unicode whitespace as blank (VT-DOM-5xx).
- **VT-DOM-322.** Inside the lock the machine MUST use the `*_locked()` readers
  (`mode_locked()`, `live_silence_threshold_seconds_locked()`); the public readers MUST NOT be
  called under the lock (the mutex is not recursive).

### 3.3 Timing defaults

| Constant | Value | Source |
|---|---|---|
| VAD poll interval | 250 ms | `RecordingStateMachineOptions` |
| VAD silence threshold | 1.2 s | `RecordingStateMachineOptions`, `kSilenceThresholdDefaultMs` |
| `wait_idle` timeout | 5000 ms | `RecordingStateMachine::wait_idle` |
| Final `bestOf` | 3 | `kFinalBestOf`, `static_assert` vs `kDefaultBestOf` |
| `session auto_paste` | true | `SessionOptions` |

- **VT-DOM-323.** `SessionOptions` MUST be re-read on every session through the options provider
  so a settings change applies without recreating the machine. Fields: `language`, `prompt`,
  `temperature`, `condition_on_previous_text`, `auto_paste`, `best_of`, `noise_suppression`,
  `speech_map`.

---

## 4. VAD and speech segmentation (`VT-DOM-4xx`)

### 4.1 Segmenter seam

- **VT-DOM-401.** `SpeechSegmenter` MUST expose `detect_speech_no_reset(samples)`, `reset()`,
  and the optional `last_frame_probabilities()` / `probability_frame_seconds()`. An empty
  probability vector MUST mean "this segmenter has no frame-level information" and the caller
  MUST have a fallback.
- **VT-DOM-402.** `CallbackSpeechSegmenter` MUST be the adapter for an injected detector
  (a null callback yields no segments) and `EnergySpeechSegmenter` the dependency-free fallback.
- **VT-DOM-403.** `EnergySpeechSegmenter` MUST use 30 ms frames, MUST set the noise floor from
  the first frame, MUST let the floor fall (`floor*0.9 + rms*0.1`, never rise on loud input) and
  MUST classify a frame as speech when `rms > floor*3.0 **or** rms > 0.02`.
- **VT-DOM-404.** The energy segmenter is a *fallback*: it can be fooled by steady noise. In the
  real composition it MUST be a clearly logged fallback behind the Silero detector
  (`docs/migration/cpp/compatibility-contracts.md` §5, commit `75b55b6`).

### 4.2 Silence auto-stop

- **VT-DOM-410.** `SilenceAutoStopDetector(segmenter, threshold_seconds = 1.2, sample_rate = 16000)`
  MUST define `threshold = max(0, t)`, `hold = min(0.8, max(0.3, t/2))`, and MUST substitute
  16000 for a zero sample rate.
- **VT-DOM-411.** The detector MUST accept both chunk-relative and absolute segment times
  (`absolute_end = end <= chunk_duration ? total + end : end`) and MUST keep
  `last_speech_end` at the latest of the segment ends and any energy-active chunk end.
- **VT-DOM-412.** The noise floor MUST start at `1e-5`, fall as `floor*0.7 + energy*0.3` when the
  chunk is quieter, and, when it is not energy-active, rise slowly as `max(1e-8, floor*1.001)`.
  A chunk MUST be energy-active when `energy > floor*3.0`.
- **VT-DOM-413.** With **no speech at all** the detector MUST stop after `total_fed_seconds >= 5.0`
  with reason `no_speech_idle`.
- **VT-DOM-414.** After speech it MUST stop with reason `trailing_silence` when at least one
  consecutive chunk is inactive **and** `total_fed_seconds - last_speech_end >= threshold`.
- **VT-DOM-415.** An empty input MUST produce no decision and MUST NOT advance the timers.
- **VT-DOM-416.** The machine MUST create one detector per session over the borrowed segmenter
  and MUST stop at most once per session.

### 4.3 What the engine actually sees

Constants ([src/domain/speech_segments.hpp:51-61](../../src/domain/speech_segments.hpp#L51-L61)):

| Constant | Value |
|---|---|
| `kSilenceTrimMarginSeconds` | 0.5 s |
| `kSilenceTrimMaxSilenceSeconds` | 0.6 s |
| `kSilenceTrimGapSeconds` | 0.3 s |
| `kDefaultMaxChunkSeconds` | 25.0 s |
| `kMinChunkSeconds` | 0.5 s |

- **VT-DOM-420.** Trimming MUST keep `margin` (0.5 s) of audio around every detected speech span;
  a speech span starting within the margin of the recording edge MUST keep the leading edge
  untouched (Alexander's requirement of 2026-10-07, commit `0426a70`).
- **VT-DOM-421.** An internal pause longer than `max_silence` (0.6 s) MUST be compressed to
  `gap` (0.3 s), keeping the first samples of each run.
- **VT-DOM-422.** When the detector finds **no** speech the audio MUST be passed through
  unchanged — a hesitant VAD MUST NOT cost the user a dictation.
- **VT-DOM-423.** Segments MUST be normalized before use: non-finite and empty spans dropped,
  spans clamped to `[0, total]`, sorted, and touching spans merged.
- **VT-DOM-424.** `extend_segments_with_energy()` MUST be an independent energy guard: 30 ms
  frames, threshold `sorted[frame_count/3] * 3.0 + 1e-6`, growing a span outward while the
  neighbouring frame carries energy. Its measured cost is 2–3 ms per 30 s of audio and it MUST
  NOT be dropped from the pipeline.
- **VT-DOM-425.** `map_speech_map()` MUST translate both segment times and per-frame
  probabilities into the trimmed timeline; a mapping outside every kept span MUST clamp to the
  nearest edge. This exists so the detector runs **once** per dictation.
- **VT-DOM-426.** `plan_speech_chunks()` MUST cut a dictation longer than the model window
  (default 25 s) **at a pause**, and inside a single long span at the frame with the lowest
  speech probability (earliest frame on ties; the middle when no probabilities are available).
  Chunk budget MUST be counted from the start of speech, zero-length chunks MUST be dropped, and
  audio MUST never be silently dropped (VT-RULE-002).
- **VT-DOM-427.** When a prepared speech map is passed to an engine, the engine MUST NOT run the
  detector again (`SpeechMap` in `SessionOptions`).
- **VT-DOM-428.** `join_transcripts()` MUST normalize whitespace runs to a single space, drop
  empty parts, and MUST NOT insert a space after `' '`, `'-'`, `'\''` or before
  `, . ; : ! ? ) ]`. Punctuation MUST NOT be invented.

### 4.4 Silence-trimming decorator

- **VT-DOM-430.** `SilenceTrimmingPort(engine, segmenter)` MUST trim before the engine and MUST
  forward the resulting `SpeechMap` in `SessionOptions`.
- **VT-DOM-431.** Recordings shorter than 4.0 s (`64000` samples) MUST NOT be trimmed: the
  detector pass (measured 355 ms for 3 s, 1369 ms for 30 s) costs more than the saved silence
  and such a clip already fits any engine window.
- **VT-DOM-432.** A failure while assembling the trimmed buffer MUST fall back to the original
  buffer rather than lose the dictation.
- **VT-DOM-433.** The port MUST expose `last_report()` with removed leading/trailing samples,
  compressed pause samples, speech segment count, `unchanged` and `skipped_short` flags, and the
  trimming numbers MUST be written to the log (never the transcript text, VT-SYS-073).

### 4.5 Known gaps

- **G-4.** `lowest_probability_sample` converts frames with a hard-coded `16000.0`
  ([src/domain/speech_segments.cpp:92](../../src/domain/speech_segments.cpp#L92)) and ignores the `sample_rate`
  argument of `plan_speech_chunks`; a non-16 kHz call would plan chunks wrongly.
- **G-5.** Two trimming paths disagree on the default margin: the RMS path
  `audio_wav::trim_silence` defaults to 0.25 s while the segment path uses 0.5 s. The normative
  path for dictation is the segment path (VT-DOM-420).

---

## 5. Terms dictionary (`VT-DOM-5xx`)

Contract: [src/domain/terms_dictionary.hpp](../../src/domain/terms_dictionary.hpp),
decorator [src/domain/terms_dictionary_port.hpp](../../src/domain/terms_dictionary_port.hpp); tests: `terms-dictionary-contract`.

### 5.1 Format

- **VT-DOM-501.** Entries MUST be separated by `,` or a newline; surrounding `" \t\r\n"` MUST be
  trimmed; empty entries MUST be skipped.
- **VT-DOM-502.** An entry containing `=` MUST be split on the **first** `=` into
  "as heard" (first side) and "as written" (second side); an entry whose either side is empty
  MUST be ignored. A plain entry is a term.
- **VT-DOM-503.** Plain terms MUST be de-duplicated case-insensitively keeping the first
  spelling; replacements MUST be de-duplicated by the lower-cased "heard" side keeping the first.
- **VT-DOM-504.** A dictionary MUST feed both mechanisms: the engine prompt (where the engine
  supports one) and the post-processing replacement, which works on **every** engine
  (decided 2026-10-07, commit `44a3f92`).

### 5.2 Initial prompt

- **VT-DOM-510.** `terms_initial_prompt()` MUST emit `"Термины пишутся латиницей: "` followed by
  the plain terms and the written sides joined with `", "` and a final `'.'`, within a **400-byte**
  budget; terms that would exceed the budget MUST be omitted.
- **VT-DOM-511.** The prompt is a hint only. A measurement on Whisper small q8 showed that
  Cyrillic-to-Latin cannot be forced this way; the phrase must not damage Russian words
  (commit `44a3f92`).

### 5.3 Replacement rules (exact pairs)

- **VT-DOM-520.** Explicit pairs MUST be applied case-insensitively on whole words only; "word"
  characters are `[0-9A-Za-z]` plus Cyrillic `U+0400..U+04FF`. A space inside the "heard" side
  MUST match only `U+0020`.
- **VT-DOM-521.** Rules MUST be tried longest "heard" first (stable sort by code-point length).
- **VT-DOM-522.** The replacement MUST be inserted verbatim, except that its first character
  MUST be upper-cased when the replaced word was capitalized (and not fully upper-case).
- **VT-DOM-523.** An exact pair MUST always win over any fuzzy match.

### 5.4 Replacement rules (sound and shape)

- **VT-DOM-530.** A fuzzy match MUST require: normalized forms within a length difference of 3,
  and a match at one of three independently evaluated stages (a failed stage MUST NOT block the
  next):
  1. **Exact phonetic form** — normalized forms equal (this stage is not a guess, so it is not
     subject to the consonant-skeleton minimum).
  2. **Similarity** — both consonant skeletons `>= 3` and equal, and Levenshtein distance of the
     full forms within `min(base, len/4)` where `base = 0` for `len <= 4`, `1` for `5..7`, `2` for
     `>= 8`; **or** "one consonant off": both skeletons `>= 4`, skeleton distance `<= 1`, and form
     similarity `>= 0.6` (`1 - distance/max_length`).
  3. **Spelled-out acronym** — the written side is 2..6 Latin letters, the word is decoded by
     backtracking over the letter-name table (longest pieces first, e.g. `даблъю→w`, `кью→q`),
     and the decoded letters MUST equal the term **exactly**; decoding budget 64. A near match
     MUST NOT replace (`иди` must not become `IDE`).
- **VT-DOM-531.** Unknown or ambiguous cases MUST be skipped, not guessed: a word that reaches
  two different replacement targets MUST be left unchanged.
- **VT-DOM-532.** The fuzzy path MUST only run on a complete word (e.g. `комит2` MUST NOT match).
- **VT-DOM-533.** Documented honest limits (they MUST NOT be claimed as handled): `кивот→kilocode`
  (similarity 0.38), `десыч→dsh` (0.50) and stuck-together words (`опикило` for `апи килокод`)
  are only fixed by an explicit pair, and a pair supports phrases on both sides
  (`опикило=API kilocode`).
- **VT-DOM-534.** `TermsDictionaryPort` MUST read the live settings dictionary on **every** call
  (so a change applies without a reload), MUST return the engine error unchanged, and MUST NOT
  skip replacement merely because there are no explicit pairs.

---

## 6. Text output (`VT-DOM-6xx`)

Contracts: [src/domain/text_output.hpp](../../src/domain/text_output.hpp),
[src/platform/api/clipboard.hpp](../../src/platform/api/clipboard.hpp),
[src/platform/api/paste.hpp](../../src/platform/api/paste.hpp); test: `text-output-contract`.

- **VT-DOM-601.** A blank or annotation-only transcript MUST be a deliberate no-op: neither the
  clipboard nor the paste MUST be touched, and `output()` MUST return `false`.
  Blankness checks Unicode whitespace (`0x09,0x0A,0x0B,0x0C,0x0D,0x20,0x85,0xA0,0x1680,`
  `0x2000..0x200A,0x2028,0x2029,0x202F,0x205F,0x3000`) and MUST tolerate broken UTF-8.
- **VT-DOM-602.** A non-empty transcript MUST always be written to the clipboard. The clipboard
  is authoritative and MUST be written **before** the paste delay and before any injection.
- **VT-DOM-603.** Automatic pasting MUST wait `kPasteDelay = 80 ms` and then inject the
  `Ctrl+V` chord once.
- **VT-DOM-604.** Clipboard writes MUST be retried up to `kClipboardMaxAttempts = 5` times with
  `kClipboardRetryDelayMs = 120 ms` between attempts (`RetryingClipboard`).
- **VT-DOM-605.** A clipboard-write failure MUST fail the whole output and MUST NOT attempt a
  paste.
- **VT-DOM-606.** The clipboard is delivered as `clipboard_only` — a **success** — when automatic
  pasting is off, when pasting is suspended for hotkey capture, when the backend reports no
  injection support, or when the sleep before the paste is cancelled. A paste failure after a
  successful clipboard write MUST also end as `clipboard_only`, never as a failed dictation.
- **VT-DOM-607.** `OutputReport` MUST be published with the outcome
  (`pasted | clipboard_only | skipped_empty | clipboard_failed`), `delivered`, and optional
  `PasteDiagnostics{reason, injection_supported, clipboard_holds_text}`.
  Known `reason` values: `pasted`, `suspended_for_hotkey_capture`, `no_injection_backend`,
  `cancelled_before_paste`, `executor_unavailable`, `integrity_level`,
  `window_rejected_input`, default `paste_failed`.
- **VT-DOM-608.** The paste MUST run on the GUI/input thread: inline when already on it,
  otherwise through `Executor::invoke(..., 2000 ms)`; an executor failure MUST degrade to
  `clipboard_only` with reason `executor_unavailable`.
- **VT-DOM-609.** `TextOutputService` MUST be single-writer: a second concurrent call MUST fail
  with `invalid_state` and message `"a text delivery is already in flight"`.
- **VT-DOM-610.** Cancellation before the clipboard write MUST return `cancelled`; cancellation
  after it MUST NOT roll the clipboard back and MUST NOT undo a successful paste.
- **VT-DOM-611.** A UAC integrity mismatch (paste blocked while the clipboard holds the text)
  MUST be reported as `integrity_level` and the text MUST still be on the clipboard
  (VT-SYS-004).
- **VT-DOM-612.** Exceptions thrown by a report sink MUST be swallowed.
- **G-2.** `clipboard_busy` is declared but never produced; `paste_failed` is only a default.
  Which reasons are contractual is not pinned by a test.

---

## 7. Audio (`VT-DOM-7xx`)

Contracts: [src/domain/audio_format.hpp](../../src/domain/audio_format.hpp),
[src/domain/audio_wav.hpp](../../src/domain/audio_wav.hpp); tests: `audio-dsp-contract`, `noise-suppression-contract`.

- **VT-DOM-701.** The recognition format MUST be PCM16, mono, 16 kHz
  (`kWhisperSampleRate = 16000`). Device audio MUST be downmixed (channel average, an odd last
  sample dropped) and resampled to 16 kHz before recognition.
- **VT-DOM-702.** `AudioFormat::validate()` MUST reject a zero sample rate, zero channels or an
  unknown sample format; `validate_buffer()` MUST additionally require a whole number of frames.
- **VT-DOM-703.** `SampleBuffer` MUST default to `960000` samples capacity
  (`16000 * 60`) and MUST return `resource_exhausted` on overflow instead of truncating.
  A single append MUST be bounded by `kMaxRecordingBytes = 512 MiB`, counting what is already
  accumulated (D7).
- **VT-DOM-704.** The WAV reader MUST require RIFF/WAVE with a `fmt ` chunk of at least 16 bytes
  and a `data` chunk, MUST advance by `8 + size + (size & 1)`, and MUST reject
  (as `unsupported`) non-PCM, non-16-bit, non-mono or non-16 kHz input — in that check order.
- **VT-DOM-705.** The WAV writer MUST refuse a sample rate other than 16 kHz
  (`invalid_argument`), MUST emit exactly a 44-byte header, MUST clamp samples to `[-1, 1]` and
  MUST quantize with `lround(x * 32767)`.
- **VT-DOM-706.** `resample_to_16k()` MUST be a documented linear fractional resampler with tail
  preservation; `source_rate == 16000` MUST return a copy.
- **VT-DOM-707.** Noise suppression MUST be optional and off by default; when enabled it MUST
  use 256-sample frames, a one-pole high-pass with coefficient 0.94, an adaptive floor with
  threshold `floor * 2.0`, damp gain 0.6, gain smoothing `gain*0.7 + target*0.3`, and MUST apply
  gain only when it is below 0.999. It MUST NOT clip loud speech.
- **VT-DOM-708.** `trim_silence()` (the RMS path) MUST use 160-sample frames, a threshold of
  `max(percentile_15 * 3, 1e-6)` with a `0.005` fallback, and MUST return an empty vector when no
  frame is active. Its default margin remains 0.25 s (⚠ gap G-5).

---

## 8. Hotkey and gamepad grammar (`VT-DOM-8xx`)

Contracts: [src/domain/hotkey_gesture.hpp](../../src/domain/hotkey_gesture.hpp),
[src/domain/gamepad_binding.hpp](../../src/domain/gamepad_binding.hpp).

### 8.1 Hotkeys

- **VT-DOM-801.** Modifier tokens MUST be case-insensitive: `Ctrl`, `Control`, `Alt`, `Shift`,
  `Win`, `Windows`, `Meta`, `Super`, `Cmd`.
- **VT-DOM-802.** Formatting MUST emit the canonical modifier order `Ctrl, Alt, Shift, Win`
  (`Ctrl+Alt+Space`).
- **VT-DOM-803.** The key component MUST be normalized by upper-casing only its first character
  when that character is an ASCII lowercase letter (`Numpad0` stays `Numpad0`).
- **VT-DOM-804.** `parse_hotkey()` MUST accept a string with no modifier; the stricter rule
  ("a modifier is required") belongs to UI capture through `is_capturable()`, which also allows
  a bare function key `F1..F24`.
- **VT-DOM-805.** `parse_hotkey()` MUST reject, with a specific `invalid_argument` message: an
  empty string, an empty component (double `+` or a trailing `+`), a second non-modifier
  component, and a missing key component.
- **VT-DOM-806.** Supported gesture examples that MUST round-trip: `Ctrl+Alt+Space`, `F12`,
  `NumPad0`, `OemTilde`.

### 8.2 Gamepads

- **VT-DOM-810.** An XInput binding MUST be `XInput|<Button>` with `<Button>` from the closed,
  case-insensitive list `A,B,X,Y,LB,RB,LT,RT,DPadUp,DPadDown,DPadLeft,DPadRight,Start,Back,`
  `LeftStick,RightStick` (16 values, .NET order).
- **VT-DOM-811.** A DirectInput binding MUST be exactly `DInput|<ProductName>|<zeroBasedIndex>`
  (3 parts); a product name containing `|` is not representable under this grammar.
- **VT-DOM-812.** `GamepadBinding::to_string()` MUST emit `""` for `none`, `"XInput|<id>"` or
  `"DInput|<id>"`. An unassigned binding is valid input.

---

## 9. Paths, logger, CPU (`VT-DOM-9xx`)

- **VT-DOM-901.** `windows_app_path_roots(env, application_directory)` MUST read `APPDATA` for
  roaming and `LOCALAPPDATA` for local; an empty variable MUST count as unset; with both unset
  both roots MUST fall back to the executable directory, with one unset the other MUST be used
  for both (VT-SYS-066/067).
- **VT-DOM-902.** `AppPaths` MUST implement every `platform::Paths` accessor and MUST be the only
  place that assembles a product path.
- **VT-DOM-903.** `FileLogger` MUST write `YYYY-MM-DD HH:MM:SS.mmm [LEVEL] message` in local time
  with the detail on the following line, MUST rotate **before** an append once the current file
  reaches `kLogRotateThresholdBytes = 1'000'000`, MUST keep `kLogArchiveCount = 5` archives
  (`voiceTyper.1.log` … `voiceTyper.5.log`, oldest removed), MUST support
  `clear_on_start` (default true), and MUST return the last `max_lines` lines from `tail()`
  (`kLogViewTailLines = 200`, polled every `kLogViewPollIntervalMs = 800`).
- **VT-DOM-904.** A logging failure MUST NOT propagate (VT-SYS-072), and recognized text MUST
  never be logged (VT-SYS-073).
- **VT-DOM-905.** `make_cpu_topology(logical, physical = 0)` MUST clamp `logical = 0` to 1 and,
  when physical cores are unknown, MUST use `max(1, logical / 2)` and mark them unknown. Thread
  clamps (`kInferenceThreadsMin/Max = 1/16`, `kVadThreadsMin/Max = 2/8`) live in the platform
  contract and MUST NOT be duplicated in the domain.

---

## 10. Requirement-to-evidence map

| Requirement block | Contract test |
|---|---|
| error, cancellation | `domain-smoke`, `platform-contract-smoke` |
| settings schema and codec | `settings-json-contract`, `settings-presenter-contract`, `fixture-check` |
| state machine | `recording-state-machine-contract` |
| VAD | `vad-contract`, `silero-vad-contract` |
| segmentation and trimming | `speech-segments-contract`, `silence-trimming-contract` |
| terms dictionary | `terms-dictionary-contract` |
| text output | `text-output-contract` |
| audio | `audio-dsp-contract`, `noise-suppression-contract` |
| paths/logger/CPU | `core-support-contract` |
