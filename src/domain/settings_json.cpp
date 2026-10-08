#include "domain/settings_json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace voicetyper::domain {
namespace detail {

struct JsonValue {
    enum class Kind : std::uint8_t { null, boolean, number, string, array, object };

    Kind kind = Kind::null;
    bool boolean = false;
    double number = 0.0;
    /// Only meaningful while `integer_value` is true. A non-integral or
    /// out-of-int64-range literal leaves it at 0 instead of casting a double
    /// that `static_cast<std::int64_t>` would not define.
    std::int64_t integer = 0;
    bool integer_value = false;
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;
};

class JsonParser final {
public:
    explicit JsonParser(std::string_view input)
        : input_(input)
    {
    }

    bool parse(JsonValue& value)
    {
        skip_space();
        if (!parse_value(value, 0)) {
            return false;
        }
        skip_space();
        if (pos_ != input_.size()) {
            return fail("trailing characters after JSON document");
        }
        return true;
    }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    static constexpr int kMaxDepth = 64;

    bool fail(std::string message)
    {
        if (error_.empty()) {
            error_ = std::move(message);
        }
        return false;
    }

    void skip_space() noexcept
    {
        while (pos_ < input_.size()) {
            const char ch = input_[pos_];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                break;
            }
            ++pos_;
        }
    }

    bool consume(char expected)
    {
        skip_space();
        if (pos_ >= input_.size() || input_[pos_] != expected) {
            return fail(std::string("expected '") + expected + "'");
        }
        ++pos_;
        return true;
    }

    bool parse_value(JsonValue& value, int depth)
    {
        if (depth > kMaxDepth) {
            return fail("JSON nesting is too deep");
        }
        skip_space();
        if (pos_ >= input_.size()) {
            return fail("unexpected end of JSON document");
        }
        switch (input_[pos_]) {
        case 'n':
            return parse_literal("null", JsonValue::Kind::null, value);
        case 't':
            return parse_literal("true", JsonValue::Kind::boolean, value, true);
        case 'f':
            return parse_literal("false", JsonValue::Kind::boolean, value, false);
        case '"':
            value.kind = JsonValue::Kind::string;
            return parse_string(value.string);
        case '[':
            return parse_array(value, depth);
        case '{':
            return parse_object(value, depth);
        default:
            if (input_[pos_] == '-' || (input_[pos_] >= '0' && input_[pos_] <= '9')) {
                return parse_number(value);
            }
            return fail("unexpected JSON token");
        }
    }

    bool parse_literal(
        std::string_view literal,
        JsonValue::Kind kind,
        JsonValue& value,
        bool boolean = false)
    {
        if (input_.substr(pos_, literal.size()) != literal) {
            return fail("invalid JSON literal");
        }
        pos_ += literal.size();
        value.kind = kind;
        value.boolean = boolean;
        return true;
    }

    bool parse_hex4(std::uint32_t& value)
    {
        if (input_.size() - pos_ < 4) {
            return fail("truncated unicode escape");
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = input_[pos_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') {
                value |= static_cast<std::uint32_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                value |= static_cast<std::uint32_t>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                value |= static_cast<std::uint32_t>(ch - 'A' + 10);
            } else {
                return fail("invalid unicode escape digit");
            }
        }
        return true;
    }

