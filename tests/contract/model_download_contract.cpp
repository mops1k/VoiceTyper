// Contract for the model downloader.
//
// The .NET build downloaded the model catalog from two Hugging Face repositories
// (ggerganov/whisper.cpp and mudler/parakeet-cpp-gguf) into the models directory, reporting
// progress as a fraction and an estimate of the remaining time
// (VoiceTyper.Core/Models/ModelDownloadProgress.cs). This contract pins the addresses, the
// file names the catalog uses, that arithmetic, and one property the port makes stricter: a
// broken transfer must leave nothing behind - a half-downloaded .bin would otherwise look
// like a ready model and fail later, inside the recogniser.

#include "core/support/model_download_service.hpp"
#include "platform/api/http.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what)
{
    ++checks;
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
        return;
    }
    ++failures;
    std::printf("  FAIL %s\n", what.c_str());
}

void check_equal(const std::string& actual, const std::string& expected, const std::string& what)
{
    check(actual == expected, what + " (expected \"" + expected + "\", got \"" + actual + "\")");
}

/// A body that yields the canned chunks, or fails in the middle of them.
class FakeStream final : public voicetyper::platform::HttpByteStream {
public:
    FakeStream(int status, std::optional<std::uint64_t> length, std::vector<std::string> chunks,
        bool fail_after_chunks = false, voicetyper::domain::CancellationSource* cancel_after_first = nullptr)
        : status_(status)
        , length_(length)
        , chunks_(std::move(chunks))
        , fail_after_chunks_(fail_after_chunks)
        , cancel_after_first_(cancel_after_first)
    {
    }

    [[nodiscard]] int status_code() const noexcept override { return status_; }
    [[nodiscard]] const std::vector<voicetyper::platform::HttpHeader>& headers() const noexcept override
    {
        return headers_;
    }
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept override { return length_; }

    voicetyper::domain::Status read_into(
        const voicetyper::platform::HttpChunkSink& sink,
        const voicetyper::domain::CancellationToken& cancellation) override
    {
        std::size_t sent = 0;
        for (const std::string& chunk : chunks_) {
            if (cancel_after_first_ != nullptr && sent == 1) {
                cancel_after_first_->request_cancellation(); // the user changed their mind
            }
            if (cancellation.can_be_cancelled() && cancellation.is_cancellation_requested()) {
                return voicetyper::domain::Status::failure(
                    voicetyper::domain::ErrorCode::cancelled, "the download was cancelled");
            }
            ++sent;
            if (!sink(chunk.data(), chunk.size())) {
                return voicetyper::domain::Status::failure(
                    voicetyper::domain::ErrorCode::io_failure, "the sink aborted the transfer");
            }
        }
        if (fail_after_chunks_) {
            return voicetyper::domain::Status::failure(
                voicetyper::domain::ErrorCode::unavailable, "the connection dropped");
        }
        return voicetyper::domain::Status::success();
    }

    void close() override { closed_ = true; }

private:
    int status_ = 200;
    std::optional<std::uint64_t> length_;
    std::vector<std::string> chunks_;
    bool fail_after_chunks_ = false;
    voicetyper::domain::CancellationSource* cancel_after_first_ = nullptr;
    std::vector<voicetyper::platform::HttpHeader> headers_;
    bool closed_ = false;
};

class FakeHttp final : public voicetyper::platform::HttpClient {
public:
    int status = 200;
    std::optional<std::uint64_t> length;
    std::vector<std::string> chunks;
    bool fails = false;
    std::string last_url;

    [[nodiscard]] voicetyper::domain::Result<voicetyper::platform::HttpResponse> get(
        const voicetyper::platform::HttpRequest&, const voicetyper::domain::CancellationToken&) override
    {
        return voicetyper::domain::Result<voicetyper::platform::HttpResponse>::failure(
            voicetyper::domain::ErrorCode::unavailable, "this seam only streams");
    }

    [[nodiscard]] voicetyper::domain::Result<std::unique_ptr<voicetyper::platform::HttpByteStream>> open(
        const voicetyper::platform::HttpRequest& request,
        const voicetyper::domain::CancellationToken&) override
    {
        last_url = request.url;
        if (fails) {
            return voicetyper::domain::Result<std::unique_ptr<voicetyper::platform::HttpByteStream>>::failure(
                voicetyper::domain::ErrorCode::unavailable, "the model host is unreachable");
        }
        return voicetyper::domain::Result<std::unique_ptr<voicetyper::platform::HttpByteStream>>(
            std::make_unique<FakeStream>(status, length, chunks, dropped, cancel_after_first));
    }

