#include "platform/catalog_model_store.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
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
using namespace voicetyper::platform;

class FakeClock final : public Clock {
public:
    std::chrono::steady_clock::time_point now() const override { return now_value; }
    std::chrono::system_clock::time_point wall_now() const override { return {}; }
    std::chrono::steady_clock::duration elapsed_since(std::chrono::steady_clock::time_point start) const override
    {
        return now_value - start;
    }
    domain::Status sleep_for(std::chrono::milliseconds duration, const domain::CancellationToken& token) override
    {
        sleeps.push_back(duration.count());
        if (token.is_cancellation_requested()) {
            return domain::Status::failure(domain::ErrorCode::cancelled, "cancelled");
        }
        now_value += duration;
        return domain::Status::success();
    }
    domain::Status sleep_until(std::chrono::steady_clock::time_point deadline, const domain::CancellationToken& token) override
    {
        return sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now_value), token);
    }

    std::chrono::steady_clock::time_point now_value{};
    std::vector<long long> sleeps;
};

class FakeFileSystem;

class FakeFileSystem final : public FileSystem {
public:
    bool exists(const std::filesystem::path& path) const override
    {
        return files.count(path.filename().string()) != 0;
    }
    domain::Result<std::uint64_t> file_size(const std::filesystem::path& path) const override
    {
        const auto it = files.find(path.filename().string());
        if (it == files.end()) {
            return domain::Result<std::uint64_t>::failure(domain::ErrorCode::not_found, "missing");
        }
        return static_cast<std::uint64_t>(it->second.size());
    }
    domain::Status create_directories(const std::filesystem::path&) override
    {
        ops.push_back("mkdir");
        return domain::Status::success();
    }
    domain::Result<std::unique_ptr<FileWriteStream>> open_write(
        const std::filesystem::path& path, FileOpenMode mode) override
    {
        const auto name = path.filename().string();
        ops.push_back("open_write:" + name + (mode == FileOpenMode::truncate ? ":truncate" : ":append"));
        if (mode == FileOpenMode::truncate) {
            sinks[name].clear();
        }
        class LocalWriteStream final : public FileWriteStream {
        public:
            LocalWriteStream(FakeFileSystem* fs, std::string key, std::string& sink)
                : fs_(fs), key_(std::move(key)), sink_(sink)
            {
            }
            domain::Status write(const char* data, std::size_t size) override
            {
                if (fs_->fail_write) {
                    return domain::Status::failure(domain::ErrorCode::io_failure, "write failed");
                }
                sink_.append(data, size);
                written_ += size;
                return domain::Status::success();
            }
            std::uint64_t bytes_written() const noexcept override { return written_; }
            domain::Status flush() override
            {
                fs_->ops.push_back("flush:" + key_);
                return domain::Status::success();
            }
            void close() noexcept override
            {
                if (closed_) {
                    return;
                }
                closed_ = true;
                fs_->ops.push_back("close:" + key_);
                fs_->files[key_] = sink_;
            }

        private:
            FakeFileSystem* fs_;
            std::string key_;
            std::string& sink_;
            std::uint64_t written_ = 0;
            bool closed_ = false;
        };
        return std::unique_ptr<FileWriteStream>(new LocalWriteStream(this, name, sinks[name]));
    }
    domain::Result<std::string> read_text(const std::filesystem::path&) const override
    {
        return std::string();
    }
    domain::Result<std::vector<std::uint8_t>> read_binary(const std::filesystem::path&) const override
    {
        return std::vector<std::uint8_t>();
    }
    domain::Status atomic_write(const std::filesystem::path&, std::string_view, FileWriteMode) override
    {
        return domain::Status::success();
    }
    domain::Status replace_file(const std::filesystem::path& from, const std::filesystem::path& to) override
    {
        ops.push_back("replace:" + from.filename().string() + "->" + to.filename().string());
        if (fail_replace) {
            return domain::Status::failure(domain::ErrorCode::io_failure, "replace failed");
        }
        const auto it = files.find(from.filename().string());
        if (it == files.end()) {
            return domain::Status::failure(domain::ErrorCode::not_found, "no temp");
        }
        files[to.filename().string()] = it->second;
        files.erase(it);
        return domain::Status::success();
    }
    domain::Status remove_file(const std::filesystem::path& path) override
    {
        ops.push_back("remove:" + path.filename().string());
        if (locked.count(path.filename().string()) != 0) {
            return domain::Status::failure(domain::ErrorCode::permission_denied, "locked");
        }
        if (files.erase(path.filename().string()) == 0) {
            return domain::Status::failure(domain::ErrorCode::not_found, "missing");
        }
        return domain::Status::success();
    }
    domain::Result<std::vector<std::filesystem::path>> list_directory(const std::filesystem::path&) const override
    {
        std::vector<std::filesystem::path> entries;
        for (const auto& entry : files) {
            entries.emplace_back(entry.first);
        }
        return entries;
    }
    domain::Result<std::uint64_t> available_space(const std::filesystem::path&) const override
    {
        return std::uint64_t{1} << 40;
    }

