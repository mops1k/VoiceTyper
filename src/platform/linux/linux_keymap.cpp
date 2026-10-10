#include "platform/linux/linux_keymap.hpp"

#include "domain/hotkey_gesture.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace voicetyper::platform::linuxos {
namespace {

/// Linux input event codes (linux/input-event-codes.h). Written out as numbers
/// on purpose: this file stays free of the kernel header, so the map is
/// contract-tested on any host and cannot drift with a kernel that renames a
/// constant.
enum : std::int32_t {
    kKeyEsc = 1,
    kKey1 = 2,
    kKey2 = 3,
    kKey3 = 4,
    kKey4 = 5,
    kKey5 = 6,
    kKey6 = 7,
    kKey7 = 8,
    kKey8 = 9,
    kKey9 = 10,
    kKey0 = 11,
    kKeyMinus = 12,
    kKeyEqual = 13,
    kKeyBackspace = 14,
    kKeyTab = 15,
    kKeyQ = 16,
    kKeyW = 17,
    kKeyE = 18,
    kKeyR = 19,
    kKeyT = 20,
    kKeyY = 21,
    kKeyU = 22,
    kKeyI = 23,
    kKeyO = 24,
    kKeyP = 25,
    kKeyLeftBrace = 26,
    kKeyRightBrace = 27,
    kKeyEnter = 28,
    kKeyLeftCtrl = 29,
    kKeyA = 30,
    kKeyS = 31,
    kKeyD = 32,
    kKeyF = 33,
    kKeyG = 34,
    kKeyH = 35,
    kKeyJ = 36,
    kKeyK = 37,
    kKeyL = 38,
    kKeySemicolon = 39,
    kKeyApostrophe = 40,
    kKeyGrave = 41,
    kKeyLeftShift = 42,
    kKeyBackslash = 43,
    kKeyZ = 44,
    kKeyX = 45,
    kKeyC = 46,
    kKeyV = 47,
    kKeyB = 48,
    kKeyN = 49,
    kKeyM = 50,
    kKeyComma = 51,
    kKeyDot = 52,
    kKeySlash = 53,
    kKeyRightShift = 54,
    kKeyLeftAlt = 56,
    kKeySpace = 57,
    kKeyCapsLock = 58,
    kKeyF1 = 59,
    kKeyF10 = 68,
    kKeyNumLock = 69,
    kKeyScrollLock = 70,
    kKeyKp7 = 71,
    kKeyKp8 = 72,
    kKeyKp9 = 73,
    kKeyKp4 = 75,
    kKeyKp5 = 76,
    kKeyKp6 = 77,
    kKeyKp1 = 79,
    kKeyKp2 = 80,
    kKeyKp3 = 81,
    kKeyKp0 = 82,
    kKeyKpDot = 83,
    kKeyF11 = 87,
    kKeyF12 = 88,
    kKeyKpEnter = 96,
    kKeyRightCtrl = 97,
    kKeyKpSlash = 98,
    kKeyPrintScreen = 99,
    kKeyRightAlt = 100,
    kKeyHome = 102,
    kKeyUp = 103,
    kKeyPageUp = 104,
    kKeyLeft = 105,
    kKeyRight = 106,
    kKeyEnd = 107,
    kKeyDown = 108,
    kKeyPageDown = 109,
    kKeyInsert = 110,
    kKeyDelete = 111,
    kKeyPause = 119,
    kKeyLeftMeta = 125,
    kKeyRightMeta = 126,
    kKeyF13 = 183,
    kKeyF24 = 194,
};

/// Qt key codes (Qt::Key_*) and keyboard modifiers (Qt::*Modifier), for the
/// KGlobalAccel fallback. Written out as numbers on purpose, exactly like the
/// input event codes above: this file stays Qt-free, and the contract test
/// compares the result against the real Qt constants.
enum : std::int32_t {
    kQtKeyEscape = 0x01000000,
    kQtKeyTab = 0x01000001,
    kQtKeyBackspace = 0x01000003,
    kQtKeyReturn = 0x01000004,
    kQtKeyEnter = 0x01000005,
    kQtKeyInsert = 0x01000006,
    kQtKeyDelete = 0x01000007,
    kQtKeyPause = 0x01000008,
    kQtKeyPrint = 0x01000009,
    kQtKeyHome = 0x01000010,
    kQtKeyEnd = 0x01000011,
    kQtKeyLeft = 0x01000012,
    kQtKeyUp = 0x01000013,
    kQtKeyRight = 0x01000014,
    kQtKeyDown = 0x01000015,
    kQtKeyPageUp = 0x01000016,
    kQtKeyPageDown = 0x01000017,
    kQtKeyCapsLock = 0x01000024,
    kQtKeyNumLock = 0x01000025,
    kQtKeyScrollLock = 0x01000026,
    kQtKeyF1 = 0x01000030,
    kQtKeypadModifier = 0x20000000,
    kQtKeyPlus = 0x2b,
    kQtKeyComma = 0x2c,
    kQtKeyMinus = 0x2d,
    kQtKeyPeriod = 0x2e,
    kQtKeyQuestion = 0x3f,
    kQtKeySemicolon = 0x3b,
    kQtKeyApostrophe = 0x27,
    kQtKeyBracketLeft = 0x5b,
    kQtKeyBackslash = 0x5c,
    kQtKeyBracketRight = 0x5d,
    kQtKeyQuoteLeft = 0x60,
};

enum : std::int32_t {
    kQtShiftModifier = 0x02000000,
    kQtControlModifier = 0x04000000,
    kQtAltModifier = 0x08000000,
    kQtMetaModifier = 0x10000000,
};

using Entry = std::pair<std::string_view, std::int32_t>;

/// The named keys, in the same spelling the Windows backend and the .NET switch
/// accept. "D0".."D9" are the digits, exactly as in WPF, and "NumPad0".."NumPad9"
/// are the numeric keypad.
constexpr std::array<Entry, 32> kNamedKeys{{
    {"Space", kKeySpace},
    {"Enter", kKeyEnter},
    {"Escape", kKeyEsc},
    {"Tab", kKeyTab},
    {"Back", kKeyBackspace},
    {"Insert", kKeyInsert},
    {"Delete", kKeyDelete},
    {"Home", kKeyHome},
    {"End", kKeyEnd},
    {"PageUp", kKeyPageUp},
    {"PageDown", kKeyPageDown},
    {"Left", kKeyLeft},
    {"Up", kKeyUp},
    {"Right", kKeyRight},
    {"Down", kKeyDown},
    {"PrintScreen", kKeyPrintScreen},
    {"Scroll", kKeyScrollLock},
    {"Pause", kKeyPause},
    {"CapsLock", kKeyCapsLock},
    {"NumLock", kKeyNumLock},
    {"OemPlus", kKeyEqual},
    {"OemMinus", kKeyMinus},
    {"OemComma", kKeyComma},
    {"OemPeriod", kKeyDot},
    {"OemQuestion", kKeySlash},
    {"OemSemicolon", kKeySemicolon},
    {"OemQuotes", kKeyApostrophe},
    {"OemOpenBrackets", kKeyLeftBrace},
    {"OemCloseBrackets", kKeyRightBrace},
    {"OemPipe", kKeyBackslash},
    {"OemTilde", kKeyGrave},
    {"KpEnter", kKeyKpEnter},
}};

/// The named keys again, this time as Qt::Key_* codes. The letters, the digits,
/// the function keys and the keypad are generated below, so only the named ones
/// are stored.
constexpr std::array<Entry, 32> kNamedQtKeys{{
    {"Space", 0x20},
    {"Enter", kQtKeyReturn},
    {"KpEnter", kQtKeyEnter},
    {"Escape", kQtKeyEscape},
    {"Tab", kQtKeyTab},
    {"Back", kQtKeyBackspace},
    {"Insert", kQtKeyInsert},
    {"Delete", kQtKeyDelete},
    {"Home", kQtKeyHome},
    {"End", kQtKeyEnd},
    {"PageUp", kQtKeyPageUp},
    {"PageDown", kQtKeyPageDown},
    {"Left", kQtKeyLeft},
    {"Up", kQtKeyUp},
    {"Right", kQtKeyRight},
    {"Down", kQtKeyDown},
    {"PrintScreen", kQtKeyPrint},
    {"Scroll", kQtKeyScrollLock},
    {"Pause", kQtKeyPause},
    {"CapsLock", kQtKeyCapsLock},
    {"NumLock", kQtKeyNumLock},
    {"OemPlus", kQtKeyPlus},
    {"OemMinus", kQtKeyMinus},
    {"OemComma", kQtKeyComma},
    {"OemPeriod", kQtKeyPeriod},
    {"OemQuestion", kQtKeyQuestion},
    {"OemSemicolon", kQtKeySemicolon},
    {"OemQuotes", kQtKeyApostrophe},
    {"OemOpenBrackets", kQtKeyBracketLeft},
    {"OemCloseBrackets", kQtKeyBracketRight},
    {"OemPipe", kQtKeyBackslash},
    {"OemTilde", kQtKeyQuoteLeft},
}};

constexpr std::array<std::int32_t, 10> kDigitCodes{
    kKey0, kKey1, kKey2, kKey3, kKey4, kKey5, kKey6, kKey7, kKey8, kKey9,
};

constexpr std::array<std::int32_t, 10> kNumPadCodes{
    kKeyKp0, kKeyKp1, kKeyKp2, kKeyKp3, kKeyKp4, kKeyKp5, kKeyKp6, kKeyKp7, kKeyKp8, kKeyKp9,
};

constexpr std::array<std::int32_t, 26> kLetterCodes{
    kKeyA, kKeyB, kKeyC, kKeyD, kKeyE, kKeyF, kKeyG, kKeyH, kKeyI, kKeyJ, kKeyK, kKeyL, kKeyM,
    kKeyN, kKeyO, kKeyP, kKeyQ, kKeyR, kKeyS, kKeyT, kKeyU, kKeyV, kKeyW, kKeyX, kKeyY, kKeyZ,
};

/// The function-key number ("F12" -> 12), 0 when the name is not F1..F24. Shared
/// by the input-code and the Qt-code maps, whose numbering differs (the input
/// codes of F11/F12 and F13..F24 are not consecutive with F1..F10).
int function_key_number(std::string_view key) noexcept
{
    if (key.size() < 2 || key.size() > 3 || (key[0] != 'F' && key[0] != 'f')) {
        return 0;
    }
    int number = 0;
    for (std::size_t index = 1; index < key.size(); ++index) {
        if (key[index] < '0' || key[index] > '9') {
            return 0;
        }
        number = number * 10 + (key[index] - '0');
    }
    if (number < 1 || number > 24) {
        return 0;
    }
    return number;
}

std::int32_t function_key_code(std::string_view key) noexcept
{
    const int number = function_key_number(key);
    if (number == 0) {
        return 0;
    }
    if (number <= 10) {
        return kKeyF1 + number - 1;
    }
    if (number <= 12) {
        return kKeyF11 + number - 11;
    }
    return kKeyF13 + number - 13;
}

/// Qt::Key_F1..F24 are consecutive, unlike the input codes.
std::int32_t qt_function_key_code(std::string_view key) noexcept
{
    const int number = function_key_number(key);
    return number == 0 ? 0 : kQtKeyF1 + number - 1;
}

std::int32_t num_pad_key_code(std::string_view key) noexcept
{
    const std::string lowered = domain::to_lower_ascii(key);
    if (lowered.size() != 7 || lowered.compare(0, 6, "numpad") != 0) {
        return 0;
    }
    if (lowered[6] < '0' || lowered[6] > '9') {
        return 0;
    }
    return kNumPadCodes[static_cast<std::size_t>(lowered[6] - '0')];
}

std::int32_t digit_key_code(std::string_view key) noexcept
{
    if (key.size() != 2 || (key[0] != 'D' && key[0] != 'd') || key[1] < '0' || key[1] > '9') {
        return 0;
    }
    return kDigitCodes[static_cast<std::size_t>(key[1] - '0')];
}

std::int32_t letter_key_code(std::string_view key) noexcept
{
    if (key.size() != 1) {
        return 0;
    }
    const char upper = key[0] >= 'a' && key[0] <= 'z' ? static_cast<char>(key[0] - 'a' + 'A') : key[0];
    if (upper < 'A' || upper > 'Z') {
        return 0;
    }
    return kLetterCodes[static_cast<std::size_t>(upper - 'A')];
}

} // namespace

