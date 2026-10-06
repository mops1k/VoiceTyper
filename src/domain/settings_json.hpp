#pragma once

// Standard-library-only settings.json codec. The wire shape and defaults are
// frozen in domain/settings.hpp; this file is the Phase B storage contract.

#include "domain/error.hpp"
#include "domain/settings.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::domain {

enum class SettingsDiagnosticKind : std::uint8_t {
    warning,
    error,
};

struct SettingsDiagnostic {
    SettingsDiagnosticKind kind = SettingsDiagnosticKind::error;
    std::string field;
    std::string message;
};

struct SettingsLoadResult {
    AppSettings settings = AppSettings::defaults();
    std::vector<SettingsDiagnostic> diagnostics;
    bool used_defaults = true;
};

class SettingsCodec final {
public:
    /// Parse a settings.json document. Invalid JSON, wrong types, and invalid
    /// enum strings follow the .NET contract: the whole document falls back to
    /// defaults. In-range numeric enums are accepted with a warning; out-of-range
    /// numeric enums warn and use the field default.
    ///
    /// Read policy, all of it decided here rather than inherited implicitly:
    ///   * Property names are matched case-insensitively and unknown properties
    ///     are ignored.
    ///   * Duplicate properties (including ones differing only in case) are
    ///     de-duplicated before any value is validated: the last occurrence
    ///     wins and earlier ones are never inspected, which is what
    ///     System.Text.Json does when it binds an object to AppSettings. An
    ///     earlier invalid value followed by a valid last value therefore does
    ///     not force a document-wide fallback.
    ///   * A JSON `null` for a *nullable* string property clears it to "unset".
    ///   * A JSON `null` for a *non-nullable* string property (recordHotkey,
    ///     cancelHotkey, termsDictionary) is accepted, stored as the empty
    ///     string and reported as a warning. Measured rationale: the real .NET
    ///     generator assigns null to a `string` property without a
    ///     JsonException, so the document still loads there; C++ cannot store
    ///     null in std::string, so empty is the closest representable value.
    ///     It is deliberately not "default + error", which would reject a
    ///     document the measured .NET behavior accepts.
    [[nodiscard]] static SettingsLoadResult load(std::string_view json);

    /// Load a file. A missing file is a normal first-run state and returns
    /// defaults with *no* diagnostic, like SettingsService.Load; a file that
    /// cannot be opened, stat'ed or parsed returns defaults with an error
    /// diagnostic. Never throws.
    [[nodiscard]] static SettingsLoadResult load_file(const std::filesystem::path& path);

    /// Serialize in the .NET WriteIndented shape. The default CRLF matches the
    /// Windows-generated golden fixture; callers may pass "\n" explicitly.
    /// A non-finite temperature is normalized to 0 (JSON has no NaN literal
    /// and System.Text.Json throws on it); this is a documented divergence.
    [[nodiscard]] static std::string serialize(
        const AppSettings& settings,
        std::string_view line_ending = "\r\n");

    /// Write to `<path>.tmp`, flush/close, then atomically replace path. A
    /// failed write/rename removes the temp file and leaves the old target
    /// alone. Never throws; every failure is reported as a Status.
    ///
    /// No settings.validate() gate: SettingsService.Save serializes whatever it
    /// is given, so a document that loaded with out-of-range UI values is
    /// re-saved verbatim. AppSettings::validate() is for the UI/settings layer,
    /// which owns clamping.
    [[nodiscard]] static Status save_file(
        const std::filesystem::path& path,
        const AppSettings& settings,
        std::string_view line_ending = "\r\n");
};

} // namespace voicetyper::domain