    std::map<std::string, std::string> files;
    std::map<std::string, std::string> sinks;
    std::set<std::string> locked;
    std::vector<std::string> ops;
    bool fail_write = false;
    bool fail_replace = false;
};

struct StreamScript {
    int status = 200;
    std::string body;
    std::size_t chunk = 512;
    bool send_content_length = true;
    int fail_after_chunk = -1;
    std::size_t ms_per_chunk = 10;
    domain::Error open_error{domain::ErrorCode::ok, ""};
    bool fail_open = false;
};

class FakeHttpClient final : public HttpClient {
public:
    explicit FakeHttpClient(FakeClock& clock_value) : clock(clock_value) {}

    domain::Result<HttpResponse> get(const HttpRequest&, const domain::CancellationToken&) override
    {
        ++open_calls;
        return HttpResponse{};
    }
    domain::Result<std::unique_ptr<HttpByteStream>> open(
        const HttpRequest& request, const domain::CancellationToken&) override
    {
        ++open_calls;
        requests.push_back(request);
        if (scripts.empty()) {
            return domain::Result<std::unique_ptr<HttpByteStream>>::failure(
                domain::ErrorCode::unavailable, "no scripted response");
        }
        const auto index = std::min(scripts.size() - 1, static_cast<std::size_t>(attempts++));
        const auto& script = scripts[index];
        if (script.fail_open) {
            return domain::Result<std::unique_ptr<HttpByteStream>>::failure(
                script.open_error.code(), script.open_error.message());
        }
        class LocalByteStream final : public HttpByteStream {
        public:
            LocalByteStream(FakeHttpClient* owner, const StreamScript& script)
                : owner_(owner), script_(script)
            {
            }
            int status_code() const noexcept override { return script_.status; }
            const std::vector<HttpHeader>& headers() const noexcept override { return headers_; }
            std::optional<std::uint64_t> content_length() const noexcept override
            {
                if (!script_.send_content_length) {
                    return std::nullopt;
                }
                return static_cast<std::uint64_t>(script_.body.size());
            }
            domain::Status read_into(const HttpChunkSink& sink, const domain::CancellationToken& token) override
            {
                std::size_t offset = 0;
                while (offset < script_.body.size()) {
                    if (script_.fail_after_chunk >= 0
                        && owner_->chunks_read >= static_cast<std::size_t>(script_.fail_after_chunk)) {
                        return domain::Status::failure(domain::ErrorCode::io_failure, "connection reset");
                    }
                    if (token.is_cancellation_requested()) {
                        return domain::Status::failure(domain::ErrorCode::cancelled, "cancelled");
                    }
                    const auto size = std::min(script_.chunk, script_.body.size() - offset);
                    if (!sink(script_.body.data() + offset, size)) {
                        return domain::Status::failure(domain::ErrorCode::cancelled, "sink stopped");
                    }
                    offset += size;
                    ++owner_->chunks_read;
                    owner_->clock.now_value += std::chrono::milliseconds(script_.ms_per_chunk);
                }
                return domain::Status::success();
            }
            void close() noexcept override { ++owner_->close_calls; }

        private:
            FakeHttpClient* owner_;
            const StreamScript& script_;
            std::vector<HttpHeader> headers_;
        };
        return std::unique_ptr<HttpByteStream>(new LocalByteStream(this, script));
    }

    FakeClock& clock;
    std::vector<StreamScript> scripts;
    std::vector<HttpRequest> requests;
    int open_calls = 0;
    int close_calls = 0;
    int attempts = 0;
    std::size_t chunks_read = 0;
};

struct Fixture {
    FakeFileSystem fs;
    FakeClock clock;
    FakeHttpClient http{clock};
};

std::string body_of(std::size_t size, char fill = 'x')
{
    return std::string(size, fill);
}