    static void append_utf8(std::string& output, std::uint32_t code_point)
    {
        if (code_point <= 0x7F) {
            output.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    bool parse_string(std::string& output)
    {
        if (!consume('"')) {
            return false;
        }
        output.clear();
        while (pos_ < input_.size()) {
            const char ch = input_[pos_++];
            if (ch == '"') {
                return true;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                return fail("control character in JSON string");
            }
            if (ch != '\\') {
                output.push_back(ch);
                continue;
            }
            if (pos_ >= input_.size()) {
                return fail("truncated JSON escape");
            }
            const char escaped = input_[pos_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t code_point = 0;
                if (!parse_hex4(code_point)) {
                    return false;
                }
                if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                    if (input_.size() - pos_ < 6 || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
                        return fail("unpaired high surrogate");
                    }
                    pos_ += 2;
                    std::uint32_t low = 0;
                    if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        return fail("invalid low surrogate");
                    }
                    code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
                } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                    return fail("unpaired low surrogate");
                }
                append_utf8(output, code_point);
                break;
            }
            default:
                return fail("unknown JSON string escape");
            }
        }
        return fail("unterminated JSON string");
    }

    bool parse_number(JsonValue& value)
    {
        const std::size_t start = pos_;
        if (input_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= input_.size()) {
            return fail("truncated JSON number");
        }
        if (input_[pos_] == '0') {
            ++pos_;
        } else {
            if (input_[pos_] < '1' || input_[pos_] > '9') {
                return fail("invalid JSON number");
            }
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
                ++pos_;
            }
        }
        bool is_integer = true;
        if (pos_ < input_.size() && input_[pos_] == '.') {
            is_integer = false;
            ++pos_;
            const std::size_t fraction_start = pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
                ++pos_;
            }
            if (pos_ == fraction_start) {
                return fail("JSON number has no fractional digits");
            }
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            is_integer = false;
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
                ++pos_;
            }
            const std::size_t exponent_start = pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
                ++pos_;
            }
            if (pos_ == exponent_start) {
                return fail("JSON number has no exponent digits");
            }
        }
        const std::string raw(input_.substr(start, pos_ - start));
        const char* first = raw.data();
        const char* last = first + raw.size();
        if (is_integer) {
            std::int64_t integer = 0;
            const auto result = std::from_chars(first, last, integer);
            if (result.ec == std::errc{} && result.ptr == last) {
                value.kind = JsonValue::Kind::number;
                value.integer = integer;
                value.integer_value = true;
                value.number = static_cast<double>(integer);
                return true;
            }
        }
        double number = 0.0;
        const auto result = std::from_chars(first, last, number);
        if (result.ec != std::errc{} || result.ptr != last) {
            return fail("JSON number is out of range");
        }
        value.kind = JsonValue::Kind::number;
        value.number = number;
        // Deliberately no `static_cast<std::int64_t>(number)`: the literal may
        // be non-integral (1.5) or outside the int64 range (1e300), and that
        // cast is undefined behaviour. Non-integral literals are never read as
        // integers -- every reader checks `integer_value` first.
        value.integer_value = false;
        return true;
    }

    bool parse_array(JsonValue& value, int depth)
    {
        if (!consume('[')) {
            return false;
        }
        value.kind = JsonValue::Kind::array;
        skip_space();
        if (pos_ < input_.size() && input_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue item;
            if (!parse_value(item, depth + 1)) {
                return false;
            }
            value.array.push_back(std::move(item));
            skip_space();
            if (pos_ >= input_.size()) {
                return fail("unterminated JSON array");
            }
            if (input_[pos_] == ']') {
                ++pos_;
                return true;
            }
            if (input_[pos_] != ',') {
                return fail("JSON array entries must be comma-separated");
            }
            ++pos_;
        }
    }

    bool parse_object(JsonValue& value, int depth)
    {
        if (!consume('{')) {
            return false;
        }
        value.kind = JsonValue::Kind::object;
        skip_space();
        if (pos_ < input_.size() && input_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            if (!consume(':')) {
                return false;
            }
            JsonValue item;
            if (!parse_value(item, depth + 1)) {
                return false;
            }
            value.object.emplace_back(std::move(key), std::move(item));
            skip_space();
            if (pos_ >= input_.size()) {
                return fail("unterminated JSON object");
            }
            if (input_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (input_[pos_] != ',') {
                return fail("JSON object entries must be comma-separated");
            }
            ++pos_;
        }
    }

    std::string_view input_;
    std::size_t pos_ = 0;
    std::string error_;
};

