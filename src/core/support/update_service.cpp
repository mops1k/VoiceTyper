#include "core/support/update_service.hpp"

#include "core/support/sha256.hpp"
#include "core/support/update_manifest.hpp"

#include <cstdio>
#include <fstream>
#include <system_error>
#include <vector>

namespace voicetyper::core::support {

UpdateService::UpdateService(platform::HttpClient& http, std::string current_version, std::string release_url)
    : http_(http)
    , current_version_(std::move(current_version))
    , release_url_(std::move(release_url))
{
}

platform::UpdateCheckResult UpdateService::check(const platform::CancellationToken& cancellation)
{
    platform::HttpRequest request;
    request.url = release_url_;
    request.headers.push_back({std::string("User-Agent"), std::string(platform::kHttpUserAgent)});
    request.headers.push_back({std::string("Accept"), std::string(platform::kGithubReleaseAccept)});

    const auto response = http_.get(request, cancellation);
    if (response.is_error()) {
        return platform::UpdateCheckResult{platform::UpdateCheckResultKind::failed, std::nullopt,
            response.error().to_string()};
    }
    // A non-2xx is not a transport error: the body carries nothing useful, but the
    // status does, and the three statuses the .NET service named keep their messages.
    if (const auto failure = failed_for_http_status(response.value().status_code); failure.has_value()) {
        return *failure;
    }
    return parse_latest_release(response.value().body, current_version_);
}

platform::Status UpdateService::download(const platform::UpdateInfo& info,
    const std::filesystem::path& target, const UpdateProgress& progress,
    const platform::CancellationToken& cancellation)
{
    if (!info.installer_url.has_value() || info.installer_url->empty()) {
        return platform::Status::failure(platform::ErrorCode::invalid_state,
            std::string(kUpdateMessageInstallerUrlMissing));
    }

    platform::HttpRequest request;
    request.url = *info.installer_url;
    request.headers.push_back({std::string("User-Agent"), std::string(platform::kHttpUserAgent)});

    auto stream = http_.open(request, cancellation);
    if (stream.is_error()) {
        return platform::Status::failure(stream.error());
    }

    std::error_code error;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), error);
    }
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) {
        return platform::Status::failure(platform::ErrorCode::io_failure,
            "the installer could not be written to " + target.string());
    }

    const std::uint64_t total = stream.value()->content_length().value_or(0);
    std::uint64_t received = 0;
    Sha256 hasher;
    const platform::Status read_status = stream.value()->read_into(
        [&](const char* data, std::size_t size) {
            file.write(data, static_cast<std::streamsize>(size));
            if (!file) {
                return false;
            }
            hasher.update(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(data), size));
            received += size;
            if (progress) {
                progress(received, total);
            }
            return true;
        },
        cancellation);
    stream.value()->close();
    file.close();

    if (read_status.is_error()) {
        std::filesystem::remove(target, error);
        return platform::Status::failure(read_status.error());
    }

    // The hash is checked before the file can be considered an installer. The .NET
    // build verified the marker in the release body; a mismatch here is a corrupt or
    // tampered download and the file must not survive.
    if (info.sha256.has_value() && !info.sha256->empty()) {
        const std::string actual = hasher.finish_hex();
        if (actual != *info.sha256) {
            std::filesystem::remove(target, error);
            return platform::Status::failure(platform::ErrorCode::io_failure,
                "the downloaded installer does not match its SHA-256: expected " + *info.sha256
                    + ", got " + actual);
        }
    }
    return platform::Status::success();
}

} // namespace voicetyper::core::support