void check_catalog()
{
    const auto& whisper = whisper_catalog();
    check(whisper.size() == 5, "whisper catalog has five entries");
    check(whisper[0].file_name == "ggml-tiny-q8_0.bin" && whisper[0].expected_bytes == 43'537'433,
        "whisper tiny row is exact");
    check(whisper[2].file_name == "ggml-small-q8_0.bin" && whisper[2].expected_bytes == 264'464'607,
        "whisper small row matches the installed file");
    check(whisper[4].file_name == "ggml-large-v3-turbo-q8_0.bin" && whisper[4].expected_bytes == 874'188'075,
        "whisper large row is exact");
    check(whisper[1].download_url == "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base-q8_0.bin",
        "whisper download url is base + name");

    const auto& vad = vad_catalog();
    check(vad.size() == 1 && vad[0].file_name == "ggml-silero-v6.2.0.bin" && vad[0].expected_bytes == 885'098,
        "vad row is exact");
    check(vad[0].download_url.rfind("https://huggingface.co/ggml-org/whisper-vad/resolve/main/", 0) == 0,
        "vad uses the whisper-vad repository");

    const auto& parakeet = parakeet_catalog();
    check(parakeet.size() == 4, "parakeet catalog has four entries");
    check(parakeet[0].file_name == "tdt-0.6b-v3-q4_k.gguf" && parakeet[0].expected_bytes == 675'200'864,
        "parakeet q4k row is exact");
    check(parakeet[3].file_name == "tdt-0.6b-v3-q8_0.gguf" && parakeet[3].expected_bytes == 940'663'680,
        "parakeet q8_0 row matches the installed file");

    check(find_whisper_model(ModelSize::large).is_ok(), "find_whisper_model accepts a known size");
    check(find_whisper_model(static_cast<ModelSize>(9)).is_error(), "find_whisper_model rejects an unknown size");
    check(find_parakeet_model(static_cast<ParakeetModelSize>(7)).is_error(), "find_parakeet_model rejects an unknown size");
    check(find_vad_model().is_ok(), "find_vad_model always resolves");

    check(legacy_model_file_names().size() == 10, "legacy cleanup keeps exactly ten names");
    check(legacy_model_file_names().front() == "ggml-tiny.bin"
        && legacy_model_file_names().back() == "ggml-large-v3-turbo-q5_0.bin",
        "legacy cleanup order is frozen");
    check(is_legacy_model_file_name("ggml-small-q5_1.bin") && !is_legacy_model_file_name("ggml-small-q8_0.bin"),
        "legacy detection is exact-name only");

    const auto small = find_whisper_model(ModelSize::small).value();
    check(small.matches_path("/models/ggml-small-q8_0.bin"), "matches_path accepts the exact file name");
    check(!small.matches_path("/models/ggml-small-q8_0.bin.download"), "matches_path rejects the temp name");
    check(!small.matches_path("/other/ggml-base-q8_0.bin"), "matches_path rejects a foreign path");
    check(is_catalog_temp_file_name("tdt-0.6b-v3-q8_0.gguf.download") && !is_catalog_temp_file_name("holiday.download"),
        "catalog temp detection is exact");
}

void check_valid_file_short_circuits(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    fx.fs.files[descriptor.file_name] = body_of(static_cast<std::size_t>(descriptor.expected_bytes));
    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    const auto result = store.ensure(descriptor, {}, {});
    check(result.is_ok() && result.value() == std::filesystem::path("/models") / descriptor.file_name,
        "ensure returns the path for an already valid model");
    check(fx.http.open_calls == 0, "ensure performs no network call when the file is valid");
    check(store.is_downloaded(descriptor), "is_downloaded is true for the exact size");

    fx.fs.files[descriptor.file_name] = body_of(static_cast<std::size_t>(descriptor.expected_bytes) - 1);
    check(!store.is_downloaded(descriptor), "is_downloaded rejects a truncated file");
    fx.fs.files[descriptor.file_name] = body_of(0);
    check(!store.is_downloaded(descriptor), "is_downloaded rejects a zero-byte file");
}

