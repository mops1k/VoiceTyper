#include "platform/catalog_model_store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>

namespace voicetyper::platform {

namespace {

Status invalid_descriptor()
{
    return Status::failure(
        ErrorCode::invalid_argument,
        "model descriptor is not part of the frozen catalog");
}

std::string_view base_url_for(ModelKind kind)
{
    switch (kind) {
    case ModelKind::whisper: return kWhisperModelBaseUrl;
    case ModelKind::vad: return kVadModelBaseUrl;
    case ModelKind::parakeet: return kParakeetModelBaseUrl;
    }
    return {};
}

bool is_catalog_entry(const ModelDescriptor& descriptor)
{
    for (const auto& entry : whisper_catalog()) {
        if (entry.file_name == descriptor.file_name) {
            return true;
        }
    }
    for (const auto& entry : vad_catalog()) {
        if (entry.file_name == descriptor.file_name) {
            return true;
        }
    }
    for (const auto& entry : parakeet_catalog()) {
        if (entry.file_name == descriptor.file_name) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string temp_file_name(std::string_view name)
{
    return std::string(name) + std::string(kModelDownloadSuffix);
}

bool is_catalog_temp_file_name(std::string_view name)
{
    for (const auto& entry : whisper_catalog()) {
        if (temp_file_name(entry.file_name) == name) {
            return true;
        }
    }
    for (const auto& entry : vad_catalog()) {
        if (temp_file_name(entry.file_name) == name) {
            return true;
        }
    }
    for (const auto& entry : parakeet_catalog()) {
        if (temp_file_name(entry.file_name) == name) {
            return true;
        }
    }
    return false;
}

bool is_legacy_model_file_name(std::string_view name)
{
    const auto& names = legacy_model_file_names();
    return std::find(names.begin(), names.end(), name) != names.end();
}

CatalogModelStore::CatalogModelStore(
    std::filesystem::path models_dir,
    FileSystem& file_system,
    HttpClient& http,
    Clock& clock,
    ModelStoreOptions options)
    : models_dir_(std::move(models_dir))
    , file_system_(file_system)
    , http_(http)
    , clock_(clock)
    , options_(options)
{
    options_.max_attempts = std::max<std::size_t>(1, options_.max_attempts);
}

std::filesystem::path CatalogModelStore::models_directory() const
{
    return models_dir_;
}

std::filesystem::path CatalogModelStore::path_for(const ModelDescriptor& descriptor) const
{
    return models_dir_ / std::filesystem::path(descriptor.file_name);
}

std::filesystem::path CatalogModelStore::temp_path_for(const ModelDescriptor& descriptor) const
{
    return models_dir_ / std::filesystem::path(temp_file_name(descriptor.file_name));
}

bool CatalogModelStore::is_valid_file(const ModelDescriptor& descriptor) const
{
    const auto path = path_for(descriptor);
    if (!file_system_.exists(path)) {
        return false;
    }
    if (!options_.validate_existing_by_size) {
        return true;
    }
    const auto size = file_system_.file_size(path);
    return size.is_ok() && size.value() == descriptor.expected_bytes;
}

bool CatalogModelStore::is_downloaded(const ModelDescriptor& descriptor) const
{
    return is_valid_file(descriptor);
}

Status CatalogModelStore::validate_descriptor(const ModelDescriptor& descriptor) const
{
    if (!is_catalog_entry(descriptor)) {
        return invalid_descriptor();
    }
    if (descriptor.file_name.find('/') != std::string::npos
        || descriptor.file_name.find('\\') != std::string::npos
        || descriptor.file_name.find("..") != std::string::npos
        || std::filesystem::path(descriptor.file_name).is_absolute()) {
        return invalid_descriptor();
    }
    const auto expected_url = std::string(base_url_for(descriptor.kind)) + descriptor.file_name;
    if (descriptor.download_url != expected_url) {
        return invalid_descriptor();
    }
    return Status::success();
}

std::mutex& CatalogModelStore::path_mutex(const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto& entry = path_mutexes_[path.string()];
    if (!entry) {
        entry = std::make_unique<std::mutex>();
    }
    return *entry;
}

Result<std::filesystem::path> CatalogModelStore::ensure(
    const ModelDescriptor& descriptor,
    const ModelProgressSink& progress,
    const CancellationToken& cancellation)
{
    const auto preflight = validate_descriptor(descriptor);
    if (preflight.is_error()) {
        return Result<std::filesystem::path>::failure(preflight.code(), preflight.message());
    }
    const auto cancelled = domain::check_cancelled(cancellation);
    if (cancelled.is_error()) {
        return Result<std::filesystem::path>::failure(cancelled.code(), cancelled.message());
    }

    const auto directory = file_system_.create_directories(models_dir_);
    if (directory.is_error()) {
        return Result<std::filesystem::path>::failure(directory.code(), directory.message());
    }
    if (is_valid_file(descriptor)) {
        return path_for(descriptor);
    }

    // Single-flight: a second caller for the same model waits and then finds the
    // file already present, so two threads never download the same weights.
    auto& guard = path_mutex(path_for(descriptor));
    std::lock_guard<std::mutex> session(guard);
    if (is_valid_file(descriptor)) {
        return path_for(descriptor);
    }
    if (options_.cleanup_stale_temps_on_enter) {
        static_cast<void>(cleanup_stale_downloads());
    }

    auto last_code = ErrorCode::unavailable;
    std::string last_message = "model download did not start";
    for (std::size_t attempt = 1; attempt <= options_.max_attempts; ++attempt) {
        const auto status = download_once(descriptor, progress, cancellation);
        if (status.is_ok()) {
            return path_for(descriptor);
        }
        last_code = status.code();
        last_message = status.message();
        const auto retryable = last_code == ErrorCode::unavailable
            || last_code == ErrorCode::io_failure
            || last_code == ErrorCode::timeout;
        if (!retryable || attempt == options_.max_attempts) {
            break;
        }
        static_cast<void>(file_system_.remove_file(temp_path_for(descriptor)));
        const auto backoff = options_.retry_backoff * static_cast<int>(1ULL << (attempt - 1));
        const auto slept = clock_.sleep_for(backoff, cancellation);
        if (slept.is_error()) {
            return Result<std::filesystem::path>::failure(slept.code(), slept.message());
        }
    }
    static_cast<void>(file_system_.remove_file(temp_path_for(descriptor)));
    return Result<std::filesystem::path>::failure(last_code, std::move(last_message));
}

Status CatalogModelStore::download_once(
    const ModelDescriptor& descriptor,
    const ModelProgressSink& progress,
    const CancellationToken& cancellation)
{
    HttpRequest request;
    request.url = descriptor.download_url;
    request.headers.push_back(HttpHeader{"User-Agent", std::string(kHttpUserAgent)});
    request.timeout = options_.timeout;

    auto opened = http_.open(request, cancellation);
    if (opened.is_error()) {
        return opened.status();
    }
    auto& stream = *opened.value();
    struct StreamGuard {
        HttpByteStream& stream;
        ~StreamGuard() { stream.close(); }
    } guard{stream};

    // Status mapping. Only genuinely transient statuses are retried by ensure():
    // a 404 must not be retried three times, and a 403 is a permission problem
    // the user has to see immediately.
    const auto status_code = stream.status_code();
    if (status_code < 200 || status_code >= 300) {
        const auto detail = "model download returned HTTP " + std::to_string(status_code);
        if (status_code == 404) {
            return Status::failure(ErrorCode::not_found, detail);
        }
        if (status_code == 401 || status_code == 403) {
            return Status::failure(ErrorCode::permission_denied, detail);
        }
        if (status_code >= 500 || status_code == 408 || status_code == 429) {
            return Status::failure(ErrorCode::unavailable, detail);
        }
        return Status::failure(ErrorCode::unsupported, detail);
    }

    const auto reported_length = stream.content_length();
    const auto total_bytes = reported_length.value_or(0) > 0
        ? reported_length.value()
        : descriptor.expected_bytes;

    const auto temp = temp_path_for(descriptor);
    auto writer = file_system_.open_write(temp, FileOpenMode::truncate);
    if (writer.is_error()) {
        return writer.status();
    }
    struct WriterGuard {
        FileWriteStream& writer;
        bool committed = false;
        ~WriterGuard()
        {
            if (!committed) {
                writer.close();
            }
        }
    } writer_guard{*writer.value()};

    std::uint64_t written = 0;
    const auto start = clock_.now();
    auto last_report = start;
    const auto report = [&] {
        if (!progress) {
            return;
        }
        const auto elapsed = std::chrono::duration<double>(clock_.elapsed_since(start)).count();
        ModelDownloadProgress update;
        update.bytes_downloaded = written;
        update.total_bytes = total_bytes;
        update.bytes_per_second = elapsed > 0.0 ? static_cast<double>(written) / elapsed : 0.0;
        progress(update);
    };
    const auto read_status = stream.read_into(
        [&](const char* data, std::size_t size) {
            if (cancellation.is_cancellation_requested()) {
                return false;
            }
            const auto written_status = writer.value()->write(data, size);
            if (written_status.is_error()) {
                return false;
            }
            written += size;
            const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(clock_.elapsed_since(last_report));
            if (since_last.count() > options_.progress_interval_ms) {
                last_report = clock_.now();
                report();
            }
            return true;
        },
        cancellation);
    report();

    if (read_status.is_error()) {
        return read_status;
    }
    if (written == 0) {
        return Status::failure(ErrorCode::corrupt_data, "model download returned an empty body");
    }
    if (options_.enforce_downloaded_size && total_bytes > 0 && written != total_bytes) {
        return Status::failure(
            ErrorCode::corrupt_data,
            "short model body: got " + std::to_string(written) + " of " + std::to_string(total_bytes));
    }
    const auto flushed = writer.value()->flush();
    if (flushed.is_error()) {
        return flushed;
    }
    writer.value()->close();
    writer_guard.committed = true;
    return file_system_.replace_file(temp, path_for(descriptor));
}

Status CatalogModelStore::remove(const ModelDescriptor& descriptor)
{
    const auto preflight = validate_descriptor(descriptor);
    if (preflight.is_error()) {
        return preflight;
    }
    auto& guard = path_mutex(path_for(descriptor));
    std::lock_guard<std::mutex> session(guard);
    return file_system_.remove_file(path_for(descriptor));
}

Status CatalogModelStore::cleanup_legacy()
{
    const auto directory = file_system_.create_directories(models_dir_);
    if (directory.is_error()) {
        return directory;
    }
    std::size_t failed = 0;
    for (const auto& name : legacy_model_file_names()) {
        const auto status = file_system_.remove_file(models_dir_ / std::filesystem::path(name));
        if (status.is_error() && status.code() != ErrorCode::not_found) {
            ++failed;
        }
    }
    if (failed != 0) {
        return Status::failure(
            ErrorCode::io_failure,
            "legacy cleanup: " + std::to_string(failed) + " of "
                + std::to_string(legacy_model_file_names().size()) + " file(s) could not be removed");
    }
    return Status::success();
}

Status CatalogModelStore::cleanup_stale_downloads()
{
    const auto entries = file_system_.list_directory(models_dir_);
    if (entries.is_error()) {
        return entries.status();
    }
    std::uint32_t removed = 0;
    for (const auto& entry : entries.value()) {
        const auto name = entry.filename().string();
        if (!is_catalog_temp_file_name(name)) {
            continue;
        }
        const auto status = file_system_.remove_file(entry);
        if (status.is_ok()) {
            ++removed;
        }
    }
    stale_cleanup_count_.fetch_add(removed, std::memory_order_relaxed);
    return Status::success();
}

std::uint32_t CatalogModelStore::stale_cleanup_count() const noexcept
{
    return stale_cleanup_count_.load(std::memory_order_relaxed);
}

} // namespace voicetyper::platform
