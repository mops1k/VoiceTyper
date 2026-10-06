#pragma once

// Global hotkey contract (keyboard half).
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §6 "Hotkeys" and
// VoiceTyper.Core/Models/HotkeyGesture.cs + VoiceTyper.Core/Services/HotkeyParser.cs.
//
// String grammar (unchanged from the .NET build):
//   * Modifiers are matched case-insensitively and accept these aliases:
//     Ctrl / Control, Alt, Shift, Win / Windows / Meta / Super / Cmd.
//   * Canonical *emission* order is Ctrl, Alt, Shift, Win, joined with '+'.
//   * Key names match System.Windows.Input.Key names, normalized by upper-casing
//     the first character (e.g. "Space", "Escape", "NumPad0", "OemTilde").
//   * The parser accepts a bare key with no modifier, while the UI capture hook
//     additionally *requires* a modifier unless the key is F1..F24.
//   * Reusable examples: "Ctrl+Alt+Space", "F12", "NumPad0", "OemTilde".
//
// Representation plus the grammar itself are pure and portable, so both live
// here. Anything that needs a platform virtual-key code belongs to
// src/platform/api/hotkeys.hpp.

#include "domain/error.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace voicetyper::domain {

/// Modifier bitset. Values are bit-for-bit identical to the .NET
/// `[Flags] HotkeyModifiers` enum, because the differential harness compares
/// them across languages.
enum class HotkeyModifiers : std::uint8_t {
    none = 0,
    alt = 1,
    control = 2,
    shift = 4,
    win = 8,
};

