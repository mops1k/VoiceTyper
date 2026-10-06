#pragma once

// Update check and installer download contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §8 "Updater and
// installer"; .NET reference VoiceTyper.Core/Services/UpdateService.cs,
// VoiceTyper.Core/Models/UpdateInfo.cs and VoiceTyper.App/Services/UpdateLauncher.cs.
//
// Frozen observable contract:
//   * Release metadata comes from the GitHub "latest release" API of
//     mops1k/VoiceTyper. The tag is stripped of a leading 'v' with TrimStart
//     semantics, not with strict semver parsing.
//   * The installer is the first asset, in API order, whose name matches
//     "VoiceTyper-<digit>...-Setup.exe". Architecture-suffixed asset names are
//     matched by the same prefix rule today; picking a per-architecture asset is
//     an open decision and is not made here.
//   * The release body may carry a SHA-256 marker on a line of the form
//     "SHA256: <64 hex>" or "SHA-256: <64 hex>", matched case-insensitively.
//     A missing marker is accepted today.
//   * Version comparison is *not* strict SemVer: build metadata after '+' is
//     ignored, missing numeric segments are zero, a non-numeric segment is zero,
//     stable is newer than any prerelease, two prereleases with an equal numeric
//     core compare equal, and whitespace is not trimmed. Reproducing that
//     comparison is required for the overlap window in which the .NET and C++
//     builds coexist.
//   * The installer downloads to "VoiceTyper-<version>-Setup.exe.download" and is
//     moved onto the target afterwards. A SHA-256 mismatch deletes the temporary
//     file and leaves an existing target untouched.
//
// Decisions intentionally NOT made here (they need a staged, approved policy per
// compatibility-contracts.md §8):
//   * requiring a SHA-256 marker;
//   * strict semver tags and Authenticode;
//   * an argv-safe, atomic launcher script and explicit rollback;
//   * Windows-style self-update on Linux.
//
// Thread affinity: check() and download_installer() block and belong on a worker
// thread. Ownership: the returned UpdateInfo is a value; the sink is owned by the
// caller.
//
// A caller rule that IS part of the contract: an update must never be triggered
// while a recording or processing session is running. That ordering is enforced
// by the caller in Phase D, and is recorded here so the constraint is not lost.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::platform {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// GitHub repository that publishes the releases.
inline constexpr std::string_view kUpdateRepository = "mops1k/VoiceTyper";
/// Base URL of the release API; the check appends "/releases/latest".
inline constexpr std::string_view kUpdateApiBaseUrl = "https://api.github.com/repos/mops1k/VoiceTyper";
/// Release asset name prefix/suffix that identifies the installer.
inline constexpr std::string_view kSetupAssetNamePrefix = "VoiceTyper-";
inline constexpr std::string_view kSetupAssetNameSuffix = "-Setup.exe";
/// Suffix of the in-progress installer download.
inline constexpr std::string_view kInstallerDownloadSuffix = ".download";

/// Outcome of a release check. The .NET UpdateCheckResultKind ordinals are
/// preserved: up_to_date = 0, update_available = 1, failed = 2.
enum class UpdateCheckResultKind : std::uint8_t {
    up_to_date = 0,
    update_available = 1,
    failed = 2,
};

[[nodiscard]] constexpr std::string_view update_check_kind_name(UpdateCheckResultKind kind) noexcept
{
    switch (kind) {
    case UpdateCheckResultKind::up_to_date: return "up_to_date";
    case UpdateCheckResultKind::update_available: return "update_available";
    case UpdateCheckResultKind::failed: return "failed";
    }
    return "unknown";
}

/// Metadata of an available update, mirroring the .NET UpdateInfo record.
struct UpdateInfo {
    /// Version with a leading 'v' already stripped, e.g. "1.1.3".
    std::string version;
    /// Direct download URL of the installer asset.
    std::optional<std::string> installer_url;
    /// SHA-256 from the release body, when a marker was present.
    std::optional<std::string> sha256;
    /// Asset size in bytes, when the API reported it.
    std::optional<std::uint64_t> size_bytes;
    /// Raw release body.
    std::optional<std::string> release_notes;
    /// Stored but not filtered, matching the .NET behavior.
    bool is_prerelease = false;
};

