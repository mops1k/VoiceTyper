#pragma once

// Win32 backends behind platform/api/text_target.hpp: the UI Automation probe
// (VT-PLT-506..509) and the SendInput Unicode typer (VT-PLT-510..511).
//
// This header stays standard C++20 (no <windows.h>), like the other Win32
// backend headers, so it can be included and linked on any host: everything that
// needs the OS sits behind the .cpp's `#if defined(_WIN32)`.
//
// The verdict predicate is a pure function here on purpose. That is the part a
// contract test can pin down without a live desktop, and it is the part that
// decides whether a dictation is typed straight into the field or falls back to
// the clipboard. The predicate is deliberately conservative: an element shape
// that was not measured to be editable is reported `unknown`, which sends the
// delivery down the clipboard path. A wrong `editable` would type into nothing
// and lose the transcript (nothing would be on the clipboard either), while a
// wrong `unknown` only costs the user a paste.
//
// Measured shapes (tools/focus-spike.md, 2026-10-16): Brave omnibox and page
// inputs = Edit + writable value pattern; Brave contenteditable and Notepad =
// Document + writable value pattern; Explorer text box and rename field = Edit;
// Windows Terminal = Text with a text pattern; Explorer list and tree items,
// tray buttons and Qt push buttons = a value pattern *only*, and they are not
// text fields.

#include "platform/api/text_target.hpp"

#include <cstdint>

namespace voicetyper::platform {

/// UIA control type identifiers the predicate uses: `UIA_EditControlTypeId`,
/// `UIA_DocumentControlTypeId`, `UIA_TextControlTypeId` (uiautomationclient.h).
/// Spelled out here so the header needs no <uiautomation.h>.
inline constexpr long kUiaControlTypeEdit = 50004;
inline constexpr long kUiaControlTypeText = 50020;
inline constexpr long kUiaControlTypeDocument = 50030;

/// MSAA role/state bits: `ROLE_SYSTEM_TEXT` and `STATE_SYSTEM_READONLY` (oleacc.h).
inline constexpr long kLegacyRoleSystemText = 42;
inline constexpr long kLegacyStateSystemReadOnly = 0x40;

/// The observations of the focused element that decide the verdict.
struct FocusedElementTraits {
    long control_type = -1;
    bool has_control_type = false;
    bool password = false;
    bool text_pattern = false;
    bool value_pattern = false;
    bool value_read_only = false;
    bool value_read_only_known = false;
    bool legacy_role_known = false;
    long legacy_role = -1;
    long legacy_state = 0;
};

/// The predicate of VT-PLT-507: which observations mean "a field that accepts
/// typed characters".
[[nodiscard]] inline TextTargetInfo classify_focused_element(const FocusedElementTraits& traits) noexcept
{
    TextTargetInfo result;

    const auto control_type_is = [&traits](long type) {
        return traits.has_control_type && traits.control_type == type;
    };
    // A read-only value pattern is the one signal that turns an otherwise
    // editable shape into a refusal (a read-only view, a locked field).
    const bool read_only_value =
        traits.value_pattern && traits.value_read_only_known && traits.value_read_only;

    if (traits.password) {
        result.kind = TextTargetKind::read_only;
        result.reason = "password";
    } else if (control_type_is(kUiaControlTypeEdit)) {
        // Measured: browser address bars and page inputs, Explorer search and
        // rename fields, the Windows Search box.
        result.kind = read_only_value ? TextTargetKind::read_only : TextTargetKind::editable;
        result.reason = read_only_value ? "read_only_edit" : "control_type_edit";
    } else if (control_type_is(kUiaControlTypeDocument)) {
        // A document is only a typing target when it also exposes a writable
        // value pattern: contenteditable (Brave) and Notepad do, a plain page
        // body does not, and typing into a plain page would lose the transcript.
        if (read_only_value) {
            result.kind = TextTargetKind::read_only;
            result.reason = "read_only_document";
        } else if (traits.value_pattern) {
            result.kind = TextTargetKind::editable;
            result.reason = "control_type_document";
        } else {
            result.kind = TextTargetKind::unknown;
            result.reason = "document_without_value_pattern";
        }
    } else if (control_type_is(kUiaControlTypeText) && traits.text_pattern) {
        // Measured: Windows Terminal's TermControl.
        result.kind = TextTargetKind::editable;
        result.reason = "text_with_pattern";
    } else if (traits.legacy_role_known && traits.legacy_role == kLegacyRoleSystemText) {
        const bool legacy_read_only = (traits.legacy_state & kLegacyStateSystemReadOnly) != 0;
        result.kind = legacy_read_only ? TextTargetKind::read_only : TextTargetKind::editable;
        result.reason = legacy_read_only ? "legacy_read_only" : "legacy_text_role";
    } else if (traits.text_pattern) {
        // A text pattern on a shape that was not measured as a typing target: not
        // editable enough to type into, but not provably a non-target either.
        result.kind = TextTargetKind::unknown;
        result.reason = "unrecognized_text_shape";
    } else if (traits.value_pattern) {
        // Deliberate (VT-PLT-507): a value pattern alone is not text editability.
        // Explorer list and tree items, tray buttons and Qt push buttons expose
        // one, and typing into them does nothing.
        result.kind = TextTargetKind::none;
        result.reason = "value_pattern_only";
    } else {
        result.kind = TextTargetKind::none;
        result.reason = "no_text_capability";
    }
    return result;
}

/// UI Automation probe. `probe()` may be called from any thread: it runs the COM
/// work on its own short-lived MTA thread, because UI Automation needs an
/// apartment and the caller may well be an STA UI thread.
///
/// Cost: two queries (VT-PLT-508) plus the 150 ms between them. Each query is
/// bounded by kTextTargetProbeTimeout, so the worst case is a few seconds when a
/// provider is hung; the measured case is a couple of milliseconds.
class Win32TextTargetProbe final : public TextTargetProbe {
public:
    Win32TextTargetProbe() = default;
    ~Win32TextTargetProbe() override;

