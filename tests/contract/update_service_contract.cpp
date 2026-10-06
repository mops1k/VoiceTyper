// Contract for the update flow: the release query, the installer download and the
// SHA-256 check (plan p_a95b558c6861, Phase 3).
//
// The network is the platform seam, so a fake client stands in for it and every path
// that matters to a user is exercised without a connection: a newer release, an
// up-to-date one, a missing release, a malformed body, a transport failure, a streamed
// download with progress, and a download whose hash does not match.

#include "core/support/sha256.hpp"
#include "core/support/update_manifest.hpp"
#include "core/support/update_service.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using voicetyper::platform::HttpByteStream;
using voicetyper::platform::HttpClient;
using voicetyper::platform::HttpRequest;
using voicetyper::platform::HttpResponse;
using voicetyper::platform::Result;
using voicetyper::platform::Status;
using voicetyper::platform::UpdateCheckResultKind;
using voicetyper::platform::UpdateInfo;
using voicetyper::platform::ErrorCode;
using voicetyper::platform::CancellationToken;

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FAIL %s\n", what.c_str());
    } else {
        std::printf("  ok   %s\n", what.c_str());
    }
}

template <typename T>
void check_equal(const T& actual, const T& expected, const std::string& what)
{
    check(actual == expected, what + " (expected " + std::to_string(expected) + ", got "
            + std::to_string(actual) + ")");
}

/// Strings have no std::to_string, and most checks here compare them.
void check_equal(const std::string& actual, const std::string& expected, const std::string& what)
{
    check(actual == expected, what + " (expected \"" + expected + "\", got \"" + actual + "\")");
}

/// A streaming body that hands out the canned chunks.
class FakeStream final : public HttpByteStream {
public:
    FakeStream(int status, std::optional<std::uint64_t> length, std::vector<std::string> chunks)
        : status_(status)
        , length_(length)
        , chunks_(std::move(chunks))
    {
    }

    [[nodiscard]] int status_code() const noexcept override { return status_; }
    [[nodiscard]] const std::vector<voicetyper::platform::HttpHeader>& headers() const noexcept override
    {
        return headers_;
    }
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept override { return length_; }

    Status read_into(const voicetyper::platform::HttpChunkSink& sink, const CancellationToken&) override
    {
        for (const std::string& chunk : chunks_) {
            if (!sink(chunk.data(), chunk.size())) {
                return Status::failure(ErrorCode::io_failure, "the sink aborted the transfer");
            }
        }
        return Status::success();
    }

    void close() override { closed_ = true; }

private:
    int status_ = 200;
    std::optional<std::uint64_t> length_;
    std::vector<std::string> chunks_;
    std::vector<voicetyper::platform::HttpHeader> headers_;
    bool closed_ = false;
};

/// The platform seam, canned.
class FakeHttp final : public HttpClient {
public:
    HttpResponse get_response{200, {}, "{}"};
    bool get_fails = false;
    std::string get_failure_message = "the name could not be resolved";

    bool open_fails = false;
    int open_status = 200;
    std::optional<std::uint64_t> open_length;
    std::vector<std::string> chunks;

    std::string last_get_url;
    std::vector<voicetyper::platform::HttpHeader> last_get_headers;
    std::string last_open_url;
    int get_calls = 0;
    int open_calls = 0;

    [[nodiscard]] Result<HttpResponse> get(const HttpRequest& request, const CancellationToken&) override
    {
        ++get_calls;
        last_get_url = request.url;
        last_get_headers = request.headers;
        if (get_fails) {
            return Result<HttpResponse>::failure(ErrorCode::unavailable, get_failure_message);
        }
        return Result<HttpResponse>(get_response);
    }

    [[nodiscard]] Result<std::unique_ptr<HttpByteStream>> open(
        const HttpRequest& request, const CancellationToken&) override
    {
        ++open_calls;
        last_open_url = request.url;
        if (open_fails) {
            return Result<std::unique_ptr<HttpByteStream>>::failure(
                ErrorCode::unavailable, "the installer host is unreachable");
        }
        return Result<std::unique_ptr<HttpByteStream>>(
            std::make_unique<FakeStream>(open_status, open_length, chunks));
    }
};

const char* kReleaseJson = R"({
  "tag_name": "v9.9.9",
  "prerelease": false,
  "body": "Release notes.\nSHA256: 1111111111111111111111111111111111111111111111111111111111111111",
  "assets": [
    {"name": "VoiceTyper-9.9.9-Setup.exe", "size": 1234, "browser_download_url": "https://example.invalid/setup.exe"}
  ]
})";

void a_newer_release_is_reported_with_the_frozen_headers()
{
    FakeHttp http;
    http.get_response = HttpResponse{200, {}, kReleaseJson};
    voicetyper::core::support::UpdateService service(http, "1.1.3");
    const auto result = service.check(CancellationToken{});

    check_equal(static_cast<int>(result.kind), static_cast<int>(UpdateCheckResultKind::update_available),
        std::string("the result kind"));
    check(result.update.has_value(), "the update is described");
    if (result.update.has_value()) {
        check_equal(result.update->version, std::string("9.9.9"), std::string("the version"));
        check_equal(result.update->installer_url.value_or(std::string()),
            std::string("https://example.invalid/setup.exe"), std::string("the installer url"));
        check_equal(result.update->sha256.value_or(std::string()),
            std::string("1111111111111111111111111111111111111111111111111111111111111111"),
            std::string("the sha256 from the release body"));
    }
    check_equal(http.get_calls, 1, std::string("GET calls"));
    check_equal(http.last_get_url, std::string(voicetyper::core::support::kUpdateLatestReleaseUrl),
        std::string("the queried url"));
    const std::string* agent = nullptr;
    const std::string* accept = nullptr;
    for (const auto& header : http.last_get_headers) {
        if (header.name == "User-Agent") {
            agent = &header.value;
        }
        if (header.name == "Accept") {
            accept = &header.value;
        }
    }
    check(agent != nullptr && *agent == std::string(voicetyper::platform::kHttpUserAgent),
        "the frozen User-Agent is sent");
    check(accept != nullptr && *accept == std::string(voicetyper::platform::kGithubReleaseAccept),
        "the GitHub Accept header is sent");
}