std::int32_t hotkey_key_code(std::string_view key_name)
{
    if (key_name.empty()) {
        return 0;
    }
    if (const std::int32_t code = letter_key_code(key_name); code != 0) {
        return code;
    }
    if (const std::int32_t code = digit_key_code(key_name); code != 0) {
        return code;
    }
    if (const std::int32_t code = num_pad_key_code(key_name); code != 0) {
        return code;
    }
    if (const std::int32_t code = function_key_code(key_name); code != 0) {
        return code;
    }
    for (const auto& [name, code] : kNamedKeys) {
        if (name == key_name) {
            return code;
        }
    }
    return 0;
}

std::string_view hotkey_key_name(std::int32_t key_code)
{
    if (key_code == 0) {
        return {};
    }
    // The letters and digits first: their names are generated, not stored.
    for (std::size_t index = 0; index < kLetterCodes.size(); ++index) {
        if (kLetterCodes[index] == key_code) {
            static constexpr std::array<std::string_view, 26> kLetters{
                "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
            };
            return kLetters[index];
        }
    }
    for (std::size_t index = 0; index < kDigitCodes.size(); ++index) {
        if (kDigitCodes[index] == key_code) {
            static constexpr std::array<std::string_view, 10> kDigits{
                "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9",
            };
            return kDigits[index];
        }
    }
    for (std::size_t index = 0; index < kNumPadCodes.size(); ++index) {
        if (kNumPadCodes[index] == key_code) {
            static constexpr std::array<std::string_view, 10> kNumPad{
                "NumPad0", "NumPad1", "NumPad2", "NumPad3", "NumPad4",
                "NumPad5", "NumPad6", "NumPad7", "NumPad8", "NumPad9",
            };
            return kNumPad[index];
        }
    }
    if (key_code >= kKeyF1 && key_code <= kKeyF10) {
        static constexpr std::array<std::string_view, 10> kFunction{
            "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10",
        };
        return kFunction[static_cast<std::size_t>(key_code - kKeyF1)];
    }
    if (key_code == kKeyF11) {
        return "F11";
    }
    if (key_code == kKeyF12) {
        return "F12";
    }
    if (key_code >= kKeyF13 && key_code <= kKeyF24) {
        static constexpr std::array<std::string_view, 12> kFunction{
            "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
        };
        return kFunction[static_cast<std::size_t>(key_code - kKeyF13)];
    }
    for (const auto& [name, code] : kNamedKeys) {
        if (code == key_code) {
            return name;
        }
    }
    return {};
}

