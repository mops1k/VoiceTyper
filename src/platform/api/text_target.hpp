#pragma once

// Direct text delivery: "is the caret in a text field?" and "type there without
// the clipboard" contract.
//
// Evidence: measured with tools/focus-spike.cpp on 2026-10-16 (Windows 11; Brave
// 1.x/Chromium, Notepad, Explorer, Windows Search, Windows Terminal). The focused
// element of a browser text field reports ControlType `Edit` with
// `UIA_IsTextPatternAvailablePropertyId` and a writable value pattern; typing
// through `SendInput` with `KEYEVENTF_UNICODE` delivered 49/49 characters
// (Cyrillic, Latin, digits, punctuation) and `GetClipboardSequenceNumber()` did
// not change. Provider details: Chromium exposes `UIA_TextPatternId` for
// `IsTextField()` (ui/accessibility/platform/ax_platform_node_win.cc) and builds
// its accessibility tree while answering the first UIA query
// (content/browser/renderer_host/legacy_render_widget_host_win.cc, `UiaRootObjectId`).
// The full matrix is in tools/focus-spike.md.
//
// Frozen observable contract (Windows-first stage; the normative requirements are
// `VT-PLT-506..` in specs/modules/platform.spec.md and `VT-DOM-613..` in
// specs/modules/domain.spec.md):
//   * A probe verdict is never a guess. When the backend cannot answer it MUST
//     report `unknown`, which is a different state from `none` ("focus is
//     provably not in a text field").
//   * A value pattern alone is NOT text editability: Explorer list and tree items,
//     tray buttons and Qt push buttons expose one (measured 2026-10-16).
//   * The Windows probe re-probes once after kTextTargetReprobeDelay, because the
//     first query is what makes Chromium build its accessibility tree.
//   * Insertion MUST NOT touch the clipboard. That is the point of the port: a
//     dictation must leave the user's clipboard alone.
//   * Insertion is best effort, exactly like the paste: a refusal is a normal
//     state the caller turns into a fallback, never an error the user cannot act
//     on. A refusal after some characters were typed MUST be reported as a
//     partial result, so the caller can decide not to duplicate the prefix.
//   * A burst that was fully posted but could not be confirmed MUST also be
//     reported as a partial result (`via == InsertVia::unconfirmed`), never as
//     "nothing was delivered": the fallback paste would duplicate a delivery the
//     target had simply not exposed yet (measured 2026-10-20, VT-PLT-519).
//
// Thread affinity: probe() may run on a worker thread (a backend MUST bound every
// platform query by kTextTargetProbeTimeout); insert() takes the keyboard, so it
// MUST run on the thread that owns input handling, like PasteSimulator::paste().
//
// Ownership: stateless; the backends hold no text.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace voicetyper::platform {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::Status;

/// Upper bound a backend MUST put on one platform query of the focused element.
/// A provider that does not answer (a hung application, a browser building its
/// accessibility tree) must not stall the probe.
inline constexpr std::chrono::milliseconds kTextTargetProbeTimeout{2000};

/// Delay before the confirming second probe. Measured 2026-10-16: the verdict is
/// already correct on the first query in current Brave/Chromium, but that query
/// is also the one that activates the accessibility tree, so the second probe is
/// what makes the result stable across Chromium versions and the emulation layer.
inline constexpr std::chrono::milliseconds kTextTargetReprobeDelay{150};

/// What the focused UI element can accept right now.
enum class TextTargetKind : std::uint8_t {
    /// The backend could not answer (no accessibility provider, access denied, a
    /// timed-out query). Typing MAY still work; only the knowledge is missing.
    unknown = 0,
    /// Focus is provably not in anything text-editable (desktop, taskbar, a
    /// button, a list item): typing would land nowhere.
    none = 1,
    /// Text-editable but refuses typing: password fields and read-only views.
    read_only = 2,
    /// A text field that accepts typed characters.
    editable = 3,
};

[[nodiscard]] constexpr std::string_view text_target_kind_name(TextTargetKind kind) noexcept
{
    switch (kind) {
    case TextTargetKind::unknown: return "unknown";
    case TextTargetKind::none: return "none";
    case TextTargetKind::read_only: return "read_only";
    case TextTargetKind::editable: return "editable";
    }
    return "unknown";
}

/// One probe result.
struct TextTargetInfo {
    TextTargetKind kind = TextTargetKind::unknown;

