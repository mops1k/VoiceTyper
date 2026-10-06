#pragma once

// Platform path contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §2 "Windows paths and
// lifecycle" and §4 (models) / §7 (logging) / §8 (updates).
//
// This is the single place where the application decides *where* things live.
// Backends (src/platform/windows, src/platform/linux) implement it; nothing else
// in the codebase may hard-code a path. No OS header is allowed here: only the
// documented suffix table and the resolution contract are portable.
//
// Windows targets (normative for the cutover):
//   settings  %APPDATA%\VoiceTyper\settings.json
//   models    %LOCALAPPDATA%\VoiceTyper\models
//   log       %LOCALAPPDATA%\VoiceTyper\logs\voiceTyper.log
//   updates   %LOCALAPPDATA%\VoiceTyper\updates\VoiceTyper-{version}-Setup.exe
//   natives   <app dir>\parakeet.dll, <app dir>\mc_wasapi.dll
//
// Portability: a Linux backend must return its own XDG-equivalent roots; it must
// not pretend to be Windows. The .NET build's Linux port is a separate reference
// branch, and Linux self-update is intentionally out of scope (see
// docs/migration/cpp/feature-parity.md, "Current gaps that are not parity
// blockers yet").
//
// Thread affinity: every method is const, pure and callable from any thread.
// Implementations must not cache mutable state without synchronization.

#include "domain/error.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace voicetyper::platform {

/// Subdirectory name shared by every per-user root.
inline constexpr std::string_view kProductDirectoryName = "VoiceTyper";

/// Relative file/directory names, byte-identical to the .NET build.
inline constexpr std::string_view kSettingsFileName = "settings.json";
inline constexpr std::string_view kSettingsTempFileName = "settings.json.tmp";
inline constexpr std::string_view kModelsDirectoryName = "models";
inline constexpr std::string_view kLogsDirectoryName = "logs";
inline constexpr std::string_view kLogFileName = "voiceTyper.log";
inline constexpr std::string_view kUpdatesDirectoryName = "updates";
/// Launcher script written next to the downloaded installer.
inline constexpr std::string_view kUpdateLauncherFileName = "run-update.cmd";

/// Native libraries expected beside the application executable.
inline constexpr std::string_view kParakeetLibraryFileName = "parakeet.dll";
inline constexpr std::string_view kNativeWasapiLibraryFileName = "mc_wasapi.dll";

/// Resolves every application path from a single backend object.
///
/// Ownership: the returned Paths value is a plain snapshot. Backends construct it
/// once at startup; callers may keep it for the process lifetime.
class Paths {
public:
    virtual ~Paths() = default;

    Paths(const Paths&) = delete;
    Paths& operator=(const Paths&) = delete;
    Paths(Paths&&) = delete;
    Paths& operator=(Paths&&) = delete;

    /// Directory holding settings.json, e.g. %APPDATA%\VoiceTyper on Windows.
    [[nodiscard]] virtual std::filesystem::path settings_directory() const = 0;

    /// Full path of settings.json.
    [[nodiscard]] virtual std::filesystem::path settings_file() const = 0;

    /// Temporary file used by the settings save (tmp + replace).
    [[nodiscard]] virtual std::filesystem::path settings_temp_file() const = 0;

    /// Model cache root, e.g. %LOCALAPPDATA%\VoiceTyper\models.
    /// Must be the same directory the .NET build used, so a side-by-side
    /// installation shares already-downloaded models.
    [[nodiscard]] virtual std::filesystem::path models_directory() const = 0;

    /// Log directory.
    [[nodiscard]] virtual std::filesystem::path logs_directory() const = 0;

    /// Current log file (voiceTyper.log).
    [[nodiscard]] virtual std::filesystem::path log_file() const = 0;

    /// Rotated archive for `index` in 1..5 (voiceTyper.1.log .. voiceTyper.5.log).
    [[nodiscard]] virtual std::filesystem::path log_archive_file(std::uint32_t index) const = 0;

    /// Update download directory.
    [[nodiscard]] virtual std::filesystem::path updates_directory() const = 0;

    /// Full installer path for a version string, `VoiceTyper-{version}-Setup.exe`.
    [[nodiscard]] virtual std::filesystem::path update_installer_file(std::string_view version) const = 0;

    /// Temporary download target; always `<installer>.download`.
    [[nodiscard]] virtual std::filesystem::path update_installer_temp_file(std::string_view version) const = 0;

    /// Launcher script path written before the installer is started.
    [[nodiscard]] virtual std::filesystem::path update_launcher_file() const = 0;

    /// Directory containing the application executable, where the native
    /// libraries (parakeet.dll, mc_wasapi.dll) must be found.
    [[nodiscard]] virtual std::filesystem::path application_directory() const = 0;

    /// Full path of a native library beside the executable.
    [[nodiscard]] virtual std::filesystem::path native_library_file(std::string_view file_name) const = 0;

protected:
    Paths() = default;
};

/// HKCU Run-key location and value used for autostart (Windows only).
/// A non-Windows backend reports the autostart capability as unsupported through
/// LifecycleService instead of pretending a registry exists.
inline constexpr std::string_view kAutostartRegistryKeyPath = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr std::string_view kAutostartRegistryValueName = "VoiceTyper";

/// Single-instance mutex name, shared with the .NET build so the two cannot run
/// at the same time and corrupt the shared settings file.
inline constexpr std::string_view kSingleInstanceMutexName = "Global\\VoiceTyper_SingleInstance";

/// Filename template of the release installer. Must stay in sync with the
/// installer script and with the release asset naming.
inline constexpr std::string_view kInstallerFileNameFormat = "VoiceTyper-{}-Setup.exe";

/// Joins a product directory onto a per-user root and normalizes separators.
[[nodiscard]] inline std::filesystem::path product_directory(std::filesystem::path root)
{
    return root / std::filesystem::path(std::string(kProductDirectoryName));
}

/// Builds `VoiceTyper-{version}-Setup.exe` from a version string.
[[nodiscard]] inline std::string installer_file_name(std::string_view version)
{
    return "VoiceTyper-" + std::string(version) + "-Setup.exe";
}

} // namespace voicetyper::platform
