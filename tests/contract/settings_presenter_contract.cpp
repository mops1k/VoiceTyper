#include "app/settings_presenter.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

using namespace voicetyper;
using namespace voicetyper::app;

/// Virtual clock so the 700 ms debounce is asserted exactly and instantly.
class FakeClock final : public platform::Clock {
public:
    std::chrono::steady_clock::time_point now() const override { return now_value; }
    std::chrono::system_clock::time_point wall_now() const override { return {}; }
    std::chrono::steady_clock::duration elapsed_since(std::chrono::steady_clock::time_point start) const override
    {
        return now_value - start;
    }
    platform::Status sleep_for(std::chrono::milliseconds, const domain::CancellationToken&) override
    {
        return platform::Status::success();
    }
    platform::Status sleep_until(std::chrono::steady_clock::time_point, const domain::CancellationToken&) override
    {
        return platform::Status::success();
    }
    void advance(std::chrono::milliseconds delta) { now_value += delta; }

    std::chrono::steady_clock::time_point now_value{};
};

class MemoryFileSystem final : public platform::FileSystem {
public:
    bool exists(const std::filesystem::path& path) const override { return files.count(path.string()) != 0; }
    domain::Result<std::uint64_t> file_size(const std::filesystem::path& path) const override
    {
        const auto it = files.find(path.string());
        if (it == files.end()) {
            return domain::Result<std::uint64_t>::failure(domain::ErrorCode::not_found, "missing");
        }
        return static_cast<std::uint64_t>(it->second.size());
    }
    platform::Status create_directories(const std::filesystem::path&) override
    {
        ++mkdirs;
        return platform::Status::success();
    }
    platform::Result<std::unique_ptr<platform::FileWriteStream>> open_write(
        const std::filesystem::path&, platform::FileOpenMode) override
    {
        return domain::Result<std::unique_ptr<platform::FileWriteStream>>::failure(
            domain::ErrorCode::unsupported, "not used here");
    }
    domain::Result<std::string> read_text(const std::filesystem::path& path) const override
    {
        const auto it = files.find(path.string());
        if (it == files.end()) {
            return domain::Result<std::string>::failure(domain::ErrorCode::not_found, "missing");
        }
        return it->second;
    }
    domain::Result<std::vector<std::uint8_t>> read_binary(const std::filesystem::path&) const override
    {
        return std::vector<std::uint8_t>();
    }
    platform::Status atomic_write(const std::filesystem::path& path, std::string_view content, platform::FileWriteMode) override
    {
        ++writes;
        if (fail_write) {
            return platform::Status::failure(domain::ErrorCode::io_failure, "disk full");
        }
        files[path.string()] = std::string(content);
        return platform::Status::success();
    }
    platform::Status replace_file(const std::filesystem::path& from, const std::filesystem::path& to) override
    {
        files[to.string()] = files[from.string()];
        files.erase(from.string());
        return platform::Status::success();
    }
    platform::Status remove_file(const std::filesystem::path& path) override
    {
        files.erase(path.string());
        return platform::Status::success();
    }
    domain::Result<std::vector<std::filesystem::path>> list_directory(const std::filesystem::path&) const override
    {
        return std::vector<std::filesystem::path>();
    }
    domain::Result<std::uint64_t> available_space(const std::filesystem::path&) const override
    {
        return std::uint64_t{1} << 40;
    }

    std::map<std::string, std::string> files;
    bool fail_write = false;
    int writes = 0;
    int mkdirs = 0;
};

/// A small but schema-correct settings.json: the presentation fields the test
/// edits plus the hotkeys it restores. Field names must match AppSettings
/// exactly, because unknown fields are ignored and a missing required enum
/// would fall back to defaults.
const char* kGolden =
    "{\r\n"
    "  \"recordingMode\": \"toggle\",\r\n"
    "  \"recordHotkey\": \"Ctrl+Shift+R\",\r\n"
    "  \"cancelHotkey\": \"Ctrl+Shift+Q\",\r\n"
    "  \"language\": \"ru\",\r\n"
    "  \"appLanguage\": \"en\",\r\n"
    "  \"temperature\": 0.25,\r\n"
    "  \"silenceThresholdMs\": 1200\r\n"
    "}\r\n";