    /// Short machine-readable reason that produced `kind`, for the log and the
    /// delivery diagnostics (`text_pattern`, `control_type`, `value_pattern`,
    /// `password`, `no_text_capability`, `unavailable`, ...). The backend MUST
    /// point it at a string literal with static storage duration.
    std::string_view reason = "unprobed";

    /// Window handle the probe looked at, 0 when the provider does not expose one.
    /// Chromium keeps web content in the renderer, so 0 is normal there and is
    /// not a failure.
    std::uintptr_t window = 0;

    /// Caret geometry when the platform reports it (Windows: the GUI thread
    /// caret, mapped to screen coordinates, best effort). Optional: a backend
    /// that cannot report it leaves the flag false.
    bool has_caret_rect = false;
    int caret_x = 0;
    int caret_y = 0;
    int caret_width = 0;
    int caret_height = 0;

    /// True only for a field that accepts typed characters. `read_only` and
    /// `unknown` are deliberately false: the caller falls back instead of typing
    /// into a password box.
    [[nodiscard]] constexpr bool accepts_typing() const noexcept
    {
        return kind == TextTargetKind::editable;
    }

    /// True when the verdict came from the platform rather than from a failure to
    /// ask it. `unknown` MUST NOT be treated as `none`.
    [[nodiscard]] constexpr bool is_conclusive() const noexcept
    {
        return kind != TextTargetKind::unknown;
    }
};

/// Answers "would typing land in a text field right now?".
class TextTargetProbe {
public:
    virtual ~TextTargetProbe() = default;

    TextTargetProbe(const TextTargetProbe&) = delete;
    TextTargetProbe& operator=(const TextTargetProbe&) = delete;
    TextTargetProbe(TextTargetProbe&&) = delete;
    TextTargetProbe& operator=(TextTargetProbe&&) = delete;

    /// Probes the focused element once. Never throws; failures are reported as
    /// `TextTargetKind::unknown` with the reason, because the caller's decision
    /// ("fall back to the clipboard") is the same either way.
    [[nodiscard]] virtual TextTargetInfo probe() = 0;

    /// True when this session has a probe backend at all. A false value is a
    /// diagnostic hint, not a promise: a permission failure can still turn a
    /// single probe into `unknown`.
    [[nodiscard]] virtual bool is_supported() const noexcept = 0;

protected:
    TextTargetProbe() = default;
};

/// Which delivery path actually carried the text. Observability only: the caller may log it and
/// must not branch on it beyond the "no confirmation" case below, because a path that works for a
/// target today may not be the one that works after an update of that target.
enum class InsertVia : std::uint8_t {
    /// Nothing was delivered.
    none = 0,
    /// Posted `WM_CHAR` messages, confirmed by reading the field back (VT-PLT-515).
    posted = 1,
    /// The whole burst was posted, but the field never confirmed it inside the
    /// confirmation window (VT-PLT-519). The text MAY be in the field: the target
    /// updates its accessible text asynchronously, and a measurement on the target
    /// machine (2026-10-20) showed confirmations arriving 300-450 ms after the post.
    /// The caller MUST NOT paste over such a delivery, because a fallback paste is
    /// exactly what inserts the transcript twice.
    unconfirmed = 2,
};

[[nodiscard]] inline constexpr std::string_view insert_via_name(InsertVia value) noexcept
{
    switch (value) {
    case InsertVia::none:
        return "none";
    case InsertVia::posted:
        return "posted";
    case InsertVia::unconfirmed:
        return "unconfirmed";
    }
    return "unknown";
}

/// Outcome of one direct insertion attempt.
struct InsertResult {
    /// `ok` when the whole text was typed. Failure codes: `permission_denied`
    /// (the focused window runs at a higher integrity level), `unavailable` (no
    /// backend, or the target refused the synthetic input), `cancelled` (the
    /// caller stopped the delivery).
    Status status = Status::success();

    /// How many units may have reached the target (Windows: UTF-16 code units).
    /// Only the zero/non-zero distinction is contractual: `status.is_error() &&
    /// typed > 0` is a delivery that a fallback paste would duplicate. A burst
    /// that was fully posted and then not confirmed is therefore reported with
    /// `typed > 0` and `via == InsertVia::unconfirmed`, never as `typed == 0` - an
    /// "honest" zero there is what produced the 2026-10-20 duplicate (VT-PLT-519).
    std::size_t typed = 0;

    /// Which path carried the text, for the diagnostic (VT-PLT-515).
    InsertVia via = InsertVia::none;

    /// True when the attempt failed after part of the text had been typed, or
    /// after the whole text was posted but could not be confirmed.
    [[nodiscard]] bool is_partial() const noexcept { return status.is_error() && typed > 0; }
};