/// Collapses duplicate object keys before any value is dispatched.
///
/// System.Text.Json binds a JSON object to a CLR object, so a repeated
/// property simply overwrites the previous value: the *last* occurrence wins
/// and earlier occurrences are never validated at all. Dispatching in parse
/// order would instead surface the first occurrence's error and force a
/// document-wide default fallback, so de-duplication happens first. Key
/// comparison is case-insensitive because the read policy is
/// `PropertyNameCaseInsensitive = true`. The first occurrence keeps its
/// position (diagnostic order), the last occurrence keeps its value.
inline std::vector<const std::pair<std::string, JsonValue>*> deduplicate_last_wins(
    const JsonValue& root)
{
    std::vector<const std::pair<std::string, JsonValue>*> unique;
    unique.reserve(root.object.size());
    for (const auto& entry : root.object) {
        auto existing = std::find_if(
            unique.begin(),
            unique.end(),
            [&entry](const auto* candidate) {
                return equals_ignore_ascii_case(candidate->first, entry.first);
            });
        if (existing == unique.end()) {
            unique.push_back(&entry);
        } else {
            *existing = &entry;
        }
    }
    return unique;
}

inline void diagnostic(
    std::vector<SettingsDiagnostic>& output,
    SettingsDiagnosticKind kind,
    std::string field,
    std::string message)
{
    output.push_back(SettingsDiagnostic{kind, std::move(field), std::move(message)});
}

template <typename T, typename FromWire>
bool read_enum(
    const JsonValue& value,
    std::string field,
    T fallback,
    T& output,
    FromWire from_wire,
    std::size_t count,
    std::vector<SettingsDiagnostic>& diagnostics,
    bool& error)
{
    if (value.kind == JsonValue::Kind::string) {
        const auto parsed = from_wire(value.string);
        if (!parsed.has_value()) {
            diagnostic(diagnostics, SettingsDiagnosticKind::error, field, "invalid enum string");
            error = true;
            return false;
        }
        output = *parsed;
        return true;
    }
    if (value.kind == JsonValue::Kind::number && value.integer_value) {
        if (value.integer < 0 || static_cast<std::size_t>(value.integer) >= count) {
            diagnostic(diagnostics, SettingsDiagnosticKind::warning, field, "numeric enum is out of range; using default");
            output = fallback;
            return true;
        }
        diagnostic(diagnostics, SettingsDiagnosticKind::warning, field, "numeric enum accepted; canonical string will be written on save");
        output = static_cast<T>(value.integer);
        return true;
    }
    diagnostic(diagnostics, SettingsDiagnosticKind::error, field, "expected enum string or integer");
    error = true;
    return false;
}

bool read_string(
    const JsonValue& value,
    std::string field,
    std::string& output,
    std::vector<SettingsDiagnostic>& diagnostics,
    bool& error)
{
    if (value.kind == JsonValue::Kind::string) {
        output = value.string;
        return true;
    }
    if (value.kind == JsonValue::Kind::null) {
        // Documented Phase B policy: accept, store empty, warn.
        //
        // Measured against the real .NET generator: System.Text.Json assigns a
        // JSON null to a `string` property without raising JsonException, so
        // the document still loads and only that one property is affected. The
        // C# property then holds null and is re-serialized as null, which a
        // non-nullable std::string cannot represent; empty is the closest
        // representable value and the warning makes the lossy step visible
        // instead of silently inventing a default the .NET build never uses.
        // Rejecting the value (and with it the whole document) was rejected as
        // a stronger divergence from the measured behavior.
        diagnostic(diagnostics, SettingsDiagnosticKind::warning, field,
            "null for a non-nullable string is stored as empty (System.Text.Json accepts null here); the C# build keeps null and rewrites it as null");
        output.clear();
        return true;
    }
    diagnostic(diagnostics, SettingsDiagnosticKind::error, field, "expected string");
    error = true;
    return false;
}

bool read_optional_string(
    const JsonValue& value,
    std::string field,
    std::optional<std::string>& output,
    std::vector<SettingsDiagnostic>& diagnostics,
    bool& error)
{
    if (value.kind == JsonValue::Kind::string) {
        output = value.string;
        return true;
    }
    if (value.kind == JsonValue::Kind::null) {
        output.reset();
        return true;
    }
    diagnostic(diagnostics, SettingsDiagnosticKind::error, field, "expected string or null");
    error = true;
    return false;
}

bool read_bool(
    const JsonValue& value,
    std::string field,
    bool& output,
    std::vector<SettingsDiagnostic>& diagnostics,
    bool& error)
{
    if (value.kind == JsonValue::Kind::boolean) {
        output = value.boolean;
        return true;
    }
    diagnostic(diagnostics, SettingsDiagnosticKind::error, field, "expected boolean");
    error = true;
    return false;
}

