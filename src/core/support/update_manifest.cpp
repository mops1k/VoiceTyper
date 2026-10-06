#include "core/support/update_manifest.hpp"

// Qt is used here and only here: QJsonDocument/QJsonObject for the wire format
// and QRegularExpression for the asset pattern, which is applied with the same
// PCRE anchoring and \d semantics as the .NET Regex.IsMatch it reproduces.
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>

#include <cstdint>
#include <string>
#include <utility>

namespace voicetyper::core::support {
namespace {

using platform::UpdateCheckResult;
using platform::UpdateCheckResultKind;
using platform::UpdateInfo;

/// A 64-bit JSON number arrives as double. A byte count at or above 2^64 cannot
/// be represented, so it is dropped instead of being truncated into an arbitrary
/// value (the .NET GetInt64 threw on it, and the port must not).
constexpr double kLargestSizeExclusive = 18446744073709551616.0; // 2^64

[[nodiscard]] UpdateCheckResult failed(std::string_view message)
{
    UpdateCheckResult result;
    result.kind = UpdateCheckResultKind::failed;
    result.error = std::string(message);
    return result;
}

[[nodiscard]] UpdateCheckResult up_to_date()
{
    UpdateCheckResult result;
    result.kind = UpdateCheckResultKind::up_to_date;
    return result;
}

[[nodiscard]] bool is_blank(std::string_view text) noexcept
{
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

/// The compiled asset pattern, built once. QRegularExpression is immutable after
/// construction and match() is const, so this is safe to share across threads -
/// the same guarantee the process-wide Regex cache gave the .NET service.
[[nodiscard]] const QRegularExpression& setup_asset_pattern()
{
    static const QRegularExpression pattern(QString::fromUtf8(
        kSetupAssetRegex.data(), static_cast<qsizetype>(kSetupAssetRegex.size())));
    return pattern;
}

[[nodiscard]] QString to_qstring(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

} // namespace

bool is_setup_asset_name(std::string_view asset_name)
{
    if (asset_name.empty()) {
        return false;
    }
    return setup_asset_pattern().match(to_qstring(asset_name)).hasMatch();
}

platform::UpdateCheckResult parse_latest_release(std::string_view json, std::string_view current_version)
{
    // Blank input is not "a repository with no release": it is a body that never
    // arrived. QJsonDocument would also reject it, but saying so explicitly keeps
    // the reason independent of the JSON library's whitespace rules.
    if (json.empty() || is_blank(json)) {
        return failed(kUpdateMessageMalformedResponse);
    }

    QJsonParseError parse_error{};
    const QByteArray body(json.data(), static_cast<qsizetype>(json.size()));
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        // A non-object root (array, scalar) is a protocol change. The .NET code
        // reached JsonElement.TryGetProperty and threw out of the service; here
        // it is the same failure as broken JSON.
        return failed(kUpdateMessageMalformedResponse);
    }

    const QJsonObject root = document.object();

    // TrimStart('v'), not strict SemVer (compatibility-contracts.md §8).
    const QJsonValue tag_value = root.value(QStringLiteral("tag_name"));
    const std::string version = tag_value.isString()
        ? platform::strip_version_tag_prefix(tag_value.toString().toStdString())
        : std::string{};

    const QJsonValue notes_value = root.value(QStringLiteral("body"));
    const bool has_notes = notes_value.isString();
    const std::string release_notes = has_notes ? notes_value.toString().toStdString() : std::string{};

    // Stored, never filtered: a prerelease is offered like any other release.
    const bool is_prerelease = root.value(QStringLiteral("prerelease")).toBool(false);

    const QJsonValue assets_value = root.value(QStringLiteral("assets"));
    if (!assets_value.isArray()) {
        return failed(kUpdateMessageInstallerMissing);
    }

    std::optional<std::string> installer_url;
    std::optional<std::uint64_t> size_bytes;
    bool asset_found = false;
    for (const QJsonValue& item : assets_value.toArray()) {
        const QJsonObject asset = item.toObject();
        const QJsonValue name_value = asset.value(QStringLiteral("name"));
        if (!name_value.isString()) {
            continue;
        }
        const std::string name = name_value.toString().toStdString();
        if (is_blank(name) || !is_setup_asset_name(name)) {
            continue;
        }

        // First matching asset in API order wins. Nothing here prefers the
        // highest-looking version name; that would be a new policy.
        const QJsonValue url_value = asset.value(QStringLiteral("browser_download_url"));
        if (url_value.isString()) {
            std::string url = url_value.toString().toStdString();
            if (!is_blank(url)) {
                installer_url = std::move(url);
            }
        }

        const QJsonValue size_value = asset.value(QStringLiteral("size"));
        if (size_value.isDouble()) {
            const double size = size_value.toDouble();
            if (size >= 0.0 && size < kLargestSizeExclusive) {
                size_bytes = static_cast<std::uint64_t>(size);
            }
        }

        asset_found = true;
        break;
    }

    if (!asset_found) {
        return failed(kUpdateMessageInstallerMissing);
    }
    if (!installer_url.has_value()) {
        return failed(kUpdateMessageInstallerUrlMissing);
    }

    // Asset and URL first, then the comparison: a release with no installer is a
    // failure even when its tag is not newer, exactly as the .NET order had it.
    if (platform::compare_update_versions(version, current_version) <= 0) {
        return up_to_date();
    }

    UpdateInfo info;
    info.version = version;
    info.installer_url = installer_url;
    info.sha256 = platform::extract_release_sha256(release_notes);
    info.size_bytes = size_bytes;
    if (has_notes) {
        info.release_notes = release_notes;
    }
    info.is_prerelease = is_prerelease;

    UpdateCheckResult result;
    result.kind = UpdateCheckResultKind::update_available;
    result.update = std::move(info);
    return result;
}

std::optional<platform::UpdateCheckResult> failed_for_http_status(int status_code)
{
    if (status_code >= 200 && status_code < 300) {
        return std::nullopt;
    }

    switch (status_code) {
    case 404:
        return failed(kUpdateMessageReleaseNotFound);
    case 403:
        return failed(kUpdateMessageAccessDenied);
    case 429:
        return failed(kUpdateMessageRateLimited);
    default:
        break;
    }

    return failed("Request failed with HTTP status " + std::to_string(status_code) + '.');
}

} // namespace voicetyper::core::support