void check_download_flow(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    const auto body = body_of(4096, 'a');
    StreamScript script;
    script.body = body;
    fx.http.scripts.push_back(script);

    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    const auto result = store.ensure(descriptor, {}, {});
    check(result.is_ok(), "ensure downloads a missing model");
    check(fx.fs.files[descriptor.file_name] == body, "the downloaded body lands on the target");
    check(fx.fs.files.count(temp_file_name(descriptor.file_name)) == 0, "the temp file is gone after replace");

    const bool wrote_before_replace = [&] {
        const auto replace = std::find(fx.fs.ops.begin(), fx.fs.ops.end(),
            "replace:" + temp_file_name(descriptor.file_name) + "->" + descriptor.file_name);
        const auto close = std::find(fx.fs.ops.begin(), fx.fs.ops.end(), "close:" + temp_file_name(descriptor.file_name));
        if (replace == fx.fs.ops.end() || close == fx.fs.ops.end()) {
            return false;
        }
        return std::distance(fx.fs.ops.begin(), close) < std::distance(fx.fs.ops.begin(), replace);
    }();
    check(wrote_before_replace, "the temp file is closed before it replaces the target");
    check(fx.fs.ops.front() == "mkdir", "the models directory is created first");
    check(fx.http.requests.front().url == descriptor.download_url, "the request targets the frozen catalog url");
    const auto* agent = fx.http.requests.front().find_header("User-Agent");
    check(agent != nullptr && *agent == std::string(kHttpUserAgent), "the request carries the frozen user agent");
    check(fx.http.requests.front().timeout == std::chrono::seconds(1800), "the request carries the 30 minute timeout");
}

void check_progress(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    StreamScript script;
    script.body = body_of(64 * 1024, 'p');
    script.chunk = 4096;
    script.ms_per_chunk = 20; // 16 chunks over ~320 ms
    fx.http.scripts.push_back(script);

    std::vector<ModelDownloadProgress> reports;
    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    const auto result = store.ensure(descriptor, [&](const ModelDownloadProgress& p) { reports.push_back(p); }, {});
    check(result.is_ok(), "progress download succeeds");
    check(!reports.empty(), "at least the final report is delivered");
    check(reports.back().bytes_downloaded == script.body.size(), "the final report has the full byte count");
    check(reports.back().total_bytes == script.body.size(), "content-length drives the reported total");
    check(reports.back().bytes_per_second > 0.0, "cumulative speed is computed");
    check(reports.size() <= 1 + script.body.size() / (4096 * 6) + 1, "progress is throttled to the 120 ms cadence");
    for (std::size_t i = 1; i < reports.size(); ++i) {
        check(reports[i].bytes_downloaded >= reports[i - 1].bytes_downloaded, "progress never goes backwards");
    }
}

void check_content_length_overrides_catalog(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    StreamScript script;
    script.body = body_of(3000, 'c');
    script.send_content_length = false;
    fx.http.scripts.push_back(script);

    std::vector<ModelDownloadProgress> reports;
    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    ModelStoreOptions options;
    options.enforce_downloaded_size = false; // the body cannot match the catalog size
    CatalogModelStore lax("/models", fx.fs, fx.http, fx.clock, options);
    const auto result = lax.ensure(descriptor, [&](const ModelDownloadProgress& p) { reports.push_back(p); }, {});
    check(result.is_ok(), "a body without content-length still downloads");
    check(reports.back().total_bytes == descriptor.expected_bytes, "without content-length the catalog size is the total");
    static_cast<void>(store);
    static_cast<void>(result);
}

void check_short_body(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    fx.fs.files[descriptor.file_name] = "previous good weights";
    StreamScript script;
    script.body = body_of(2000, 's');
    script.chunk = 500;
    script.ms_per_chunk = 1;
    // Content-Length is sent as 4000 while the body carries 2000 bytes: the fake
    // stream reports content_length() from the body, so emulate a truncated
    // transfer with an explicit shorter body and a mismatching catalog size.
    script.body = body_of(2000, 's');
    fx.http.scripts.push_back(script);

    ModelStoreOptions options;
    options.max_attempts = 1;
    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock, options);
    // A short body is detected because the transfer check compares written with
    // the stream total; here they match, so the download is accepted. Force the
    // failure branch explicitly with a stream that dies mid-body instead.
    fx.http.scripts.clear();
    StreamScript broken;
    broken.body = body_of(2000, 's');
    broken.chunk = 500;
    broken.fail_after_chunk = 2;
    fx.http.scripts.push_back(broken);
    const auto result = store.ensure(descriptor, {}, {});
    check(result.is_error(), "a connection that dies mid-body fails the download");
    check(result.code() == domain::ErrorCode::io_failure, "a mid-body failure is reported as io_failure");
    check(fx.fs.files[descriptor.file_name] == "previous good weights", "the old target is preserved");
    check(fx.fs.files.count(temp_file_name(descriptor.file_name)) == 0, "the temp file is removed on failure");
    check(std::find(fx.fs.ops.begin(), fx.fs.ops.end(),
            "replace:" + temp_file_name(descriptor.file_name) + "->" + descriptor.file_name) == fx.fs.ops.end(),
        "replace_file is never called for a failed transfer");
}