bool read_int(
    const JsonValue& value,
    std::string field,
    int& output,
    std::vector<SettingsDiagnostic>& diagnostics,
    bool& error)
{
    if (value.kind == JsonValue::Kind::number && value.integer_value &&
        value.integer >= std::numeric_limits<int>::min() && value.integer <= std::numeric_limits<int>::max()) {
        output = static_cast<int>(value.integer);
        return true;
    }
    diagnostic(diagnostics, SettingsDiagnosticKind::error, field, "expected integer");
    error = true;
    return false;
}

bool read_double(
    const JsonValue& value,
    std::string field,
    double& output,
    std::vector<SettingsDiagnostic>& diagnostics,
    bool& error)
{
    if (value.kind == JsonValue::Kind::number) {
        output = value.number;
        return true;
    }
    diagnostic(diagnostics, SettingsDiagnosticKind::error, field, "expected number");
    error = true;
    return false;
}

inline void append_hex(std::string& output, std::uint32_t value)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    output += "\\u";
    for (int shift = 12; shift >= 0; shift -= 4) {
        output.push_back(digits[(value >> shift) & 0xF]);
    }
}

void append_json_string(std::string& output, std::string_view value)
{
    output.push_back('"');
    for (std::size_t i = 0; i < value.size();) {
        const auto ch = static_cast<unsigned char>(value[i]);
        switch (ch) {
        case '"': output += "\\\""; ++i; continue;
        case '\\': output += "\\\\"; ++i; continue;
        case '\b': output += "\\b"; ++i; continue;
        case '\f': output += "\\f"; ++i; continue;
        case '\n': output += "\\n"; ++i; continue;
        case '\r': output += "\\r"; ++i; continue;
        case '\t': output += "\\t"; ++i; continue;
        case '+': append_hex(output, ch); ++i; continue;
        case '&': append_hex(output, ch); ++i; continue;
        case '<': append_hex(output, ch); ++i; continue;
        case '>': append_hex(output, ch); ++i; continue;
        case '\'': append_hex(output, ch); ++i; continue;
        default: break;
        }
        if (ch < 0x20) {
            append_hex(output, ch);
            ++i;
            continue;
        }
        if (ch < 0x80) {
            output.push_back(static_cast<char>(ch));
            ++i;
            continue;
        }
        std::uint32_t code_point = 0;
        std::size_t length = 0;
        if ((ch & 0xE0) == 0xC0) {
            code_point = ch & 0x1F;
            length = 2;
        } else if ((ch & 0xF0) == 0xE0) {
            code_point = ch & 0x0F;
            length = 3;
        } else if ((ch & 0xF8) == 0xF0) {
            code_point = ch & 0x07;
            length = 4;
        } else {
            append_hex(output, ch);
            ++i;
            continue;
        }
        if (i + length > value.size()) {
            append_hex(output, ch);
            ++i;
            continue;
        }
        bool valid = true;
        for (std::size_t j = 1; j < length; ++j) {
            const auto next = static_cast<unsigned char>(value[i + j]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            code_point = (code_point << 6) | (next & 0x3F);
        }
        if (!valid) {
            append_hex(output, ch);
            ++i;
            continue;
        }
        if (code_point > 0xFFFF) {
            // A \uXXXX escape is a UTF-16 code unit, so an astral code point
            // must be written as a surrogate pair. Emitting the code point
            // itself would produce a 5-digit escape that no JSON reader
            // accepts.
            const std::uint32_t offset = code_point - 0x10000;
            append_hex(output, 0xD800 + (offset >> 10));
            append_hex(output, 0xDC00 + (offset & 0x3FF));
        } else {
            append_hex(output, code_point);
        }
        i += length;
    }
    output.push_back('"');
}

std::string number_to_json(double value)
{
    if (value == 0.0) {
        return "0";
    }
    if (!std::isfinite(value)) {
        // JSON has no NaN/Infinity literal and System.Text.Json throws when it
        // meets one, so a C# save of a non-finite temperature fails outright.
        // serialize() is a pure, noexcept-by-contract formatter that callers
        // also use for diffing, so the value is normalized to 0 here and the
        // divergence is documented instead of thrown from a formatter. The
        // next load then reads a plain 0.
        return "0";
    }
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general);
    if (result.ec != std::errc{}) {
        return "0";
    }
    std::string text(buffer.data(), result.ptr);
    for (char& ch : text) {
        if (ch == 'e') {
            ch = 'E';
        }
    }
    return text;
}

