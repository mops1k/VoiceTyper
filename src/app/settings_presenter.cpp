#include "app/settings_presenter.hpp"

#include <utility>

namespace voicetyper::app {

using domain::ErrorCode;

std::string_view settings_change_name(SettingsChange change) noexcept
{
    switch (change) {
    case SettingsChange::none: return "none";
    case SettingsChange::language: return "language";
    case SettingsChange::engine: return "engine";
    case SettingsChange::model: return "model";
    case SettingsChange::microphone: return "microphone";
    case SettingsChange::hotkeys: return "hotkeys";
    case SettingsChange::appearance: return "appearance";
    case SettingsChange::behaviour: return "behaviour";
    case SettingsChange::startup: return "startup";
    }
    return "unknown";
}

SettingsPresenter::SettingsPresenter(
    std::filesystem::path settings_path,
    platform::FileSystem& file_system,
    platform::Clock& clock)
    : settings_path_(std::move(settings_path))
    , file_system_(file_system)
    , clock_(clock)
    , last_change_(clock_.now())
{
}

SettingsLoadReport SettingsPresenter::load()
{
    // Read through the FileSystem port, not through std::filesystem, so the
    // presenter is testable without a real disk and the Windows backend controls
    // how the file is opened.
    const auto text = file_system_.read_text(settings_path_);
    if (text.is_error()) {
        // A missing file is a normal first run: defaults, no diagnostic. Any
        // other read failure is reported, because the user needs to know that
        // their settings were not read.
        settings_ = domain::AppSettings::defaults();
        SettingsLoadReport report;
        report.used_defaults = true;
        if (text.code() != ErrorCode::not_found) {
            report.diagnostics.push_back(domain::SettingsDiagnostic{
                domain::SettingsDiagnosticKind::error, "settings", text.error().message()});
        }
        dirty_ = false;
        dirty_generation_ = 0;
        pending_ = SettingsChange::none;
        last_change_ = clock_.now();
        return report;
    }

    const auto result = domain::SettingsCodec::load(text.value());
    settings_ = result.settings;
    dirty_ = false;
    dirty_generation_ = 0;
    pending_ = SettingsChange::none;
    last_change_ = clock_.now();

    SettingsLoadReport report;
    report.used_defaults = result.used_defaults;
    report.diagnostics = result.diagnostics;
    return report;
}

std::chrono::milliseconds SettingsPresenter::since_last_change() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_.elapsed_since(last_change_));
}

bool SettingsPresenter::debounce_elapsed() const
{
    return dirty_ && since_last_change() >= kAutosaveDebounce;
}

domain::Status SettingsPresenter::flush()
{
    if (!dirty_) {
        return domain::Status::success();
    }
    const auto text = domain::SettingsCodec::serialize(settings_);
    const auto written = file_system_.atomic_write(settings_path_, text);
    if (written.is_error()) {
        // The old file is untouched; the UI keeps the dirty flag so a later
        // attempt (or quit) can try again.
        return written;
    }
    dirty_ = false;
    pending_ = SettingsChange::none;
    last_change_ = clock_.now();
    return domain::Status::success();
}

} // namespace voicetyper::app
