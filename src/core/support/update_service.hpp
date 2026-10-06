#pragma once

// The update flow: query the release feed, download the installer, verify it.
//
// Reproduces VoiceTyper.Core/Services/UpdateService.cs (query + download + SHA-256)
// and VoiceTyper.App/Services/UpdateLauncher.cs (running the installer), on top of the
// frozen seams of src/platform/api/http.hpp and src/platform/api/updater.hpp.
//
// Everything here works against platform::HttpClient, so the whole flow - status
// mapping, JSON parsing (update_manifest.hpp), streaming to disk, progress reporting
// and the hash check - is contract-tested with a fake client and no network. That is
// the point of the split: the .NET service mixed HTTP, parsing and file IO in one
// class and could only be tested with a mocked HttpClient; here the parsing is already
// separate and this class is the part that touches the network.

#include "platform/api/http.hpp"
#include "platform/api/updater.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace voicetyper::core::support {

/// The release feed this application updates from, frozen in the .NET build
/// (UpdateService.cs:33-37: Repository "mops1k/VoiceTyper", BaseUrl + "/releases/latest").
inline constexpr std::string_view kUpdateLatestReleaseUrl
    = "https://api.github.com/repos/mops1k/VoiceTyper/releases/latest";

/// Progress of a download: bytes written so far and the total when the server sent a
/// Content-Length (zero when it did not, which the UI shows as an indeterminate bar).
using UpdateProgress = std::function<void(std::uint64_t received, std::uint64_t total)>;

class UpdateService {
public:
    UpdateService(platform::HttpClient& http, std::string current_version,
        std::string release_url = std::string(kUpdateLatestReleaseUrl));

    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    /// GETs the release feed with the frozen headers and decides whether it offers a
    /// version newer than current_version().
    ///
    /// A transport failure, a non-2xx status and a malformed body all come back as
    /// UpdateCheckResultKind::failed with a readable message; nothing throws.
    [[nodiscard]] platform::UpdateCheckResult check(const platform::CancellationToken& cancellation);

    /// Streams the installer into `target`, hashing while it writes, and verifies the
    /// result against `info.sha256` when that is not empty.
    ///
    /// A file that fails verification is removed: a half-verified installer must never
    /// be left where a later launch could run it.
    [[nodiscard]] platform::Status download(const platform::UpdateInfo& info,
        const std::filesystem::path& target, const UpdateProgress& progress,
        const platform::CancellationToken& cancellation);

    [[nodiscard]] const std::string& current_version() const noexcept { return current_version_; }
    [[nodiscard]] const std::string& release_url() const noexcept { return release_url_; }

private:
    platform::HttpClient& http_;
    std::string current_version_;
    std::string release_url_;
};

} // namespace voicetyper::core::support