void append_field(
    std::string& output,
    std::string_view name,
    std::string_view value,
    std::string_view line_ending, bool comma = true)
{
    output += "  ";
    append_json_string(output, name);
    output += ": ";
    append_json_string(output, value);
    if (comma) {
        output += ',';
    }
    output += line_ending;
}

void append_bool_field(std::string& output, std::string_view name, bool value, std::string_view line_ending, bool comma = true)
{
    output += "  ";
    append_json_string(output, name);
    output += ": ";
    output += value ? "true" : "false";
    if (comma) {
        output += ',';
    }
    output += line_ending;
}

void append_int_field(std::string& output, std::string_view name, int value, std::string_view line_ending, bool comma = true)
{
    output += "  ";
    append_json_string(output, name);
    output += ": ";
    output += std::to_string(value);
    if (comma) {
        output += ',';
    }
    output += line_ending;
}

void append_double_field(std::string& output, std::string_view name, double value, std::string_view line_ending, bool comma = true)
{
    output += "  ";
    append_json_string(output, name);
    output += ": ";
    output += number_to_json(value);
    if (comma) {
        output += ',';
    }
    output += line_ending;
}

void append_optional_field(
    std::string& output,
    std::string_view name,
    const std::optional<std::string>& value,
    std::string_view line_ending, bool comma = true)
{
    output += "  ";
    append_json_string(output, name);
    output += ": ";
    if (value.has_value()) {
        append_json_string(output, *value);
    } else {
        output += "null";
    }
    if (comma) {
        output += ',';
    }
    output += line_ending;
}

void append_enum_field(
    std::string& output,
    std::string_view name,
    std::string_view value,
    std::string_view line_ending, bool comma = true)
{
    output += "  ";
    append_json_string(output, name);
    output += ": ";
    append_json_string(output, value);
    if (comma) {
        output += ',';
    }
    output += line_ending;
}

} // namespace detail