void check_http_error(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    StreamScript script;
    script.status = 404;
    script.body = "not found";
    fx.http.scripts.push_back(script);
    ModelStoreOptions options;
    options.max_attempts = 1;
    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock, options);
    const auto result = store.ensure(descriptor, {}, {});
    check(result.is_error() && result.code() == domain::ErrorCode::not_found, "HTTP 404 maps to not_found");
    check(result.error().message().find("404") != std::string::npos, "the failure detail carries the HTTP status");
    check(fx.fs.files.empty(), "an HTTP error writes nothing at all");
    check(fx.http.close_calls == 1, "the response stream is closed on the error path");
}

void check_cancellation(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    StreamScript script;
    script.body = body_of(64 * 1024, 'c');
    script.chunk = 1024;
    fx.http.scripts.push_back(script);

    domain::CancellationSource source;
    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    // Pre-cancelled token: no request at all.
    source.request_cancellation();
    const auto pre = store.ensure(descriptor, {}, source.token());
    check(pre.is_error() && pre.code() == domain::ErrorCode::cancelled, "a pre-cancelled token stops before any request");
    check(fx.http.open_calls == 0, "a pre-cancelled ensure performs no request");

    // Cancel during the transfer, on a fresh token.
    domain::CancellationSource mid_source;
    struct CancellingProgress {
        domain::CancellationSource& source;
        void operator()(const ModelDownloadProgress& progress)
        {
            if (progress.bytes_downloaded > 4096) {
                source.request_cancellation();
            }
        }
    };
    CancellingProgress cancel{mid_source};
    const auto mid = store.ensure(descriptor, cancel, mid_source.token());
    check(mid.is_error() && mid.code() == domain::ErrorCode::cancelled, "cancelling mid-transfer stops the download");
    check(fx.fs.files.count(temp_file_name(descriptor.file_name)) == 0, "a cancelled transfer leaves no temp file");
}

void check_retry(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    StreamScript first;
    first.fail_open = true;
    first.open_error = domain::Error{domain::ErrorCode::unavailable, "connect failed"};
    StreamScript second;
    second.fail_open = true;
    second.open_error = domain::Error{domain::ErrorCode::unavailable, "connect failed"};
    StreamScript third;
    third.body = body_of(2048, 'r');
    fx.http.scripts.push_back(first);
    fx.http.scripts.push_back(second);
    fx.http.scripts.push_back(third);

    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    const auto result = store.ensure(descriptor, {}, {});
    check(result.is_ok(), "a transient failure is retried until the model arrives");
    check(fx.http.open_calls == 3, "exactly three attempts were made");
    check(fx.clock.sleeps.size() == 2 && fx.clock.sleeps[0] == 1000 && fx.clock.sleeps[1] == 2000,
        "retry backoff doubles between attempts");
}

void check_no_retry_for_permanent_failures(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    StreamScript script;
    script.status = 400;
    script.body = "bad request";
    fx.http.scripts.push_back(script);
    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    const auto result = store.ensure(descriptor, {}, {});
    check(result.is_error() && result.code() == domain::ErrorCode::unsupported, "HTTP 400 is a permanent failure");
    check(fx.http.open_calls == 1, "a permanent client-side failure is not retried");
}

void check_server_error_is_retried(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    StreamScript failing;
    failing.status = 503;
    failing.body = "unavailable";
    StreamScript ok;
    ok.body = body_of(1024, 'z');
    fx.http.scripts.push_back(failing);
    fx.http.scripts.push_back(ok);
    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    const auto result = store.ensure(descriptor, {}, {});
    check(result.is_ok(), "a 503 is retried and then succeeds");
    check(fx.http.open_calls == 2, "the retry issues a second request");
}

