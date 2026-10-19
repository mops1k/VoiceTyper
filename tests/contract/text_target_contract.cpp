// Direct-delivery contract (VT-PLT-506..513, VT-DOM-613..617).
//
// Two things are pinned here, and both are portable on purpose:
//   * the Windows probe predicate as a pure function, against the shapes that
//     were actually measured with tools/focus-spike.cpp on 2026-10-16 - the
//     editable targets (Brave input, omnibox, contenteditable, Notepad, Explorer
//     text box and rename field, Windows Search, Windows Terminal) and the
//     look-alikes that must NOT be typed into (Explorer list and tree items,
//     tray buttons, Qt push buttons - all of them expose a value pattern);
//   * the no-op portable backend, which is what keeps Linux and a probe-less host
//     on the clipboard-then-paste path (VT-PLT-512) instead of failing.

#include "platform/api/paste.hpp"
#include "platform/api/text_target.hpp"
#include "platform/portable/portable_runtime.hpp"
#include "platform/windows/win32_text_target.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

using namespace voicetyper;
using namespace voicetyper::platform;

/// The measured element shapes, named after the window they came from.
struct Shape {
    const char* name;
    FocusedElementTraits traits;
    TextTargetKind expected;
    const char* expected_reason;
};

FocusedElementTraits make_traits(long control_type, bool text_pattern, bool value_pattern,
    bool value_read_only, long legacy_role)
{
    FocusedElementTraits traits;
    traits.has_control_type = true;
    traits.control_type = control_type;
    traits.text_pattern = text_pattern;
    traits.value_pattern = value_pattern;
    traits.value_read_only_known = true;
    traits.value_read_only = value_read_only;
    traits.legacy_role_known = true;
    traits.legacy_role = legacy_role;
    return traits;
}

