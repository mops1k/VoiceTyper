#pragma once

// GitHub release-manifest parsing for the update core (plan p_a95b558c6861,
// Phase 3). Reproduces VoiceTyper.Core/Services/UpdateService.cs:63-143.
//
// Why this module, and why it is this small:
//   * the wire/JSON rules must not live in the UI, and they are the only part of
//     the updater that can be contract-tested without a network;
//   * the result type, the version comparer, the 'v'-strip and the SHA-256
//     marker parser are the *already frozen* portable contract of
//     src/platform/api/updater.hpp. This module uses them instead of defining a
//     second UpdateInfo or a second comparer, so the .NET-compatible comparison
//     has exactly one implementation (docs/migration/cpp/compatibility-contracts.md
//     §8 "Version comparison" requires reproducing it, not re-inventing it).
//
// Dependency: QtCore, for QJsonDocument only, and only in the .cpp. The header
// stays Qt-free, so a caller that needs just the types does not pull Qt in.
//
// Failure policy: a failure is always UpdateCheckResultKind::failed with a
// message, never an exception and never a half-built result. Two deliberate
// differences from the .NET reference, both so that no exception can escape:
//   * a non-string tag_name or a non-numeric size is a protocol change that the
//     C# code turned into an uncaught InvalidOperationException; here it
//     degrades (absent version, absent size) and the ordinary checks take over;
//   * a JSON root that is not an object fails with the malformed-response
//     message instead of throwing out of JsonElement.TryGetProperty.
//
// Threading: parse_latest_release is pure and safe from any thread. Ownership:
// the returned UpdateCheckResult is a value.

#include "platform/api/updater.hpp"

#include <optional>
#include <string_view>

namespace voicetyper::core::support {

/// The installer asset pattern, UpdateService.cs:37 and
/// compatibility-contracts.md §8 "Release query". Kept as text so the contract
/// test can pin the regex itself, not only its behaviour.
inline constexpr std::string_view kSetupAssetRegex = R"(^VoiceTyper-\d[^/]*?-Setup\.exe$)";

// Failure texts of the release query. The .NET service produced user-facing
// Russian strings; the C++ core layer keeps diagnostics in English, the way
// src/domain and src/platform do, and the localized wording belongs to the
// update UI (Phase 4 of the same plan), which can key off the result kind.
/// HTTP 404: the release/repository is gone or private.
inline constexpr std::string_view kUpdateMessageReleaseNotFound
    = "Release not found. The repository is unavailable or private.";
/// HTTP 403.
inline constexpr std::string_view kUpdateMessageAccessDenied = "GitHub access denied (403).";
/// HTTP 429.
inline constexpr std::string_view kUpdateMessageRateLimited = "GitHub rate limit exceeded.";
/// The body is not a JSON object (or not JSON at all).
inline constexpr std::string_view kUpdateMessageMalformedResponse = "Invalid server response.";
/// No asset matches kSetupAssetRegex.
inline constexpr std::string_view kUpdateMessageInstallerMissing = "Installer not found in the release.";
/// The matching asset exists but names no download URL.
inline constexpr std::string_view kUpdateMessageInstallerUrlMissing = "The installer has no download URL.";

/// True when `asset_name` identifies the Inno Setup installer.
///
/// Matches `Regex.IsMatch(name, @"^VoiceTyper-\d[^/]*?-Setup\.exe$")` exactly
/// (a digit right after the prefix, no '/' in between, anchored at both ends).
[[nodiscard]] bool is_setup_asset_name(std::string_view asset_name);

/// Parses one GitHub "releases/latest" body and decides whether it offers a
/// version newer than `current_version`.
///
/// `json` is already-fetched body text; the HTTP call and the status-code
/// mapping stay outside (see failed_for_http_status). Never throws.
///
/// Order of the checks reproduces the .NET one and is observable:
/// asset and URL first, SHA marker, then the version comparison - so a release
/// with no installer is a failure even when its tag is not newer.
[[nodiscard]] platform::UpdateCheckResult parse_latest_release(
    std::string_view json, std::string_view current_version);

/// The failure a non-success release-query status maps to, or nullopt for 2xx,
/// where the caller parses the body.
///
/// Covers the three statuses the .NET service special-cased (404, 403, 429);
/// every other non-2xx keeps its code in the message, because a bare "request
/// failed" tells neither the log nor the user anything.
[[nodiscard]] std::optional<platform::UpdateCheckResult> failed_for_http_status(int status_code);

} // namespace voicetyper::core::support
