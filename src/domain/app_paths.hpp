#pragma once

// Portable implementation of the platform::Paths contract. It takes the
// per-user roots from a platform backend, so this file has no OS API calls and
// can be tested identically on Windows and Arch.

#include "platform/api/paths.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace voicetyper::domain {

/// Roots supplied by a platform backend:
///   roaming  -> %APPDATA% on Windows, XDG config on Arch;
///   local    -> %LOCALAPPDATA% on Windows, XDG data on Arch;
///   app      -> the directory containing the running executable.
struct AppPathRoots {
    std::filesystem::path roaming;
    std::filesystem::path local;
    std::filesystem::path application;
};

/// Environment lookup used by the resolver below. Returns nullopt for an unset
/// or empty variable. Injected so the layout is contract-tested on any host.
using EnvironmentLookup = std::function<std::optional<std::string>(std::string_view)>;

/// The Windows per-user roots, exactly as the .NET build defines them
/// (`SettingsService.cs:37-41` uses `SpecialFolder.ApplicationData` = Roaming;
/// `FileLogger.cs:22-25`, `ModelManager.cs:68-71` and `UpdateService.cs:46-48`
/// use `SpecialFolder.LocalApplicationData` = Local):
///
///   settings              -> <roaming>\VoiceTyper\settings.json
///   models, logs, updates -> <local>\VoiceTyper\...
///   native libraries      -> `application_directory`
///
/// A missing root falls back to the other one and finally to
/// `application_directory`, so a stripped environment can never produce an empty
/// path. This function exists because a composition root that builds its own
/// path string is exactly how settings end up in the wrong Windows profile and a
/// real user's settings are silently not loaded.
[[nodiscard]] AppPathRoots windows_app_path_roots(
    const EnvironmentLookup& environment, std::filesystem::path application_directory);

/// Concrete immutable Paths value for the roots above. Construction is pure and
/// const methods are safe from any thread.
class AppPaths final : public platform::Paths {
public:
    explicit AppPaths(AppPathRoots roots);

    [[nodiscard]] std::filesystem::path settings_directory() const override;
    [[nodiscard]] std::filesystem::path settings_file() const override;
    [[nodiscard]] std::filesystem::path settings_temp_file() const override;
    [[nodiscard]] std::filesystem::path models_directory() const override;
    [[nodiscard]] std::filesystem::path logs_directory() const override;
    [[nodiscard]] std::filesystem::path log_file() const override;
    [[nodiscard]] std::filesystem::path log_archive_file(std::uint32_t index) const override;
    [[nodiscard]] std::filesystem::path updates_directory() const override;
    [[nodiscard]] std::filesystem::path update_installer_file(std::string_view version) const override;
    [[nodiscard]] std::filesystem::path update_installer_temp_file(std::string_view version) const override;
    [[nodiscard]] std::filesystem::path update_launcher_file() const override;
    [[nodiscard]] std::filesystem::path application_directory() const override;
    [[nodiscard]] std::filesystem::path native_library_file(std::string_view file_name) const override;

private:
    AppPathRoots roots_;
};

} // namespace voicetyper::domain
