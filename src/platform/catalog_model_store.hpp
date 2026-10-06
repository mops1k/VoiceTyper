#pragma once

// Concrete ModelStore over the injected platform seams. Pure control flow: no
// OS call of its own, no static mutable state, no network of its own.

#include "platform/api/clock.hpp"
#include "platform/api/file_system.hpp"
#include "platform/api/http.hpp"
#include "platform/api/model_store.hpp"

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace voicetyper::platform {

struct ModelStoreOptions {
    std::chrono::seconds timeout{kModelDownloadTimeoutMinutes * 60};
    int progress_interval_ms{kModelProgressIntervalMs};
    std::size_t max_attempts{3};
    std::chrono::milliseconds retry_backoff{1000};
    /// A pre-existing file counts as installed only when its size matches the
    /// catalog. The .NET build accepted File.Exists alone, which silently trusts
    /// a truncated download.
    bool validate_existing_by_size{true};
    /// A body shorter than its Content-Length is corrupt_data, not success.
    bool enforce_downloaded_size{true};
    /// Resume of a partial download is designed but not shipped yet: a stale
    /// temp is always deleted, which removes the append-corruption trap.
    bool resume_partial{false};
    bool cleanup_stale_temps_on_enter{true};
};

class CatalogModelStore final : public ModelStore {
public:
    CatalogModelStore(
        std::filesystem::path models_dir,
        FileSystem& file_system,
        HttpClient& http,
        Clock& clock,
        ModelStoreOptions options = {});

    [[nodiscard]] std::filesystem::path models_directory() const override;
    [[nodiscard]] std::filesystem::path path_for(const ModelDescriptor& descriptor) const override;
    [[nodiscard]] bool is_downloaded(const ModelDescriptor& descriptor) const override;
    [[nodiscard]] Result<std::filesystem::path> ensure(
        const ModelDescriptor& descriptor,
        const ModelProgressSink& progress,
        const CancellationToken& cancellation) override;
    Status remove(const ModelDescriptor& descriptor) override;
    Status cleanup_legacy() override;

    /// Removes only "<catalog file name>.download" entries, so a user's own
    /// "*.download" file is never touched.
    Status cleanup_stale_downloads();

    [[nodiscard]] std::uint32_t stale_cleanup_count() const noexcept;

private:
    [[nodiscard]] std::filesystem::path temp_path_for(const ModelDescriptor& descriptor) const;
    [[nodiscard]] Status validate_descriptor(const ModelDescriptor& descriptor) const;
    [[nodiscard]] bool is_valid_file(const ModelDescriptor& descriptor) const;
    [[nodiscard]] Status download_once(
        const ModelDescriptor& descriptor,
        const ModelProgressSink& progress,
        const CancellationToken& cancellation);
    [[nodiscard]] std::mutex& path_mutex(const std::filesystem::path& path);

    std::filesystem::path models_dir_;
    FileSystem& file_system_;
    HttpClient& http_;
    Clock& clock_;
    ModelStoreOptions options_;
    std::mutex registry_mutex_;
    std::map<std::string, std::unique_ptr<std::mutex>> path_mutexes_;
    std::atomic<std::uint32_t> stale_cleanup_count_{0};
};

/// True when `name` is one of the ten frozen legacy fp16/q5 names.
[[nodiscard]] bool is_legacy_model_file_name(std::string_view name);
/// "<name>.download".
[[nodiscard]] std::string temp_file_name(std::string_view name);
/// True when `name` equals a catalog entry name plus ".download".
[[nodiscard]] bool is_catalog_temp_file_name(std::string_view name);

} // namespace voicetyper::platform