void check_predicate()
{
    const Shape shapes[] = {
        // Editable, measured 2026-10-16 (control_type, T, V, RO, role).
        {"brave omnibox", make_traits(kUiaControlTypeEdit, true, true, false, 42),
            TextTargetKind::editable, "control_type_edit"},
        {"brave page input", make_traits(kUiaControlTypeEdit, true, true, false, 42),
            TextTargetKind::editable, "control_type_edit"},
        {"brave contenteditable", make_traits(kUiaControlTypeDocument, true, true, false, 15),
            TextTargetKind::editable, "control_type_document"},
        {"notepad", make_traits(kUiaControlTypeDocument, true, true, false, 15),
            TextTargetKind::editable, "control_type_document"},
        {"explorer search box", make_traits(kUiaControlTypeEdit, true, true, false, 42),
            TextTargetKind::editable, "control_type_edit"},
        {"explorer rename field", make_traits(kUiaControlTypeEdit, false, true, false, 42),
            TextTargetKind::editable, "control_type_edit"},
        {"windows search box", make_traits(kUiaControlTypeEdit, true, true, false, 42),
            TextTargetKind::editable, "control_type_edit"},
        {"windows terminal", make_traits(kUiaControlTypeText, true, false, false, 41),
            TextTargetKind::editable, "text_with_pattern"},

        // Not editable: a value pattern alone is not text editability.
        {"explorer list item", make_traits(50007, false, true, false, 34), TextTargetKind::none,
            "value_pattern_only"},
        {"explorer tree item", make_traits(50024, false, true, false, 36), TextTargetKind::none,
            "value_pattern_only"},
        {"qt push button", make_traits(50000, false, true, false, 43), TextTargetKind::none,
            "value_pattern_only"},
        {"qt radio button", make_traits(50013, false, true, false, 45), TextTargetKind::none,
            "value_pattern_only"},
        {"desktop", make_traits(50007, false, false, false, 34), TextTargetKind::none,
            "no_text_capability"},
        {"taskbar pane", make_traits(50033, false, false, false, 10), TextTargetKind::none,
            "no_text_capability"},

        // Unknown: the shape was not measured as a typing target, so the delivery
        // must fall back to the clipboard instead of typing into nothing.
        {"plain web document", make_traits(kUiaControlTypeDocument, true, false, false, 15),
            TextTargetKind::unknown, "document_without_value_pattern"},
        {"unmeasured text control", make_traits(50016, true, true, false, 52),
            TextTargetKind::unknown, "unrecognized_text_shape"},

        // A read-only shape refuses typing, and a password field is never a target.
        {"read-only edit", make_traits(kUiaControlTypeEdit, true, true, true, 42),
            TextTargetKind::read_only, "read_only_edit"},
        {"read-only document", make_traits(kUiaControlTypeDocument, true, true, true, 15),
            TextTargetKind::read_only, "read_only_document"},
    };

    for (const Shape& shape : shapes) {
        const TextTargetInfo info = classify_focused_element(shape.traits);
        check(info.kind == shape.expected,
            std::string(shape.name) + ": expected " + std::string(text_target_kind_name(shape.expected))
                + ", got " + std::string(text_target_kind_name(info.kind)));
        check(info.reason == shape.expected_reason,
            std::string(shape.name) + ": expected reason " + shape.expected_reason + ", got "
                + std::string(info.reason));
        check(info.accepts_typing() == (shape.expected == TextTargetKind::editable),
            std::string(shape.name) + ": accepts_typing() must follow the verdict");
        if (shape.expected != TextTargetKind::unknown && shape.expected != TextTargetKind::none) {
            check(info.is_conclusive(), std::string(shape.name) + ": a decided shape is conclusive");
        }
    }

    // A password field is read-only whichever shape it wears, because typing into
    // it would put the transcript where the user cannot check it.
    FocusedElementTraits password = make_traits(kUiaControlTypeEdit, true, true, false, 42);
    password.password = true;
    const TextTargetInfo password_info = classify_focused_element(password);
    check(password_info.kind == TextTargetKind::read_only && password_info.reason == "password",
        "a password field is read_only with reason password");

    // The weakest shape of all: nothing to read at all.
    FocusedElementTraits empty;
    const TextTargetInfo empty_info = classify_focused_element(empty);
    check(empty_info.kind == TextTargetKind::none && empty_info.reason == "no_text_capability",
        "an element with no control type and no patterns is none");
    check(empty_info.is_conclusive(), "`none` is a conclusive verdict, unlike `unknown`");
}

void check_outcome_contract()
{
    check(text_output_outcome_name(TextOutputOutcome::direct) == "direct",
        "the direct outcome is named (VT-PLT-513)");
    check(is_delivered(TextOutputOutcome::direct), "a typed delivery counts as delivered");
    check(!is_delivered(TextOutputOutcome::skipped_empty) && !is_delivered(TextOutputOutcome::clipboard_failed),
        "the skipped and failed outcomes stay undelivered");
}

void check_portable_noop()
{
    PortableTextTargetProbe probe;
    check(!probe.is_supported(), "the portable probe declares itself unsupported");
    const TextTargetInfo info = probe.probe();
    check(info.kind == TextTargetKind::unknown, "the portable probe cannot answer");
    check(info.reason == "unsupported_platform", "and says why");
    check(!info.is_conclusive() && !info.accepts_typing(),
        "`unknown` neither accepts typing nor claims to be conclusive (VT-PLT-506)");

    PortableDirectTextInjector injector;
    check(!injector.is_supported(), "the portable injector declares itself unsupported");
    const InsertResult result = injector.insert("hello", {});
    check(result.status.is_error() && result.status.code() == ErrorCode::unavailable,
        "the portable injector reports unavailable instead of pretending to type");
    check(result.typed == 0 && !result.is_partial(),
        "and it never claims a partial delivery, so the fallback cannot duplicate text");
}

} // namespace

int main()
{
    check_predicate();
    check_outcome_contract();
    check_portable_noop();

    if (failures != 0) {
        std::cerr << "text-target-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "text-target-contract: OK\n";
    return 0;
}
