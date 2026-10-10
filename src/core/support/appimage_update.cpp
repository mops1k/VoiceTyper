#include "core/support/appimage_update.hpp"

#include <system_error>
#include <utility>

namespace voicetyper::core::support {
namespace {

[[nodiscard]] domain::Status io_failure(std::string_view what, const std::error_code& code)
{
    return domain::Status::failure(
        domain::ErrorCode::io_failure, std::string(what) + ": " + code.message());
}

} // namespace

std::optional<AppImageUpdateTarget> resolve_appimage_update_target(std::string_view appimage_env)
{
    if (appimage_env.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path image{std::string(appimage_env)};
    std::error_code code;
    if (!std::filesystem::is_regular_file(image, code) || code) {
        return std::nullopt;
    }
    AppImageUpdateTarget target;
    target.image = image;
    target.download = image;
    target.download += kAppImageDownloadSuffix;
    return target;
}

domain::Status replace_appimage(
    const std::filesystem::path& downloaded, const std::filesystem::path& target)
{
    std::error_code code;
    if (!std::filesystem::is_regular_file(downloaded, code) || code) {
        return domain::Status::failure(domain::ErrorCode::io_failure,
            "the downloaded AppImage is missing: " + downloaded.string());
    }

    // 0755, so the image stays runnable after the swap (a download has no
    // executable bit at all, and the replaced file's mode does not carry over a
    // rename).
    std::filesystem::permissions(downloaded,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read
            | std::filesystem::perms::group_exec | std::filesystem::perms::others_read
            | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace, code);
    if (code) {
        std::error_code cleanup;
        std::filesystem::remove(downloaded, cleanup);
        return io_failure("cannot make the downloaded AppImage executable", code);
    }

    // Atomic replacement: the running process keeps the old file mapped, so the
    // swap is invisible to it and a crash cannot leave a truncated image behind.
    std::filesystem::rename(downloaded, target, code);
    if (code) {
        std::error_code cleanup;
        std::filesystem::remove(downloaded, cleanup);
        return io_failure("cannot replace the running AppImage", code);
    }
    return domain::Status::success();
}

} // namespace voicetyper::core::support