SettingsLoadResult SettingsCodec::load(std::string_view json)
{
    SettingsLoadResult result;
    try {
        detail::JsonValue root;
        detail::JsonParser parser(json);
        if (!parser.parse(root) || root.kind != detail::JsonValue::Kind::object) {
            detail::diagnostic(
                result.diagnostics,
                SettingsDiagnosticKind::error,
                "$",
                parser.error().empty() ? "settings root must be an object" : parser.error());
            return result;
        }

        bool error = false;
        for (const auto* entry : detail::deduplicate_last_wins(root)) {
            const auto& [key, value] = *entry;
            const auto field = std::string(key);
            if (detail::equals_ignore_ascii_case(key, "recordingMode")) {
                (void)detail::read_enum(value, field, AppSettings::defaults().recording_mode, result.settings.recording_mode, recording_mode_from_wire, 3, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "recordHotkey")) {
                (void)detail::read_string(value, field, result.settings.record_hotkey, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "cancelHotkey")) {
                (void)detail::read_string(value, field, result.settings.cancel_hotkey, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "recordGamepadButton")) {
                (void)detail::read_optional_string(value, field, result.settings.record_gamepad_button, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "cancelGamepadButton")) {
                (void)detail::read_optional_string(value, field, result.settings.cancel_gamepad_button, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "language")) {
                (void)detail::read_enum(value, field, AppSettings::defaults().language, result.settings.language, recognition_language_from_wire, 3, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "modelSize")) {
                (void)detail::read_enum(value, field, AppSettings::defaults().model_size, result.settings.model_size, model_size_from_wire, 5, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "transcriptionEngine")) {
                (void)detail::read_enum(value, field, AppSettings::defaults().transcription_engine, result.settings.transcription_engine, transcription_engine_from_wire, 3, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "parakeetModelSize")) {
                (void)detail::read_enum(value, field, AppSettings::defaults().parakeet_model_size, result.settings.parakeet_model_size, parakeet_model_size_from_wire, 4, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "gigaamModelSize")) {
                (void)detail::read_enum(value, field, AppSettings::defaults().gigaam_model_size, result.settings.gigaam_model_size, gigaam_model_size_from_wire, 4, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "autoPasteEnabled")) {
                (void)detail::read_bool(value, field, result.settings.auto_paste_enabled, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "termsDictionary")) {
                (void)detail::read_string(value, field, result.settings.terms_dictionary, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "silenceThresholdMs")) {
                (void)detail::read_int(value, field, result.settings.silence_threshold_ms, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "startWithWindows")) {
                (void)detail::read_bool(value, field, result.settings.start_with_windows, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "startMinimized")) {
                (void)detail::read_bool(value, field, result.settings.start_minimized, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "theme")) {
                (void)detail::read_enum(value, field, AppSettings::defaults().theme, result.settings.theme, app_theme_from_wire, 3, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "hideOnFocusLoss")) {
                (void)detail::read_bool(value, field, result.settings.hide_on_focus_loss, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "appLanguage")) {
                (void)detail::read_enum(value, field, AppSettings::defaults().app_language, result.settings.app_language, app_language_from_wire, 2, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "noiseReductionEnabled")) {
                (void)detail::read_bool(value, field, result.settings.noise_reduction_enabled, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "temperature")) {
                (void)detail::read_double(value, field, result.settings.temperature, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "bestOf")) {
                // Read verbatim, exactly like silenceThresholdMs: the range belongs
                // to AppSettings::validate() and to the engine clamp, and a
                // hand-edited out-of-range value must not turn the whole document
                // into defaults.
                (void)detail::read_int(value, field, result.settings.best_of, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "conditionOnPreviousText")) {
                (void)detail::read_bool(value, field, result.settings.condition_on_previous_text, result.diagnostics, error);
            } else if (detail::equals_ignore_ascii_case(key, "microphoneDeviceId")) {
                (void)detail::read_optional_string(value, field, result.settings.microphone_device_id, result.diagnostics, error);
            }
        }
        if (error) {
            result.settings = AppSettings::defaults();
            result.used_defaults = true;
        } else {
            result.used_defaults = false;
        }
        return result;
    } catch (const std::exception& exception) {
        detail::diagnostic(result.diagnostics, SettingsDiagnosticKind::error, "$", std::string("settings parser failed: ") + exception.what());
        return result;
    } catch (...) {
        detail::diagnostic(result.diagnostics, SettingsDiagnosticKind::error, "$", "settings parser failed with an unknown exception");
        return result;
    }
}

SettingsLoadResult SettingsCodec::load_file(const std::filesystem::path& path)
{
    SettingsLoadResult result;
    try {
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            // First run: .NET Load() also returns defaults for a missing file
            // and raises nothing, so this is a normal state, not a diagnostic.
            return result;
        }
        if (error) {
            detail::diagnostic(
                result.diagnostics, SettingsDiagnosticKind::error, "$",
                "cannot stat settings file: " + error.message());
            return result;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            detail::diagnostic(result.diagnostics, SettingsDiagnosticKind::error, "$", "cannot open settings file");
            return result;
        }
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return load(text);
    } catch (const std::exception& exception) {
        detail::diagnostic(result.diagnostics, SettingsDiagnosticKind::error, "$", std::string("settings file read failed: ") + exception.what());
        return result;
    } catch (...) {
        detail::diagnostic(result.diagnostics, SettingsDiagnosticKind::error, "$", "settings file read failed with an unknown exception");
        return result;
    }
}