void the_same_version_is_up_to_date()
{
    FakeHttp http;
    http.get_response = HttpResponse{200, {}, kReleaseJson};
    voicetyper::core::support::UpdateService service(http, "9.9.9");
    const auto result = service.check(CancellationToken{});
    check_equal(static_cast<int>(result.kind), static_cast<int>(UpdateCheckResultKind::up_to_date),
        std::string("the result kind for the running version"));
}

void the_failure_statuses_and_bodies_are_explained()
{
    {
        FakeHttp http;
        http.get_response = HttpResponse{404, {}, ""};
        voicetyper::core::support::UpdateService service(http, "1.1.3");
        const auto result = service.check(CancellationToken{});
        check_equal(static_cast<int>(result.kind), static_cast<int>(UpdateCheckResultKind::failed),
            std::string("404 is a failed check"));
        check(result.error == std::string(voicetyper::core::support::kUpdateMessageReleaseNotFound),
            "404 carries the release-not-found message");
    }
    {
        FakeHttp http;
        http.get_response = HttpResponse{200, {}, "{not json"};
        voicetyper::core::support::UpdateService service(http, "1.1.3");
        const auto result = service.check(CancellationToken{});
        check_equal(static_cast<int>(result.kind), static_cast<int>(UpdateCheckResultKind::failed),
            std::string("a malformed body is a failed check"));
        check(!result.error.empty(), "a malformed body carries a message");
    }
    {
        FakeHttp http;
        http.get_fails = true;
        voicetyper::core::support::UpdateService service(http, "1.1.3");
        const auto result = service.check(CancellationToken{});
        check_equal(static_cast<int>(result.kind), static_cast<int>(UpdateCheckResultKind::failed),
            std::string("a transport failure is a failed check"));
        check(result.error.find(http.get_failure_message) != std::string::npos,
            "a transport failure carries the transport message");
    }
}

void a_download_is_streamed_hashed_and_reported()
{
    FakeHttp http;
    http.chunks = {std::string("Voice"), std::string("Typer"), std::string("-installer")};
    http.open_length = 21;
    const auto path = std::filesystem::temp_directory_path() / "voicetyper-update-service-test.bin";
    std::filesystem::remove(path);

    const std::string payload = "VoiceTyper-installer";
    UpdateInfo info;
    info.version = "9.9.9";
    info.installer_url = "https://example.invalid/setup.exe";
    info.sha256 = voicetyper::core::support::sha256_hex(payload);

    voicetyper::core::support::UpdateService service(http, "1.1.3");
    std::vector<std::uint64_t> progress;
    const Status status = service.download(info, path, [&progress](std::uint64_t received, std::uint64_t) {
        progress.push_back(received);
    }, CancellationToken{});

    check(status.is_ok(), "a verified download succeeds");
    check_equal(http.open_calls, 1, std::string("streamed downloads"));
    check_equal(http.last_open_url, info.installer_url.value_or(std::string()),
        std::string("the installer url"));
    check(std::filesystem::exists(path), "the installer file exists");
    {
        // Scoped on purpose: Windows refuses to delete a file that is still open, and
        // the mismatch check below deletes this very file.
        std::ifstream file(path, std::ios::binary);
        const std::string written((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        check_equal(written, payload, std::string("the written bytes"));
    }
    check(progress.size() == 3, "progress was reported per chunk");
    check(!progress.empty() && progress.back() == payload.size(), "progress ends at the total size");

    UpdateInfo wrong = info;
    wrong.sha256 = std::string(64, 'a');
    const Status mismatch = service.download(wrong, path, {}, CancellationToken{});
    check(!mismatch.is_ok(), "a hash mismatch fails the download");
    check(!std::filesystem::exists(path), "a mismatching installer is removed");
}

void a_failed_open_leaves_no_file()
{
    FakeHttp http;
    http.open_fails = true;
    const auto path = std::filesystem::temp_directory_path() / "voicetyper-update-service-absent.bin";
    std::filesystem::remove(path);
    UpdateInfo info;
    info.version = "9.9.9";
    info.installer_url = "https://example.invalid/setup.exe";
    voicetyper::core::support::UpdateService service(http, "1.1.3");
    const Status status = service.download(info, path, {}, CancellationToken{});
    check(!status.is_ok(), "an unreachable installer fails");
    check(!std::filesystem::exists(path), "no file is left behind");
}

} // namespace

int main()
{
    std::printf("update-service-contract: the release query, the download and the hash\n");
    a_newer_release_is_reported_with_the_frozen_headers();
    the_same_version_is_up_to_date();
    the_failure_statuses_and_bodies_are_explained();
    a_download_is_streamed_hashed_and_reported();
    a_failed_open_leaves_no_file();
    if (failures == 0) {
        std::printf("update-service-contract: OK (%d checks)\n", checks);
        return 0;
    }
    std::printf("update-service-contract: %d checks, %d failures\n", checks, failures);
    return 1;
}