    bool dropped = false;
    voicetyper::domain::CancellationSource* cancel_after_first = nullptr;
};

void the_urls_are_the_dotnet_repositories()
{
    using voicetyper::core::support::ModelEngine;
    using voicetyper::core::support::model_download_url;
    check_equal(model_download_url(ModelEngine::whisper, "ggml-tiny-q8_0.bin"),
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny-q8_0.bin",
        "адрес модели Whisper");
    check_equal(model_download_url(ModelEngine::parakeet, "tdt-0.6b-v3-q8_0.gguf"),
        "https://huggingface.co/mudler/parakeet-cpp-gguf/resolve/main/tdt-0.6b-v3-q8_0.gguf",
        "адрес модели Parakeet");
}

void the_file_names_are_the_catalog_ones()
{
    using voicetyper::core::support::parakeet_model_file_name;
    using voicetyper::core::support::whisper_model_file_name;
    check_equal(std::string(whisper_model_file_name(voicetyper::domain::ModelSize::tiny)),
        "ggml-tiny-q8_0.bin", "имя файла tiny");
    check_equal(std::string(whisper_model_file_name(voicetyper::domain::ModelSize::large)),
        "ggml-large-v3-turbo-q8_0.bin", "имя файла large turbo");
    check_equal(std::string(parakeet_model_file_name(voicetyper::domain::ParakeetModelSize::q4k)),
        "tdt-0.6b-v3-q4_k.gguf", "имя файла parakeet q4_k");
    check_equal(std::string(parakeet_model_file_name(voicetyper::domain::ParakeetModelSize::q8_0)),
        "tdt-0.6b-v3-q8_0.gguf", "имя файла parakeet q8_0");
}

void progress_reports_the_fraction_and_the_remaining_time()
{
    using voicetyper::core::support::ModelDownloadProgress;
    ModelDownloadProgress half;
    half.downloaded = 500;
    half.total = 1000;
    half.bytes_per_second = 100.0;
    check(std::abs(half.fraction() - 0.5) < 1e-9, "доля загрузки 0.5");
    check(half.remaining_seconds().has_value()
            && std::abs(*half.remaining_seconds() - 5.0) < 1e-9,
        "остаток времени 5 с");

    ModelDownloadProgress unknown;
    unknown.downloaded = 500;
    check(unknown.fraction() == 0.0, "без известной длины доля равна нулю");
    check(!unknown.remaining_seconds().has_value(), "без длины остаток неизвестен");

    ModelDownloadProgress done;
    done.downloaded = 1000;
    done.total = 1000;
    done.bytes_per_second = 10.0;
    check(done.fraction() == 1.0, "загруженное целиком даёт долю 1.0");
    check(!done.remaining_seconds().has_value(), "загруженное целиком не имеет остатка");
}

