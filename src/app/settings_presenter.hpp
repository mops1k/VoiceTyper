#pragma once

// Presentation state for the settings UI, with no Qt dependency.
//
// The .NET app autosaves 700 ms after the last edit, applies the change to the
// running services immediately, and saves atomically. Keeping that here, in a
// portable presenter, means the debounce, the dirty tracking and the "never lose
// the old file" save can be contract-tested without a display server, and the Qt
// layer stays a thin view.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/settings.hpp"
#include "domain/settings_json.hpp"
#include "platform/api/clock.hpp"
#include "platform/api/file_system.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace voicetyper::app {

/// Frozen: the .NET SettingsViewModel waits 700 ms after the last change.
inline constexpr std::chrono::milliseconds kAutosaveDebounce{700};

/// What changed, so the composition root can apply it without diffing.
enum class SettingsChange : std::uint8_t {
    none = 0,
    language = 1,
    engine = 2,
    model = 3,
    microphone = 4,
    hotkeys = 5,
    appearance = 6,
    behaviour = 7,
    startup = 8,
};

[[nodiscard]] std::string_view settings_change_name(SettingsChange change) noexcept;

struct SettingsLoadReport {
    bool used_defaults = true;
    std::vector<domain::SettingsDiagnostic> diagnostics;
};

class SettingsPresenter {
public:
    SettingsPresenter(
        std::filesystem::path settings_path,
        platform::FileSystem& file_system,
        platform::Clock& clock);

    /// Reads the file. A missing file is a normal first run: defaults, no
    /// diagnostic. An unreadable or corrupt file is also defaults, with a
    /// diagnostic the UI can show - the .NET behaviour we keep.
    [[nodiscard]] SettingsLoadReport load();
    /// Applies `mutate` to the settings, marks dirty, restarts the debounce and
    /// notifies the change listener immediately (apply-before-save).
    template <typename Mutator>
    void update(SettingsChange change, Mutator&& mutate)
    {
        std::forward<Mutator>(mutate)(settings_);
        pending_ = change;
        dirty_ = true;
        ++dirty_generation_;
        last_change_ = clock_.now();
    }
    /// Writes now, if anything changed. Returns success when the file is
    /// written or already clean.
    [[nodiscard]] domain::Status flush();
    /// True when a debounced save is due (i.e. the caller should schedule it).
    [[nodiscard]] bool debounce_elapsed() const;
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] const domain::AppSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] std::filesystem::path path() const noexcept { return settings_path_; }
    void set_dirty(bool value) noexcept { dirty_ = value; }
    /// Time since the last edit, used by the debounce scheduler.
    [[nodiscard]] std::chrono::milliseconds since_last_change() const;

private:
    std::filesystem::path settings_path_;
    platform::FileSystem& file_system_;
    platform::Clock& clock_;
    domain::AppSettings settings_ = domain::AppSettings::defaults();
    std::chrono::steady_clock::time_point last_change_{};
    bool dirty_ = false;
    std::uint64_t dirty_generation_ = 0;
    SettingsChange pending_ = SettingsChange::none;
};

} // namespace voicetyper::app
