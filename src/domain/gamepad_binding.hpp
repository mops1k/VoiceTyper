#pragma once

// Gamepad binding contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §6 "Gamepads" and
// VoiceTyper.Core/Models/GamepadBinding.cs +
// VoiceTyper.Core/Services/GamepadBindingParser.cs / GamepadBindingMatcher.cs.
//
// String grammar (unchanged from the .NET build):
//   * XInput:      "XInput|<Button>", e.g. "XInput|A"; the button name is
//                  validated against XInputPadButton.
//   * DirectInput: "DInput|<ProductName>|<zeroBasedButtonIndex>", e.g.
//                  "DInput|Logitech|3" — exactly three '|'-separated parts.
//   * A DirectInput product name that itself contains '|' is not representable
//                  under the current grammar. The limitation is documented here
//                  so that any future parser migration is a deliberate, tested
//                  change rather than an accident.
//
// Enumerators keep the .NET ordinal values (declaration order), because the
// differential harness compares them across languages.

#include "domain/error.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace voicetyper::domain {

/// Where a gamepad button came from.
enum class GamepadSource : std::uint8_t {
    /// Not set. A binding with this source is "unassigned".
    none = 0,
    /// Xbox-compatible controller via XInput. Indices 0..3.
    xinput = 1,
    /// Arbitrary joystick/gamepad via DirectInput.
    directinput = 2,
};

[[nodiscard]] constexpr std::string_view gamepad_source_name(GamepadSource source) noexcept
{
    switch (source) {
    case GamepadSource::none: return "none";
    case GamepadSource::xinput: return "xinput";
    case GamepadSource::directinput: return "directinput";
    }
    return "unknown";
}

/// Fixed XInput pad buttons. Values match the .NET `XInputPadButton` ordinals.
enum class XInputPadButton : std::uint8_t {
    a = 0,
    b = 1,
    x = 2,
    y = 3,
    lb = 4,
    rb = 5,
    lt = 6,
    rt = 7,
    dpad_up = 8,
    dpad_down = 9,
    dpad_left = 10,
    dpad_right = 11,
    start = 12,
    back = 13,
    left_stick = 14,
    right_stick = 15,
};

/// Every valid XInput button name, in the .NET declaration order.
inline constexpr std::array<std::string_view, 16> kXInputButtonNames{
    "A", "B", "X", "Y", "LB", "RB", "LT", "RT",
    "DPadUp", "DPadDown", "DPadLeft", "DPadRight",
    "Start", "Back", "LeftStick", "RightStick",
};

/// Maps a button to its wire name. Returns nullopt for an out-of-range value.
[[nodiscard]] constexpr std::optional<std::string_view> x_input_button_name(XInputPadButton button) noexcept
{
    const auto index = static_cast<std::size_t>(button);
    if (index >= kXInputButtonNames.size()) {
        return std::nullopt;
    }
    return kXInputButtonNames[index];
}

namespace detail {

[[nodiscard]] inline char lower_ascii(char ch) noexcept
{
    const auto byte = static_cast<unsigned char>(ch);
    return (byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte - 'A' + 'a') : ch;
}

/// ASCII-only case-insensitive equality. Key/button names are ASCII, so a
/// locale-independent compare is both sufficient and reproducible across
/// Windows and Linux, which a differential harness requires.
[[nodiscard]] inline bool equals_ignore_ascii_case(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lower_ascii(lhs[i]) != lower_ascii(rhs[i])) {
            return false;
        }
    }
    return true;
}

} // namespace detail

/// Maps a wire name back to a button. Comparison is case-insensitive, matching
/// the case-insensitive binding comparison in the .NET matcher.
[[nodiscard]] inline std::optional<XInputPadButton> x_input_button_from_name(std::string_view name)
{
    for (std::size_t i = 0; i < kXInputButtonNames.size(); ++i) {
        if (detail::equals_ignore_ascii_case(name, kXInputButtonNames[i])) {
            return static_cast<XInputPadButton>(i);
        }
    }
    return std::nullopt;
}

/// A button press observed by the gamepad poll loop.
struct GamepadInput {
    GamepadSource source = GamepadSource::none;
    /// XInput: the button name ("A", "LB", ...).
    /// DirectInput: "<productName>|<index>".
    std::string button_id;

    friend bool operator==(const GamepadInput& lhs, const GamepadInput& rhs) noexcept
    {
        return lhs.source == rhs.source && lhs.button_id == rhs.button_id;
    }
};

/// A gamepad button bound to an action (record / cancel), in the
/// platform-independent form used by settings and the poll loop.
struct GamepadBinding {
    GamepadSource source = GamepadSource::none;
    /// XInput: the button name ("A", "LB", ...).
    /// DirectInput: "<productName>|<index>".
    std::string button_id;

    [[nodiscard]] bool is_assigned() const noexcept { return source != GamepadSource::none; }

    /// Renders the settings.json spelling: "XInput|A" or
    /// "DInput|Logitech|3". An unassigned binding renders as an empty string,
    /// which the settings layer writes as JSON null.
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const GamepadBinding& lhs, const GamepadBinding& rhs) noexcept
    {
        return lhs.source == rhs.source && lhs.button_id == rhs.button_id;
    }
};

inline std::string GamepadBinding::to_string() const
{
    switch (source) {
    case GamepadSource::none:
        return {};
    case GamepadSource::xinput:
        return "XInput|" + button_id;
    case GamepadSource::directinput:
        return "DInput|" + button_id;
    }
    return {};
}

/// Grammar token for XInput bindings, as it appears in settings.json.
inline constexpr std::string_view kGamepadXInputToken = "XInput";
/// Grammar token for DirectInput bindings, as it appears in settings.json.
inline constexpr std::string_view kGamepadDirectInputToken = "DInput";
/// Number of '|'-separated parts in a DirectInput binding string.
inline constexpr std::size_t kGamepadDirectInputPartCount = 3;

} // namespace voicetyper::domain