/// Result of a release check. `error` is human-readable and user-facing.
struct UpdateCheckResult {
    UpdateCheckResultKind kind = UpdateCheckResultKind::up_to_date;
    /// Set only when `kind` is update_available.
    std::optional<UpdateInfo> update;
    /// Set only when `kind` is failed.
    std::string error;

    [[nodiscard]] bool is_up_to_date() const noexcept { return kind == UpdateCheckResultKind::up_to_date; }
    [[nodiscard]] bool is_available() const noexcept { return kind == UpdateCheckResultKind::update_available; }
    [[nodiscard]] bool is_failed() const noexcept { return kind == UpdateCheckResultKind::failed; }
};

/// Receives download progress as a 0..1 fraction on the download thread.
using UpdateProgressSink = std::function<void(double fraction)>;

/// Compares two version strings the way the current updater does.
///
/// This is NOT strict SemVer; see the header comment. It exists so both builds
/// agree during the overlap window, and so any change to it is a deliberate,
/// tested migration step.
[[nodiscard]] int compare_update_versions(std::string_view a, std::string_view b);

/// Strips leading 'v' characters, matching `TrimStart('v')` semantics.
[[nodiscard]] std::string strip_version_tag_prefix(std::string_view tag);

/// Extracts a SHA-256 marker from a release body, or nullopt when absent.
/// Accepts "SHA256:" and "SHA-256:" case-insensitively on a whole line.
[[nodiscard]] std::optional<std::string> extract_release_sha256(std::string_view release_body);

// --- Inline implementations of the frozen comparison rules -------------------
//
// These reproduce the *current* .NET behavior, which is what the overlap window
// between the two builds requires. They are not a proposed improvement: a
// strict-SemVer version would be a versioned migration decision, recorded
// separately.

namespace detail {

/// Parses a version into its numeric segments and optional prerelease suffix,
/// dropping build metadata after '+'. A non-numeric or overflowing segment
/// becomes 0, and a missing segment is treated as 0 by the comparison.
struct ParsedVersion {
    std::vector<std::uint64_t> numbers;
    bool has_prerelease = false;
};

[[nodiscard]] inline bool parse_decimal_segment(std::string_view text, std::uint64_t& out)
{
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return false; // overflow -> zero, as in the .NET TryParse path
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

[[nodiscard]] inline ParsedVersion parse_version(std::string_view version)
{
    ParsedVersion parsed;
    if (version.empty()) {
        return parsed;
    }

    std::string_view core = version;
    const std::size_t plus = core.find('+');
    if (plus != std::string_view::npos) {
        core = core.substr(0, plus);
    }

    const std::size_t dash = core.find('-');
    if (dash != std::string_view::npos) {
        parsed.has_prerelease = true;
        core = core.substr(0, dash);
    }

    std::size_t start = 0;
    while (true) {
        const std::size_t dot = core.find('.', start);
        const std::string_view segment
            = core.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        std::uint64_t value = 0;
        if (!parse_decimal_segment(segment, value)) {
            value = 0;
        }
        parsed.numbers.push_back(value);
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return parsed;
}

[[nodiscard]] inline char lower_ascii(char ch) noexcept
{
    const auto byte = static_cast<unsigned char>(ch);
    return (byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte - 'A' + 'a') : ch;
}

} // namespace detail

inline int compare_update_versions(std::string_view a, std::string_view b)
{
    const detail::ParsedVersion left = detail::parse_version(a);
    const detail::ParsedVersion right = detail::parse_version(b);

    const std::size_t length = left.numbers.size() > right.numbers.size() ? left.numbers.size()
                                                                          : right.numbers.size();
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint64_t x = i < left.numbers.size() ? left.numbers[i] : 0;
        const std::uint64_t y = i < right.numbers.size() ? right.numbers[i] : 0;
        if (x != y) {
            return x > y ? 1 : -1;
        }
    }

    // Equal numeric core: having no prerelease suffix is newer. Two prereleases
    // with an equal core compare equal regardless of their identifiers.
    if (left.has_prerelease != right.has_prerelease) {
        return left.has_prerelease ? -1 : 1;
    }
    return 0;
}

inline std::string strip_version_tag_prefix(std::string_view tag)
{
    std::size_t start = 0;
    while (start < tag.size() && tag[start] == 'v') {
        ++start;
    }
    return std::string(tag.substr(start));
}

inline std::optional<std::string> extract_release_sha256(std::string_view release_body)
{
    std::size_t position = 0;
    while (position <= release_body.size()) {
        std::size_t end = release_body.find('\n', position);
        if (end == std::string_view::npos) {
            end = release_body.size();
        }

        const std::string_view line = release_body.substr(position, end - position);

        // Trim leading/trailing whitespace, then match "SHA256:"/"SHA-256:".
        std::size_t begin = 0;
        while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t' || line[begin] == '\r')) {
            ++begin;
        }
        std::size_t last = line.size();
        while (last > begin
            && (line[last - 1] == ' ' || line[last - 1] == '\t' || line[last - 1] == '\r')) {
            --last;
        }
        const std::string_view trimmed = line.substr(begin, last - begin);

        std::size_t marker = 0;
        if (trimmed.size() > 7 && detail::lower_ascii(trimmed[0]) == 's' && detail::lower_ascii(trimmed[1]) == 'h'
            && detail::lower_ascii(trimmed[2]) == 'a') {
            marker = 3;
            if (detail::lower_ascii(trimmed[3]) == '-') {
                ++marker;
            }
        }

        if (marker != 0 && trimmed.size() > marker + 3 && trimmed[marker] == '2' && trimmed[marker + 1] == '5'
            && trimmed[marker + 2] == '6' && trimmed[marker + 3] == ':') {
            std::size_t value_start = marker + 4;
            while (value_start < trimmed.size() && (trimmed[value_start] == ' ' || trimmed[value_start] == '\t')) {
                ++value_start;
            }
            std::size_t value_end = trimmed.size();
            while (value_end > value_start && (trimmed[value_end - 1] == ' ' || trimmed[value_end - 1] == '\t')) {
                --value_end;
            }
            const std::string_view digest = trimmed.substr(value_start, value_end - value_start);
            if (digest.size() == 64) {
                bool all_hex = true;
                for (const char ch : digest) {
                    const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')
                        || (ch >= 'A' && ch <= 'F');
                    if (!hex) {
                        all_hex = false;
                        break;
                    }
                }
                if (all_hex) {
                    return std::string(digest);
                }
            }
        }

        if (end == release_body.size()) {
            break;
        }
        position = end + 1;
    }
    return std::nullopt;
}