std::filesystem::path scratch_directory()
{
    const auto path = std::filesystem::temp_directory_path() / "voicetyper-model-download-test";
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void a_successful_download_writes_the_file()
{
    using voicetyper::core::support::ModelDownloadService;
    using voicetyper::core::support::ModelEngine;
    const auto directory = scratch_directory();
    FakeHttp http;
    http.length = 11; // "hello" + "world!"
    http.chunks = {"hello", "world!"};

    ModelDownloadService service(http, directory);
    std::vector<std::uint64_t> seen;
    const auto status = service.download(ModelEngine::whisper, "ggml-tiny-q8_0.bin",
        [&seen](const voicetyper::core::support::ModelDownloadProgress& progress) {
            seen.push_back(progress.downloaded);
        },
        voicetyper::domain::CancellationToken{});

    check(status.is_ok(), std::string("успешная загрузка сообщает об успехе")
        + (status.is_ok() ? std::string() : " [" + status.error().to_string() + "]"));
    check(http.last_url.find("ggerganov/whisper.cpp") != std::string::npos,
        "файл запрошен у нужного репозитория");
    const auto target = directory / "ggml-tiny-q8_0.bin";
    check(std::filesystem::exists(target), "файл появился в папке моделей");
    std::ifstream file(target, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    const auto size = std::filesystem::exists(target)
        ? std::filesystem::file_size(target)
        : static_cast<std::uintmax_t>(0);
    check(content == "helloworld!",
        "содержимое совпадает с полученным (размер " + std::to_string(size)
            + ", прочитано " + std::to_string(content.size()) + ")");
    check(!std::filesystem::exists(directory / "ggml-tiny-q8_0.bin.part"),
        "временный файл убран после успеха");
    check(seen.size() >= 2 && seen.front() < seen.back(), "прогресс растёт по ходу загрузки");
    std::filesystem::remove_all(directory);
}

void a_broken_transfer_leaves_nothing_behind()
{
    using voicetyper::core::support::ModelDownloadService;
    using voicetyper::core::support::ModelEngine;
    const auto directory = scratch_directory();
    FakeHttp http;
    http.length = 100;
    http.chunks = {"hello"};
    http.dropped = true; // the body fails after the first chunk

    ModelDownloadService service(http, directory);
    const auto status = service.download(ModelEngine::parakeet, "tdt-0.6b-v3-q4_k.gguf",
        {}, voicetyper::domain::CancellationToken{});

    check(status.is_error(), "оборванная загрузка сообщает об ошибке");
    check(!std::filesystem::exists(directory / "tdt-0.6b-v3-q4_k.gguf"),
        "целевой файл не появился");
    check(!std::filesystem::exists(directory / "tdt-0.6b-v3-q4_k.gguf.part"),
        "временный файл убран и при ошибке");
    std::size_t leftovers = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        static_cast<void>(entry);
        ++leftovers;
    }
    check(leftovers == 0, "в папке моделей не осталось мусора");
    std::filesystem::remove_all(directory);
}

void a_cancelled_download_leaves_nothing_behind()
{
    using voicetyper::core::support::ModelDownloadService;
    using voicetyper::core::support::ModelEngine;
    const auto directory = scratch_directory();
    FakeHttp http;
    http.length = 100;
    http.chunks = {"hello", "world"};

    // The user changed their mind before the transfer began: the service must refuse at once
    // and not even open the connection.
    voicetyper::domain::CancellationSource source;
    source.request_cancellation();
    ModelDownloadService service(http, directory);
    const auto status = service.download(ModelEngine::whisper, "ggml-base-q8_0.bin", {},
        source.token());
    check(status.is_error(), "отменённая загрузка сообщает об ошибке");
    check(http.last_url.empty(), "соединение даже не открывалось");
    check(!std::filesystem::exists(directory / "ggml-base-q8_0.bin"), "файла нет");
    check(!std::filesystem::exists(directory / "ggml-base-q8_0.bin.part"), "временного файла нет");
    std::filesystem::remove_all(directory);
}

void a_cancelled_transfer_leaves_no_leftovers()
{
    using voicetyper::core::support::ModelDownloadService;
    using voicetyper::core::support::ModelEngine;
    const auto directory = scratch_directory();
    voicetyper::domain::CancellationSource source;
    FakeHttp http;
    http.length = 1024;
    http.chunks = {"first", "second", "third"};
    http.cancel_after_first = &source; // the user presses cancel while it runs

    ModelDownloadService service(http, directory);
    const auto status = service.download(ModelEngine::parakeet, "tdt-0.6b-v3-q6_k.gguf", {},
        source.token());
    check(status.is_error(), "отмена во время загрузки — это ошибка");
    check(status.error().code() == voicetyper::domain::ErrorCode::cancelled,
        "код ошибки именно «отменено», а не «связь оборвалась»");
    std::size_t leftovers = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        static_cast<void>(entry);
        ++leftovers;
    }
    check(leftovers == 0, "после отмены в папке моделей пусто: ни .part, ни целевого файла");
    std::filesystem::remove_all(directory);
}

void an_unreachable_host_is_reported()
{
    using voicetyper::core::support::ModelDownloadService;
    using voicetyper::core::support::ModelEngine;
    const auto directory = scratch_directory();
    FakeHttp http;
    http.fails = true;
    ModelDownloadService service(http, directory);
    const auto status = service.download(ModelEngine::whisper, "ggml-tiny-q8_0.bin",
        {}, voicetyper::domain::CancellationToken{});
    check(status.is_error(), "недоступный хост даёт ошибку, а не молчание");
    check(!std::filesystem::exists(directory / "ggml-tiny-q8_0.bin"), "файла нет");
    std::filesystem::remove_all(directory);
}

} // namespace

int main()
{
    std::printf("model-download-contract:\n");
    the_urls_are_the_dotnet_repositories();
    the_file_names_are_the_catalog_ones();
    progress_reports_the_fraction_and_the_remaining_time();
    a_successful_download_writes_the_file();
    a_broken_transfer_leaves_nothing_behind();
    an_unreachable_host_is_reported();
    a_cancelled_download_leaves_nothing_behind();
    a_cancelled_transfer_leaves_no_leftovers();

    if (failures == 0) {
        std::printf("model-download-contract: OK (%d checks)\n", checks);
        return 0;
    }
    std::printf("model-download-contract: FAILED (%d of %d checks)\n", failures, checks);
    return 1;
}