void check_first_run_uses_defaults()
{
    MemoryFileSystem fs;
    FakeClock clock;
    SettingsPresenter presenter("/app/settings.json", fs, clock);
    const auto report = presenter.load();
    check(report.used_defaults, "a missing file is a first run, not an error");
    check(report.diagnostics.empty(), "a first run reports no diagnostics");
    check(!presenter.dirty(), "a first run leaves nothing to save");
    check(presenter.settings().app_language == domain::AppLanguage::ru, "the default language is ru");
}

void check_loads_existing_and_stays_clean()
{
    MemoryFileSystem fs;
    FakeClock clock;
    fs.files["/app/settings.json"] = kGolden;
    SettingsPresenter presenter("/app/settings.json", fs, clock);
    const auto report = presenter.load();
    check(!report.used_defaults, "an existing file is read");
    check(presenter.settings().record_hotkey == "Ctrl+Shift+R", "the hotkey is restored");
    check(presenter.settings().temperature == 0.25, "temperature is restored");
    check(presenter.flush().is_ok() && fs.writes == 0, "loading alone never rewrites the file");
}

void check_corrupt_file_reports_but_loads_defaults()
{
    MemoryFileSystem fs;
    FakeClock clock;
    fs.files["/app/settings.json"] = "{ this is not json";
    SettingsPresenter presenter("/app/settings.json", fs, clock);
    const auto report = presenter.load();
    check(report.used_defaults, "a corrupt file falls back to defaults");
    check(!report.diagnostics.empty(), "and says so");
}

void check_debounce_and_atomic_save()
{
    MemoryFileSystem fs;
    FakeClock clock;
    SettingsPresenter presenter("/app/settings.json", fs, clock);
    static_cast<void>(presenter.load());

    presenter.update(SettingsChange::language, [](domain::AppSettings& settings) {
        settings.app_language = domain::AppLanguage::en;
    });
    check(presenter.dirty(), "an edit marks the presenter dirty");
    check(!presenter.debounce_elapsed(), "the 700 ms debounce has not elapsed yet");

    clock.advance(std::chrono::milliseconds(699));
    check(!presenter.debounce_elapsed(), "699 ms is still inside the debounce");
    clock.advance(std::chrono::milliseconds(2));
    check(presenter.debounce_elapsed(), "701 ms crosses the debounce");

    const auto saved = presenter.flush();
    check(saved.is_ok() && fs.writes == 1, "flush writes exactly once");
    check(!presenter.dirty(), "a successful flush clears the dirty flag");
    const auto reread = domain::SettingsCodec::load(fs.files.at("/app/settings.json"));
    check(reread.settings.app_language == domain::AppLanguage::en, "the change is on disk");
    check(presenter.flush().is_ok() && fs.writes == 1, "a clean presenter does not rewrite");

    // A failed save must keep the dirty flag so quit can retry, and must not
    // destroy the previous file.
    presenter.update(SettingsChange::model, [](domain::AppSettings& settings) {
        settings.model_size = domain::ModelSize::medium;
    });
    fs.fail_write = true;
    check(presenter.flush().is_error(), "a failed save is reported");
    check(presenter.dirty(), "and the presenter stays dirty for a retry");
    const auto preserved = domain::SettingsCodec::load(fs.files.at("/app/settings.json"));
    check(preserved.settings.app_language == domain::AppLanguage::en, "the previous file is intact");
    fs.fail_write = false;
    check(presenter.flush().is_ok() && !presenter.dirty(), "a retry succeeds and clears the flag");
}

void check_repeated_edits_restart_the_debounce()
{
    MemoryFileSystem fs;
    FakeClock clock;
    SettingsPresenter presenter("/app/settings.json", fs, clock);
    static_cast<void>(presenter.load());

    presenter.update(SettingsChange::engine, [](domain::AppSettings&) {});
    clock.advance(std::chrono::milliseconds(500));
    presenter.update(SettingsChange::engine, [](domain::AppSettings&) {});
    clock.advance(std::chrono::milliseconds(500));
    check(!presenter.debounce_elapsed(),
        "a second edit restarts the debounce, so rapid edits save once");
    clock.advance(std::chrono::milliseconds(250));
    check(presenter.debounce_elapsed(), "the debounce fires 700 ms after the last edit");
}

} // namespace

int main()
{
    check_first_run_uses_defaults();
    check_loads_existing_and_stays_clean();
    check_corrupt_file_reports_but_loads_defaults();
    check_debounce_and_atomic_save();
    check_repeated_edits_restart_the_debounce();

    if (failures != 0) {
        std::cerr << "settings-presenter-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "settings-presenter-contract: OK\n";
    return 0;
}