class UpdateService {
public:
    virtual ~UpdateService() = default;

    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;
    UpdateService(UpdateService&&) = delete;
    UpdateService& operator=(UpdateService&&) = delete;

    /// Directory the installer is downloaded into.
    [[nodiscard]] virtual std::filesystem::path updates_directory() const = 0;

    /// Target path for a version: "VoiceTyper-<version>-Setup.exe".
    [[nodiscard]] virtual std::filesystem::path installer_path(std::string_view version) const = 0;

    /// Temporary download path: the installer path plus ".download".
    [[nodiscard]] virtual std::filesystem::path installer_temp_path(std::string_view version) const = 0;

    /// Checks the release API for something newer than `current_version`.
    ///
    /// Network and protocol problems are *not* exceptions and not a hard failure:
    /// they are reported as a failed UpdateCheckResult, exactly as the .NET
    /// service does, so the UI can show a message and retry later. Only an
    /// explicit cancellation propagates as ErrorCode::cancelled.
    [[nodiscard]] virtual Result<UpdateCheckResult> check_for_update(
        std::string_view current_version, const CancellationToken& cancellation) = 0;

    /// Downloads the installer into the updates directory.
    ///
    /// A SHA-256 marker, when present, is verified case-insensitively; on
    /// mismatch the temporary file is deleted and any existing target is left
    /// alone, reported as corrupt_data. Failure codes: invalid_argument (no
    /// installer URL), unavailable, timeout, cancelled, io_failure,
    /// corrupt_data (checksum mismatch).
    [[nodiscard]] virtual Result<std::filesystem::path> download_installer(
        const UpdateInfo& update, const UpdateProgressSink& progress, const CancellationToken& cancellation) = 0;

protected:
    UpdateService() = default;
};

} // namespace voicetyper::platform