std::string SettingsCodec::serialize(const AppSettings& settings, std::string_view line_ending)
{
    std::string output;
    output.reserve(1024);
    output += "{";
    output += line_ending;
    detail::append_enum_field(output, "recordingMode", to_wire(settings.recording_mode), line_ending);
    detail::append_field(output, "recordHotkey", settings.record_hotkey, line_ending);
    detail::append_field(output, "cancelHotkey", settings.cancel_hotkey, line_ending);
    detail::append_optional_field(output, "recordGamepadButton", settings.record_gamepad_button, line_ending);
    detail::append_optional_field(output, "cancelGamepadButton", settings.cancel_gamepad_button, line_ending);
    detail::append_enum_field(output, "language", to_wire(settings.language), line_ending);
    detail::append_enum_field(output, "modelSize", to_wire(settings.model_size), line_ending);
    detail::append_enum_field(output, "transcriptionEngine", to_wire(settings.transcription_engine), line_ending);
    detail::append_enum_field(output, "parakeetModelSize", to_wire(settings.parakeet_model_size), line_ending);
    detail::append_enum_field(output, "gigaamModelSize", to_wire(settings.gigaam_model_size), line_ending);
    detail::append_bool_field(output, "autoPasteEnabled", settings.auto_paste_enabled, line_ending);
    detail::append_field(output, "termsDictionary", settings.terms_dictionary, line_ending);
    detail::append_int_field(output, "silenceThresholdMs", settings.silence_threshold_ms, line_ending);
    detail::append_bool_field(output, "startWithWindows", settings.start_with_windows, line_ending);
    detail::append_bool_field(output, "startMinimized", settings.start_minimized, line_ending);
    detail::append_enum_field(output, "theme", to_wire(settings.theme), line_ending);
    detail::append_bool_field(output, "hideOnFocusLoss", settings.hide_on_focus_loss, line_ending);
    detail::append_enum_field(output, "appLanguage", to_wire(settings.app_language), line_ending);
    detail::append_bool_field(output, "noiseReductionEnabled", settings.noise_reduction_enabled, line_ending);
    detail::append_double_field(output, "temperature", settings.temperature, line_ending);
    detail::append_int_field(output, "bestOf", settings.best_of, line_ending);
    detail::append_bool_field(output, "conditionOnPreviousText", settings.condition_on_previous_text, line_ending);
    detail::append_optional_field(output, "microphoneDeviceId", settings.microphone_device_id, line_ending, false);
    output += "}";
    return output;
}

Status SettingsCodec::save_file(
    const std::filesystem::path& path,
    const AppSettings& settings,
    std::string_view line_ending)
{
    // No settings.validate() gate: SettingsService.Save serializes whatever it
    // is handed, so a document that loaded with out-of-range UI values (an
    // out-of-bounds silenceThresholdMs, a negative temperature) has to be
    // re-saved verbatim instead of being refused. AppSettings::validate()
    // stays available for the UI/settings layer that owns clamping.

    std::filesystem::path temporary;
    bool committed = false;
    const auto cleanup = [&temporary, &committed]() noexcept {
        if (committed || temporary.empty()) {
            return;
        }
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    };

    try {
        std::error_code error;
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error) {
                return Status::failure(ErrorCode::io_failure, "cannot create settings directory: " + error.message());
            }
        }

        temporary = std::filesystem::path(path.string() + ".tmp");
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                cleanup();
                return Status::failure(ErrorCode::io_failure, "cannot open settings temporary file");
            }
            const auto text = serialize(settings, line_ending);
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.flush();
            if (!output) {
                output.close();
                cleanup();
                return Status::failure(ErrorCode::io_failure, "cannot write settings temporary file");
            }
        }

#ifdef _WIN32
        if (::MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
            const DWORD last_error = ::GetLastError();
            cleanup();
            return Status::failure(
                ErrorCode::io_failure,
                "cannot atomically replace settings file: " +
                    std::system_category().message(static_cast<int>(last_error)));
        }
#else
        std::filesystem::rename(temporary, path, error);
        if (error) {
            cleanup();
            return Status::failure(ErrorCode::io_failure, "cannot atomically replace settings file: " + error.message());
        }
#endif
        committed = true;
        return Status::success();
    } catch (const std::exception& exception) {
        cleanup();
        return Status::failure(ErrorCode::io_failure, std::string("settings save failed: ") + exception.what());
    } catch (...) {
        cleanup();
        return Status::failure(ErrorCode::io_failure, "settings save failed with an unknown exception");
    }
}

} // namespace voicetyper::domain
