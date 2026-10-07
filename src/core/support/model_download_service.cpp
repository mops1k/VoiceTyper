#include "core/support/model_download_service.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace voicetyper::core::support {

namespace {

/// The repositories the .NET catalog used.
constexpr std::string_view kWhisperBaseUrl =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";
constexpr std::string_view kParakeetBaseUrl =
    "https://huggingface.co/mudler/parakeet-cpp-gguf/resolve/main/";

/// The models are public artifacts on a file host: no credentials, and the user agent keeps
/// the requests identifiable, exactly like the update service does.
constexpr std::string_view kModelUserAgent = "VoiceTyper/1.0";

} // namespace

double ModelDownloadProgress::fraction() const noexcept
{
    if (total == 0) {
        return 0.0;
    }
    return std::min(1.0, static_cast<double>(downloaded) / static_cast<double>(total));
}

std::optional<double> ModelDownloadProgress::remaining_seconds() const noexcept
{
    if (total == 0 || downloaded >= total || bytes_per_second <= 0.0) {
        return std::nullopt;
    }
    return static_cast<double>(total - downloaded) / bytes_per_second;
}

std::string_view whisper_model_file_name(domain::ModelSize size)
{
    switch (size) {
    case domain::ModelSize::tiny:
        return "ggml-tiny-q8_0.bin";
    case domain::ModelSize::base:
        return "ggml-base-q8_0.bin";
    case domain::ModelSize::small:
        return "ggml-small-q8_0.bin";
    case domain::ModelSize::medium:
        return "ggml-medium-q8_0.bin";
    case domain::ModelSize::large:
        return "ggml-large-v3-turbo-q8_0.bin";
    }
    return "ggml-small-q8_0.bin";
}

std::string_view parakeet_model_file_name(domain::ParakeetModelSize size)
{
    switch (size) {
    case domain::ParakeetModelSize::q4k:
        return "tdt-0.6b-v3-q4_k.gguf";
    case domain::ParakeetModelSize::q5k:
        return "tdt-0.6b-v3-q5_k.gguf";
    case domain::ParakeetModelSize::q6k:
        return "tdt-0.6b-v3-q6_k.gguf";
    case domain::ParakeetModelSize::q8_0:
        return "tdt-0.6b-v3-q8_0.gguf";
    }
    return "tdt-0.6b-v3-q8_0.gguf";
}

std::string model_download_url(ModelEngine engine, std::string_view file_name)
{
    const std::string_view base =
        engine == ModelEngine::whisper ? kWhisperBaseUrl : kParakeetBaseUrl;
    std::string url(base);
    url.append(file_name);
    return url;
}

ModelDownloadService::ModelDownloadService(platform::HttpClient& http,
    std::filesystem::path models_directory)
    : http_(http)
    , models_directory_(std::move(models_directory))
{
}

std::filesystem::path ModelDownloadService::path_for(std::string_view file_name) const
{
    return models_directory_ / std::filesystem::path(std::string(file_name));
}

platform::Status ModelDownloadService::download(ModelEngine engine, std::string_view file_name,
    const std::function<void(const ModelDownloadProgress&)>& progress,
    const domain::CancellationToken& cancellation)
{
    if (file_name.empty()) {
        return platform::Status::failure(platform::ErrorCode::invalid_argument,
            "the model file name is empty");
    }

    std::error_code error;
    std::filesystem::create_directories(models_directory_, error);
    const auto target = path_for(file_name);
    const auto temporary = path_for(std::string(file_name) + ".part");
    std::filesystem::remove(temporary, error);
    std::filesystem::remove(target, error);

    platform::HttpRequest request;
    request.url = model_download_url(engine, file_name);
    request.headers.push_back({std::string("User-Agent"), std::string(kModelUserAgent)});
    request.timeout = kModelDownloadTimeout;

    auto opened = http_.open(request, cancellation);
    if (opened.is_error()) {
        return platform::Status::failure(opened.error());
    }
    auto stream = std::move(opened.value());
    if (stream == nullptr) {
        return platform::Status::failure(platform::ErrorCode::unavailable,
            "the model host returned no stream");
    }
    if (stream->status_code() / 100 != 2) {
        const int code = stream->status_code();
        stream->close();
        return platform::Status::failure(platform::ErrorCode::unavailable,
            "the model host answered with status " + std::to_string(code));
    }

    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
        stream->close();
        return platform::Status::failure(platform::ErrorCode::io_failure,
            "the model file cannot be written: " + temporary.string());
    }

    ModelDownloadProgress state;
    state.total = stream->content_length().value_or(0);
    const auto started = std::chrono::steady_clock::now();
    const auto sink = [&](const char* data, std::size_t size) {
        if (cancellation.is_cancellation_requested()) {
            return false;
        }
        file.write(data, static_cast<std::streamsize>(size));
        if (!file) {
            return false;
        }
        state.downloaded += size;
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        state.bytes_per_second =
            elapsed > 0.0 ? static_cast<double>(state.downloaded) / elapsed : 0.0;
        if (progress) {
            progress(state);
        }
        return true;
    };

    const platform::Status read_status = stream->read_into(sink, cancellation);
    stream->close();
    file.close();

    if (read_status.is_error()) {
        std::filesystem::remove(temporary, error);
        return platform::Status::failure(read_status.error());
    }
    if (state.total > 0 && state.downloaded != state.total) {
        std::filesystem::remove(temporary, error);
        return platform::Status::failure(platform::ErrorCode::io_failure,
            "the download stopped before the whole file arrived");
    }

    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return platform::Status::failure(platform::ErrorCode::io_failure,
            "the model file cannot be moved into place: " + target.string());
    }
    return platform::Status::success();
}

} // namespace voicetyper::core::support