/// Types text into whatever currently has the keyboard focus, without the
/// clipboard.
class DirectTextInjector {
public:
    virtual ~DirectTextInjector() = default;

    DirectTextInjector(const DirectTextInjector&) = delete;
    DirectTextInjector& operator=(const DirectTextInjector&) = delete;
    DirectTextInjector(DirectTextInjector&&) = delete;
    DirectTextInjector& operator=(DirectTextInjector&&) = delete;

    /// Types `utf8` into the focused element once. Must not clear, read or
    /// otherwise disturb the clipboard.
    ///
    /// The text is UTF-8 and may contain any script: the Windows backend maps it
    /// to UTF-16 code units and sends them as Unicode input, so no keyboard
    /// layout is involved. Implementations must stop and report `cancelled` when
    /// the token is set.
    [[nodiscard]] virtual InsertResult insert(
        std::string_view utf8, const CancellationToken& cancellation) = 0;

    /// True when direct insertion is expected to work in this session. A false
    /// value is a diagnostic hint, not a guarantee: the focused window's
    /// integrity level and its input policy can still refuse the events.
    [[nodiscard]] virtual bool is_supported() const noexcept = 0;

protected:
    DirectTextInjector() = default;
};

/// Reads the focused field back, so a delivery made by someone else can still be
/// confirmed.
///
/// Why this port exists: `PasteSimulator::paste()` sends a synthetic `Ctrl+V` and
/// learns only that the input stream accepted the chord — measured 2026-10-19, a
/// Windows Terminal window accepted the chord and showed nothing, and the caller
/// reported "pasted" anyway. The domain owns the transcript, so it can do what the
/// injector does for its own delivery: look at the field before claiming success
/// (VT-PLT-516).
///
/// Thread affinity: like `insert()`, it runs on the thread that owns input
/// handling, immediately after the paste.
class FocusedTextField {
public:
    virtual ~FocusedTextField() = default;

    FocusedTextField(const FocusedTextField&) = delete;
    FocusedTextField& operator=(const FocusedTextField&) = delete;
    FocusedTextField(FocusedTextField&&) = delete;
    FocusedTextField& operator=(FocusedTextField&&) = delete;

    /// At most `units` characters immediately before the caret, encoded as UTF-8.
    /// False when the focused element exposes no text pattern, no selection, or
    /// nothing to anchor the read at: the caller must treat that as "cannot tell",
    /// never as "empty field".
    ///
    /// A non-empty selection is a trap for this read (VT-DOM-620): the text is
    /// anchored at the selection's end, but a delivery replaces the whole
    /// selection, so the characters before a selection are not the characters the
    /// transcript will continue. A caller that wants the text a delivery appends to
    /// MUST consult `selection_present()` first.
    [[nodiscard]] virtual bool read_caret_tail(std::size_t units, std::string* utf8) = 0;

    /// The whole value of the focused element, as UTF-8. False when it has no value
    /// pattern (documents and consoles have none).
    [[nodiscard]] virtual bool read_value(std::string* utf8) = 0;

    /// True when the focused element holds a non-empty text selection, i.e. the next
    /// inserted text replaces it instead of being appended at a caret (VT-DOM-620).
    /// False only when the caret is known to be collapsed; a backend that cannot read
    /// the selection MUST report true, because a caller must not guess at the text a
    /// selection is about to replace.
    [[nodiscard]] virtual bool selection_present() = 0;

    /// True when the foreground window holds at least one text-capable element,
    /// whether or not it is the focused one.
    ///
    /// Why it exists: a paste that cannot be read back is not proof of failure - a
    /// plugin editor can be invisible to UI Automation while the paste works. A window
    /// with no text-capable element anywhere, on the other hand, cannot have received
    /// the text: measured 2026-10-19, dictating on the desktop left the transcript
    /// uninserted and the user had no way to know. The search is bounded, and a
    /// backend that cannot answer MUST return true (silence) rather than guess.
    [[nodiscard]] virtual bool has_text_input_in_foreground_window() = 0;

    /// False on a platform whose backends cannot answer at all. The caller then
    /// keeps the pre-existing behaviour instead of inventing a verdict: the Linux
    /// stage has no accessibility reader yet, and a false "unconfirmed" on every
    /// paste would be worse than the old silence.
    [[nodiscard]] virtual bool is_supported() const noexcept = 0;

protected:
    FocusedTextField() = default;
};

} // namespace voicetyper::platform
