#include "domain/app_paths.hpp"

#include <utility>

namespace voicetyper::domain {
namespace {

std::filesystem::path root_from_environment(const EnvironmentLookup& environment, std::string_view name)
{
    if (!environment) {
        return {};
    }
    const auto value = environment(name);
    if (!value.has_value() || value->empty()) {
        return {};
    }
    return std::filesystem::path(*value);
}

} // namespace

AppPathRoots windows_app_path_roots(
    const EnvironmentLookup& environment, std::filesystem::path application_directory)
{
    AppPathRoots roots;
    roots.roaming = root_from_environment(environment, "APPDATA");
    roots.local = root_from_environment(environment, "LOCALAPPDATA");
    roots.application = std::move(application_directory);

    if (roots.roaming.empty() && roots.local.empty()) {
        roots.roaming = roots.application;
        roots.local = roots.application;
    } else if (roots.roaming.empty()) {
        roots.roaming = roots.local;
    } else if (roots.local.empty()) {
        roots.local = roots.roaming;
    }
    return roots;
}

AppPaths::AppPaths(AppPathRoots roots)
    : roots_(std::move(roots))
{
}

std::filesystem::path AppPaths::settings_directory() const
{
    return platform::product_directory(roots_.roaming);
}

std::filesystem::path AppPaths::settings_file() const
{
    return settings_directory() / std::filesystem::path(std::string(platform::kSettingsFileName));
}

std::filesystem::path AppPaths::settings_temp_file() const
{
    return settings_directory() / std::filesystem::path(std::string(platform::kSettingsTempFileName));
}

std::filesystem::path AppPaths::models_directory() const
{
    return platform::product_directory(roots_.local) /
        std::filesystem::path(std::string(platform::kModelsDirectoryName));
}

std::filesystem::path AppPaths::logs_directory() const
{
    return platform::product_directory(roots_.local) /
        std::filesystem::path(std::string(platform::kLogsDirectoryName));
}

std::filesystem::path AppPaths::log_file() const
{
    return logs_directory() / std::filesystem::path(std::string(platform::kLogFileName));
}

std::filesystem::path AppPaths::log_archive_file(std::uint32_t index) const
{
    return logs_directory() / ("voiceTyper." + std::to_string(index) + ".log");
}

std::filesystem::path AppPaths::updates_directory() const
{
    return platform::product_directory(roots_.local) /
        std::filesystem::path(std::string(platform::kUpdatesDirectoryName));
}

std::filesystem::path AppPaths::update_installer_file(std::string_view version) const
{
    return updates_directory() / platform::installer_file_name(version);
}

std::filesystem::path AppPaths::update_installer_temp_file(std::string_view version) const
{
    return updates_directory() / (platform::installer_file_name(version) + ".download");
}

std::filesystem::path AppPaths::update_launcher_file() const
{
    return updates_directory() / std::filesystem::path(std::string(platform::kUpdateLauncherFileName));
}

std::filesystem::path AppPaths::application_directory() const
{
    return roots_.application;
}

std::filesystem::path AppPaths::native_library_file(std::string_view file_name) const
{
    return roots_.application / std::filesystem::path(std::string(file_name));
}

} // namespace voicetyper::domain
