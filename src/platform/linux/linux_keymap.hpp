#pragma once

// The key-name map between the settings file and the Linux input subsystem.
//
// Evidence and contract:
//   * src/domain/hotkey_gesture.hpp - the key stays a System.Windows.Input.Key
//     name ("Space", "F12", "NumPad0", "OemTilde", "A"), upper-cased on its first
//     character, so it round-trips through settings.json unchanged; the platform
//     layer maps it to a native code at registration time.
//   * src/platform/windows/win32_hotkeys.cpp - the Windows half of that map
//     (VK codes); this is the Linux half, using the input event codes from
//     <linux/input-event-codes.h>.
//
// Why the map is a table and not a formula: the Linux key codes are not
// sequential in any useful way (KEY_A is 30 but KEY_B is 48, KEY_1 is 2 but
// KEY_0 is 11), so a table is the only honest representation.
//
// This header is standard C++ only: it includes no Linux header, so the map is
// contract-tested without /dev/input and without a desktop session.

#include "domain/hotkey_gesture.hpp"

#include <cstdint>
#include <string_view>

namespace voicetyper::platform::linuxos {

/// The Linux input event code for a WPF key name, 0 when the name is unknown
/// (0 is never a valid key code: KEY_RESERVED).
[[nodiscard]] std::int32_t hotkey_key_code(std::string_view key_name);

/// The WPF key name for a Linux input event code, empty when the code has no
/// name this product knows. This is what the capture hook reports.
[[nodiscard]] std::string_view hotkey_key_name(std::int32_t key_code);

/// True for the modifier key codes (Ctrl/Alt/Shift/Meta, left and right). The
/// capture hook needs this to know which events are modifiers rather than the
/// key of the gesture.
[[nodiscard]] bool is_modifier_key_code(std::int32_t key_code);

/// The modifier bits a key code contributes, or 0 when it is not a modifier.
[[nodiscard]] std::uint8_t modifier_bits_for_key_code(std::int32_t key_code);

/// The Qt key code for a WPF key name, for the KGlobalAccel fallback: the value
/// is Qt::Key_* plus Qt::KeypadModifier for "NumPad0".."NumPad9", and 0 when the
/// name is unknown. Plain integers keep this header Qt-free; the contract test
/// compares them against the real Qt constants.
[[nodiscard]] std::int32_t hotkey_qt_key_code(std::string_view key_name);

/// The Qt keyboard-modifier bits (Qt::ControlModifier and friends) of a gesture's
/// modifiers, 0 for `none`.
[[nodiscard]] std::int32_t hotkey_qt_modifier_flags(domain::HotkeyModifiers modifiers);

} // namespace voicetyper::platform::linuxos
