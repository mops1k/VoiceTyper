#pragma once

// Linux self-update mechanics for an AppImage: which file to replace and how to
// replace it safely.
//
// The AppImage runtime exports APPIMAGE (the absolute path of the .AppImage the
// user launched) and APPDIR; this module turns that value into a plan and performs
// the swap. It is deliberately Qt-free and touches nothing but the file system, so
// the whole flow is contract-tested without an AppImage - and a build that was not
// started from an AppImage simply gets no target, which is how the composition
// knows to open the release page instead (VT-SYS-014).
//
// Why a rename and not a write: the running process keeps its own mapping of the
// old file, so renaming a new file over it is atomic and safe while the old image
// is still in use; writing into the file would fail with ETXTBSY.

#include "domain/error.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace voicetyper::core::support {

/// Suffix of the in-progress AppImage download. It sits next to the running image
/// on purpose: the replacement is a rename, and a rename only stays atomic inside
/// one file system.
inline constexpr std::string_view kAppImageDownloadSuffix = ".download";

/// The file an update replaces and the temporary file it downloads into.
struct AppImageUpdateTarget {
    std::filesystem::path image;
    std::filesystem::path download;
};

/// Resolves the AppImage the process is running from.
///
/// `appimage_env` is the value of the APPIMAGE environment variable. A target is
/// returned only when it names an existing regular file: anything else (empty
/// value, a directory, a path that is gone) means this build was not started from
/// an AppImage, and the caller falls back to opening the release page.
[[nodiscard]] std::optional<AppImageUpdateTarget> resolve_appimage_update_target(
    std::string_view appimage_env);

/// Replaces `target` with `downloaded`: makes the file executable and renames it
/// over the running image. On failure the partial download is removed and an error
/// status is returned - a half-written image must never be left where the next
/// launch would execute it.
[[nodiscard]] domain::Status replace_appimage(
    const std::filesystem::path& downloaded, const std::filesystem::path& target);

} // namespace voicetyper::core::support