bool is_modifier_key_code(std::int32_t key_code)
{
    return modifier_bits_for_key_code(key_code) != 0;
}

std::uint8_t modifier_bits_for_key_code(std::int32_t key_code)
{
    switch (key_code) {
    case kKeyLeftCtrl:
    case kKeyRightCtrl:
        return static_cast<std::uint8_t>(domain::HotkeyModifiers::control);
    case kKeyLeftAlt:
    case kKeyRightAlt:
        return static_cast<std::uint8_t>(domain::HotkeyModifiers::alt);
    case kKeyLeftShift:
    case kKeyRightShift:
        return static_cast<std::uint8_t>(domain::HotkeyModifiers::shift);
    case kKeyLeftMeta:
    case kKeyRightMeta:
        return static_cast<std::uint8_t>(domain::HotkeyModifiers::win);
    default:
        return 0;
    }
}

std::int32_t hotkey_qt_key_code(std::string_view key_name)
{
    if (key_name.empty()) {
        return 0;
    }
    // A single letter is its own Qt code: Qt::Key_A is 'A'.
    if (key_name.size() == 1) {
        const char upper = key_name[0] >= 'a' && key_name[0] <= 'z'
            ? static_cast<char>(key_name[0] - 'a' + 'A')
            : key_name[0];
        return upper >= 'A' && upper <= 'Z' ? static_cast<std::int32_t>(upper) : 0;
    }
    // The digits are ASCII too: Qt::Key_0 is '0'. "D0".."D9" is the WPF spelling.
    if (key_name.size() == 2 && (key_name[0] == 'D' || key_name[0] == 'd')
        && key_name[1] >= '0' && key_name[1] <= '9') {
        return static_cast<std::int32_t>('0' + (key_name[1] - '0'));
    }
    if (const std::int32_t code = qt_function_key_code(key_name); code != 0) {
        return code;
    }
    // The keypad digits are Qt::Key_0..9 plus Qt::KeypadModifier.
    const std::string lowered = domain::to_lower_ascii(key_name);
    if (lowered.size() == 7 && lowered.compare(0, 6, "numpad") == 0
        && lowered[6] >= '0' && lowered[6] <= '9') {
        return kQtKeypadModifier + static_cast<std::int32_t>('0' + (lowered[6] - '0'));
    }
    for (const auto& [name, code] : kNamedQtKeys) {
        if (name == key_name) {
            return code;
        }
    }
    return 0;
}

std::int32_t hotkey_qt_modifier_flags(domain::HotkeyModifiers modifiers)
{
    const auto bits = static_cast<std::uint8_t>(modifiers);
    std::int32_t flags = 0;
    if ((bits & static_cast<std::uint8_t>(domain::HotkeyModifiers::shift)) != 0) {
        flags |= kQtShiftModifier;
    }
    if ((bits & static_cast<std::uint8_t>(domain::HotkeyModifiers::control)) != 0) {
        flags |= kQtControlModifier;
    }
    if ((bits & static_cast<std::uint8_t>(domain::HotkeyModifiers::alt)) != 0) {
        flags |= kQtAltModifier;
    }
    if ((bits & static_cast<std::uint8_t>(domain::HotkeyModifiers::win)) != 0) {
        flags |= kQtMetaModifier;
    }
    return flags;
}

} // namespace voicetyper::platform::linuxos