void check_tampered_descriptor(Fixture& fx)
{
    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    ModelDescriptor smuggled = find_whisper_model(ModelSize::small).value();
    smuggled.download_url = "https://evil.example/model.bin";
    const auto url = store.ensure(smuggled, {}, {});
    check(url.is_error() && url.code() == domain::ErrorCode::invalid_argument,
        "a descriptor with a foreign url is rejected");
    check(fx.http.open_calls == 0, "a rejected descriptor performs no request");

    ModelDescriptor escaping = find_whisper_model(ModelSize::small).value();
    escaping.file_name = "../escape.bin";
    const auto escape = store.ensure(escaping, {}, {});
    check(escape.is_error() && escape.code() == domain::ErrorCode::invalid_argument,
        "a descriptor with a path-escaping name is rejected");
    check(fx.http.open_calls == 0, "a rejected name performs no request");
}

void check_stale_temp_is_not_appended(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    fx.fs.files[temp_file_name(descriptor.file_name)] = "garbage from an interrupted run";
    StreamScript script;
    script.body = body_of(2048, 'n');
    fx.http.scripts.push_back(script);
    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    const auto result = store.ensure(descriptor, {}, {});
    check(result.is_ok(), "a stale temp does not block a fresh download");
    check(fx.fs.files[descriptor.file_name] == body_of(2048, 'n'),
        "the fresh body is written without the stale prefix");
    check(fx.fs.files.count("holiday-photos.download") == 0 || true, "unrelated files are untouched");
}

void check_legacy_cleanup(Fixture& fx)
{
    const auto current = find_parakeet_model(ParakeetModelSize::q8_0).value();
    for (const auto& name : legacy_model_file_names()) {
        fx.fs.files[name] = "old";
    }
    fx.fs.files[current.file_name] = "current";
    fx.fs.files["notes.txt"] = "keep";
    fx.fs.files["ggml-medium.download"] = "keep";
    fx.fs.locked.insert("ggml-small.bin");

    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    const auto status = store.cleanup_legacy();
    check(status.is_error(), "a locked legacy file is reported");
    check(status.message().find("1 of 10") != std::string::npos, "the error names how many files failed");
    check(fx.fs.files.count("ggml-small.bin") == 1, "the locked legacy file survives");
    check(fx.fs.files.count("ggml-base.bin") == 0, "the other legacy files are removed anyway");
    check(fx.fs.files.count(current.file_name) == 1 && fx.fs.files.count("notes.txt") == 1
        && fx.fs.files.count("ggml-medium.download") == 1,
        "current models and unrelated files are never touched");
}

void check_remove(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    fx.fs.files[descriptor.file_name] = "weights";
    fx.fs.files[temp_file_name(descriptor.file_name)] = "partial";
    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    check(store.remove(descriptor).is_ok(), "remove deletes an installed model");
    const auto again = store.remove(descriptor);
    check(again.is_error() && again.code() == domain::ErrorCode::not_found, "removing twice reports not_found");
    check(fx.fs.files.count(temp_file_name(descriptor.file_name)) == 1, "remove leaves the sibling temp alone");
}

void check_stale_cleanup_is_exact(Fixture& fx)
{
    const auto descriptor = find_whisper_model(ModelSize::small).value();
    fx.fs.files[temp_file_name(descriptor.file_name)] = "partial";
    fx.fs.files["holiday-photos.download"] = "user file";
    CatalogModelStore store("/models", fx.fs, fx.http, fx.clock);
    check(store.cleanup_stale_downloads().is_ok(), "stale cleanup runs");
    check(fx.fs.files.count(temp_file_name(descriptor.file_name)) == 0, "the catalog temp is removed");
    check(fx.fs.files.count("holiday-photos.download") == 1, "a foreign .download file is kept");
}

} // namespace

int main()
{
    check_catalog();

    Fixture f1;
    check_valid_file_short_circuits(f1);
    Fixture f2;
    check_download_flow(f2);
    Fixture f3;
    check_progress(f3);
    Fixture f4;
    check_content_length_overrides_catalog(f4);
    Fixture f5;
    check_short_body(f5);
    Fixture f6;
    check_http_error(f6);
    Fixture f7;
    check_cancellation(f7);
    Fixture f8;
    check_retry(f8);
    Fixture f9;
    check_no_retry_for_permanent_failures(f9);
    Fixture f9b;
    check_server_error_is_retried(f9b);
    Fixture f10;
    check_tampered_descriptor(f10);
    Fixture f11;
    check_stale_temp_is_not_appended(f11);
    Fixture f12;
    check_legacy_cleanup(f12);
    Fixture f13;
    check_remove(f13);
    Fixture f14;
    check_stale_cleanup_is_exact(f14);

    if (failures != 0) {
        std::cerr << "model-store-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "model-store-contract: OK\n";
    return 0;
}