    Win32TextTargetProbe(const Win32TextTargetProbe&) = delete;
    Win32TextTargetProbe& operator=(const Win32TextTargetProbe&) = delete;
    Win32TextTargetProbe(Win32TextTargetProbe&&) = delete;
    Win32TextTargetProbe& operator=(Win32TextTargetProbe&&) = delete;

    [[nodiscard]] TextTargetInfo probe() override;

    /// UI Automation is part of the OS, so the backend is always present here.
    /// A per-call failure (a denied or hung provider) is reported as `unknown`
    /// by probe() instead.
    [[nodiscard]] bool is_supported() const noexcept override;

    /// The last query's error, for the diagnostic; 0 after a successful probe.
    [[nodiscard]] unsigned long last_error() const noexcept;

private:
    unsigned long last_error_ = 0;
};

/// Types text into the focused window with `SendInput`/`KEYEVENTF_UNICODE`.
/// Takes the keyboard, so it MUST run on the input-owning thread (VT-PLT-510).
class Win32DirectTextInjector final : public DirectTextInjector {
public:
    Win32DirectTextInjector() = default;
    ~Win32DirectTextInjector() override;

    Win32DirectTextInjector(const Win32DirectTextInjector&) = delete;
    Win32DirectTextInjector& operator=(const Win32DirectTextInjector&) = delete;
    Win32DirectTextInjector(Win32DirectTextInjector&&) = delete;
    Win32DirectTextInjector& operator=(Win32DirectTextInjector&&) = delete;

    [[nodiscard]] InsertResult insert(
        std::string_view utf8, const CancellationToken& cancellation) override;

    /// True: Unicode injection is in-box. A refusal by the focused window is
    /// reported per call (permission_denied for a higher integrity level).
    [[nodiscard]] bool is_supported() const noexcept override;

    /// The last injection's error, for the diagnostic; 0 when nothing failed.
    [[nodiscard]] unsigned long last_error() const noexcept;

private:
    unsigned long last_error_ = 0;
};

/// UI Automation reader of the focused field (VT-PLT-516). The domain uses it to
/// confirm a `Ctrl+V` that the paste backend only queued: on 2026-10-19 a Windows
/// Terminal window accepted the chord and showed nothing, and the caller had no way
/// to notice.
///
/// Read-only by construction: it takes a bounded slice of the field and never
/// writes it, selects anything or moves the caret.
class Win32FocusedTextField final : public FocusedTextField {
public:
    Win32FocusedTextField() = default;
    ~Win32FocusedTextField() override;

    Win32FocusedTextField(const Win32FocusedTextField&) = delete;
    Win32FocusedTextField& operator=(const Win32FocusedTextField&) = delete;
    Win32FocusedTextField(Win32FocusedTextField&&) = delete;
    Win32FocusedTextField& operator=(Win32FocusedTextField&&) = delete;

    [[nodiscard]] bool read_caret_tail(std::size_t units, std::string* utf8) override;
    [[nodiscard]] bool read_value(std::string* utf8) override;
    [[nodiscard]] bool has_text_input_in_foreground_window() override;

    /// UI Automation is part of the OS: the reader is always present here. A
    /// per-call failure (a field without a text pattern) is reported by the read
    /// itself returning false.
    [[nodiscard]] bool is_supported() const noexcept override;
};

} // namespace voicetyper::platform