[[nodiscard]] constexpr HotkeyModifiers operator|(HotkeyModifiers lhs, HotkeyModifiers rhs) noexcept
{
    return static_cast<HotkeyModifiers>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr HotkeyModifiers operator&(HotkeyModifiers lhs, HotkeyModifiers rhs) noexcept
{
    return static_cast<HotkeyModifiers>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr HotkeyModifiers operator^(HotkeyModifiers lhs, HotkeyModifiers rhs) noexcept
{
    return static_cast<HotkeyModifiers>(static_cast<std::uint8_t>(lhs) ^ static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr HotkeyModifiers operator~(HotkeyModifiers value) noexcept
{
    return static_cast<HotkeyModifiers>(static_cast<std::uint8_t>(~static_cast<std::uint8_t>(value)));
}

constexpr HotkeyModifiers& operator|=(HotkeyModifiers& lhs, HotkeyModifiers rhs) noexcept
{
    lhs = lhs | rhs;
    return lhs;
}

/// True when every bit of `flag` is present in `set`. `none` is never "present",
/// so callers must test specific modifiers rather than `has_modifier(s, none)`.
[[nodiscard]] constexpr bool has_modifier(HotkeyModifiers set, HotkeyModifiers flag) noexcept
{
    if (flag == HotkeyModifiers::none) {
        return false;
    }
    const auto bits = static_cast<std::uint8_t>(flag);
    return (static_cast<std::uint8_t>(set) & bits) == bits;
}

/// Canonical emission order: Ctrl, Alt, Shift, Win.
inline constexpr std::array<HotkeyModifiers, 4> kCanonicalModifierOrder{
    HotkeyModifiers::control,
    HotkeyModifiers::alt,
    HotkeyModifiers::shift,
    HotkeyModifiers::win,
};

/// Canonical spelling of each entry of kCanonicalModifierOrder, same index.
inline constexpr std::array<std::string_view, 4> kModifierTokenOrder{
    "Ctrl",
    "Alt",
    "Shift",
    "Win",
};

/// Every accepted modifier token, lower-case, with the bit it maps to.
/// Parsing is case-insensitive, so the table is stored lower-case.
inline constexpr std::array<std::pair<std::string_view, HotkeyModifiers>, 9> kModifierAliases{{
    {"ctrl", HotkeyModifiers::control},
    {"control", HotkeyModifiers::control},
    {"alt", HotkeyModifiers::alt},
    {"shift", HotkeyModifiers::shift},
    {"win", HotkeyModifiers::win},
    {"windows", HotkeyModifiers::win},
    {"meta", HotkeyModifiers::win},
    {"super", HotkeyModifiers::win},
    {"cmd", HotkeyModifiers::win},
}};

/// Lower-cases an ASCII token without touching the locale.
[[nodiscard]] inline std::string to_lower_ascii(std::string_view text)
{
    std::string out(text);
    for (char& ch : out) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') {
            ch = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return out;
}

/// Upper-cases the first character of a key name and lower-cases the rest of an
/// ASCII-only key token is *not* applied: the .NET contract only upper-cases the
/// first character, so "NumPad0" and "Numpad0" are both normalized to "NumPad0".
[[nodiscard]] inline std::string normalize_key_token(std::string_view text)
{
    std::string out(text);
    if (out.empty()) {
        return out;
    }
    const auto first = static_cast<unsigned char>(out[0]);
    if (first >= 'a' && first <= 'z') {
        out[0] = static_cast<char>(first - 'a' + 'A');
    }
    return out;
}

/// F1..F24 — the only key range the UI capture accepts without a modifier.
[[nodiscard]] constexpr bool is_function_key(std::string_view key) noexcept
{
    if (key.size() < 2 || key.size() > 3) {
        return false;
    }
    if (key[0] != 'F' && key[0] != 'f') {
        return false;
    }
    if (key.size() == 2) {
        return key[1] >= '0' && key[1] <= '9';
    }
    return key[1] >= '1' && key[1] <= '2' && key[2] >= '0' && key[2] <= '9';
}

/// A global hotkey: modifier set plus a key name.
///
/// `key` is a System.Windows.Input.Key name such as "Space", "Escape" or "F12",
/// upper-cased on its first character. It stays a string rather than a platform
/// virtual-key code so that it round-trips through settings.json unchanged; the
/// platform layer maps it to a VK/code at registration time.
struct HotkeyGesture {
    HotkeyModifiers modifiers = HotkeyModifiers::none;
    std::string key;

    /// A gesture is usable when it names a key. Modifier-only gestures are
    /// rejected, matching the .NET registration path.
    [[nodiscard]] bool is_valid() const noexcept { return !key.empty(); }

    /// True when the UI capture hook would accept this gesture: a modifier is
    /// required unless the key is F1..F24.
    [[nodiscard]] bool is_capturable() const noexcept
    {
        return is_valid() && (modifiers != HotkeyModifiers::none || is_function_key(key));
    }

    /// Canonical "+"-joined spelling, e.g. "Ctrl+Alt+Space" or "F12".
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const HotkeyGesture& lhs, const HotkeyGesture& rhs) noexcept
    {
        return lhs.modifiers == rhs.modifiers && lhs.key == rhs.key;
    }
};

inline std::string HotkeyGesture::to_string() const
{
    std::string text;
    for (std::size_t i = 0; i < kCanonicalModifierOrder.size(); ++i) {
        if (!has_modifier(modifiers, kCanonicalModifierOrder[i])) {
            continue;
        }
        if (!text.empty()) {
            text += '+';
        }
        text += kModifierTokenOrder[i];
    }

    if (!key.empty()) {
        if (!text.empty()) {
            text += '+';
        }
        text += key;
    }
    return text;
}

/// Parses a settings.json hotkey string ("Ctrl+Alt+Space", "F12", "Space").
///
/// Contract details preserved from the .NET parser:
///   * modifier matching is case-insensitive and accepts all aliases;
///   * the key token is normalized by upper-casing its first character;
///   * a bare key without modifiers parses successfully — the stricter
///     "modifier required" rule belongs to the UI capture path, not to loading;
///   * an empty string is rejected with ErrorCode::invalid_argument.
inline Result<HotkeyGesture> parse_hotkey(std::string_view text)
{
    HotkeyGesture gesture;
    if (text.empty()) {
        return Result<HotkeyGesture>::failure(ErrorCode::invalid_argument, "hotkey string is empty");
    }

    std::string_view remaining = text;
    std::string key_token;

    while (!remaining.empty()) {
        const std::size_t separator = remaining.find('+');
        const std::string_view token = remaining.substr(0, separator);

        if (token.empty()) {
            return Result<HotkeyGesture>::failure(
                ErrorCode::invalid_argument, "hotkey string has an empty component");
        }

        bool recognized_modifier = false;
        const std::string lowered = to_lower_ascii(token);
        for (const auto& [alias, bits] : kModifierAliases) {
            if (alias == lowered) {
                gesture.modifiers |= bits;
                recognized_modifier = true;
                break;
            }
        }

        if (!recognized_modifier) {
            if (!key_token.empty()) {
                // A second non-modifier token: the string is not a valid gesture.
                return Result<HotkeyGesture>::failure(
                    ErrorCode::invalid_argument, "hotkey string has more than one key component");
            }
            key_token.assign(token);
        }

        if (separator == std::string_view::npos) {
            break;
        }
        remaining = remaining.substr(separator + 1);
    }

    gesture.key = normalize_key_token(key_token);
    if (!gesture.is_valid()) {
        return Result<HotkeyGesture>::failure(
            ErrorCode::invalid_argument, "hotkey string has no key component");
    }
    return gesture;
}

} // namespace voicetyper::domain
