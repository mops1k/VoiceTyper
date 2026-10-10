// Contract for the Linux self-update file mechanics: resolving the running AppImage
// from the environment and replacing it atomically. File-system only, no AppImage,
// no Qt, no network.

#include "core/support/appimage_update.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

using namespace voicetyper;
using namespace voicetyper::core::support;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

std::filesystem::path scratch()
{
    const std::filesystem::path dir
        = std::filesystem::temp_directory_path() / "voicetyper-appimage-update-contract";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void check_target_resolution()
{
    const std::filesystem::path dir = scratch();
    const std::filesystem::path image = dir / "VoiceTyper-3.0.0-x86_64.AppImage";
    write_file(image, "old image");

    // An empty APPIMAGE means the build was not started from an AppImage (source
    // build, package install) - the composition then opens the release page.
    check(!resolve_appimage_update_target("").has_value(), "an empty APPIMAGE yields no target");

    check(!resolve_appimage_update_target((dir / "missing.AppImage").string()).has_value(),
        "a path that does not exist yields no target");

    check(!resolve_appimage_update_target(dir.string()).has_value(),
        "a directory yields no target");

    const std::optional<AppImageUpdateTarget> target
        = resolve_appimage_update_target(image.string());
    check(target.has_value(), "an existing regular file yields a target");
    if (target.has_value()) {
        check(target->image == image, "the image is the path from the environment");
        check(target->download
                == std::filesystem::path(image.string() + std::string(kAppImageDownloadSuffix)),
            "the download sits next to the image with the .download suffix");
    }
}

void check_replacement()
{
    const std::filesystem::path dir = scratch();
    const std::filesystem::path image = dir / "VoiceTyper-3.0.0-x86_64.AppImage";
    const std::filesystem::path download = dir / "VoiceTyper-3.0.1-x86_64.AppImage.download";
    write_file(image, "old image");
    write_file(download, "new image");

    const domain::Status status = replace_appimage(download, image);
    check(status.is_ok(), "replacing an existing image succeeds");
    check(read_file(image) == "new image", "the running image now holds the download");
    check(!std::filesystem::exists(download), "the temporary download is gone after the swap");

    const std::filesystem::perms permissions = std::filesystem::status(image).permissions();
    const bool executable
        = (permissions & std::filesystem::perms::owner_exec) != std::filesystem::perms::none
        && (permissions & std::filesystem::perms::group_exec) != std::filesystem::perms::none
        && (permissions & std::filesystem::perms::others_exec) != std::filesystem::perms::none;
    check(executable, "the replaced image is executable for everyone");

    // A missing download is an error and must leave the running image untouched.
    const domain::Status failed = replace_appimage(dir / "absent.download", image);
    check(failed.is_error(), "a missing download fails");
    check(read_file(image) == "new image", "a failed replacement leaves the running image alone");
}

} // namespace

int main()
{
    check_target_resolution();
    check_replacement();

    if (failures != 0) {
        std::cerr << "appimage-update-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "appimage-update-contract: OK\n";
    return 0;
}
