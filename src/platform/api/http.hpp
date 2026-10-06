#pragma once

// HTTP client contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §4 (model download
// defaults) and §8 (release API, User-Agent, timeout).
//
// Privacy boundary — this is a hard rule, not a style note: the C++ port must
// not add any network path for audio or recognized text. The only traffic this
// client may carry is (a) model file downloads from the frozen catalog URLs,
// (b) the GitHub release metadata query, and (c) the release asset download.
// feature-parity.md lists "no telemetry in current source" as a blocking parity
// row. Any new caller must be added deliberately, not "while we are here".
//
// Frozen defaults: 30 minute timeout, User-Agent "VoiceTyper/1.0". The release
// check additionally sends Accept: application/vnd.github+json.
//
// The client must not follow redirects to a different host silently for the
// release API; a redirect to a new host is a protocol change worth reporting.
// Model downloads may follow ordinary CDN redirects, which the frozen catalog
// relies on.
//
// Thread affinity: safe from any thread. Callers own the CancellationToken and
// the response body. Ownership: response bodies are owned by the caller and are
// valid until the next call on the same instance. A ByteStream is a move-only
// handle that must be closed before the call returns, and its read callback runs
// on the download thread.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace voicetyper::platform {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

namespace detail {

[[nodiscard]] inline bool header_name_equals(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto left = static_cast<char>(lhs[i] >= 'A' && lhs[i] <= 'Z' ? lhs[i] + ('a' - 'A') : lhs[i]);
        const auto right = static_cast<char>(rhs[i] >= 'A' && rhs[i] <= 'Z' ? rhs[i] + ('a' - 'A') : rhs[i]);
        if (left != right) {
            return false;
        }
    }
    return true;
}

} // namespace detail

/// Default request timeout (30 minutes), matching the .NET HttpClient.
inline constexpr std::chrono::seconds kHttpDefaultTimeout{1800};
/// User-Agent sent with every request. Frozen.
inline constexpr std::string_view kHttpUserAgent = "VoiceTyper/1.0";
/// Accept header value for the GitHub release API.
inline constexpr std::string_view kGithubReleaseAccept = "application/vnd.github+json";
/// Copy buffer used for streaming downloads, bytes. Frozen at 128 KiB.
inline constexpr std::size_t kHttpCopyBufferBytes = 128 * 1024;

/// One header field.
struct HttpHeader {
    std::string name;
    std::string value;
};

/// A request. No body is needed: VoiceTyper only performs GETs.
struct HttpRequest {
    std::string url;
    std::vector<HttpHeader> headers;
    /// Overrides kHttpDefaultTimeout when nonzero.
    std::chrono::seconds timeout{};

    /// Case-insensitive lookup, returning nullptr when absent.
    [[nodiscard]] const std::string* find_header(std::string_view name) const noexcept
    {
        for (const auto& header : headers) {
            if (detail::header_name_equals(header.name, name)) {
                return &header.value;
            }
        }
        return nullptr;
    }
};

/// A buffered response. Bodies here are release metadata, never binary payloads.
struct HttpResponse {
    int status_code = 0;
    std::vector<HttpHeader> headers;
    std::string body;

    /// Case-insensitive header lookup.
    [[nodiscard]] const std::string* find_header(std::string_view name) const noexcept
    {
        for (const auto& header : headers) {
            if (detail::header_name_equals(header.name, name)) {
                return &header.value;
            }
        }
        return nullptr;
    }

    /// True for 2xx.
    [[nodiscard]] bool is_success() const noexcept { return status_code >= 200 && status_code < 300; }

    /// Content-Length when present and parseable, else nullopt.
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept
    {
        const auto* value = find_header("Content-Length");
        if (value == nullptr || value->empty()) {
            return std::nullopt;
        }
        std::uint64_t parsed = 0;
        for (const auto ch : *value) {
            if (ch < '0' || ch > '9') {
                return std::nullopt;
            }
            parsed = parsed * 10U + static_cast<std::uint64_t>(ch - '0');
        }
        return parsed;
    }
};

/// A streaming response body. The callback receives each chunk and returns
/// false to abort the transfer.
using HttpChunkSink = std::function<bool(const char* data, std::size_t size)>;

/// Owns an open response body. Closing is idempotent; the destructor closes.
class HttpByteStream {
public:
    HttpByteStream() = default;
    virtual ~HttpByteStream() = default;

    HttpByteStream(const HttpByteStream&) = delete;
    HttpByteStream& operator=(const HttpByteStream&) = delete;
    HttpByteStream(HttpByteStream&&) = delete;
    HttpByteStream& operator=(HttpByteStream&&) = delete;

    /// Status code of the open response.
    [[nodiscard]] virtual int status_code() const noexcept = 0;

    /// Response headers, available before the first read.
    [[nodiscard]] virtual const std::vector<HttpHeader>& headers() const noexcept = 0;

    /// Content-Length when the server sent it.
    [[nodiscard]] virtual std::optional<std::uint64_t> content_length() const noexcept = 0;

    /// Streams the body into `sink`. Blocks until the body ends, `sink` returns
    /// false, or the token fires. Failure: cancelled, io_failure.
    virtual Status read_into(const HttpChunkSink& sink, const CancellationToken& cancellation) = 0;

    /// Ends the transfer and releases the connection. Idempotent.
    virtual void close() = 0;
};

class HttpClient {
public:
    virtual ~HttpClient() = default;

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

    /// Performs a GET and buffers the whole body. Used for release metadata.
    /// Failure codes: unavailable (DNS/connect), timeout, cancelled, io_failure,
    /// and a protocol failure for a non-2xx status, which the caller inspects
    /// via HttpResponse::status_code rather than as an error.
    [[nodiscard]] virtual Result<HttpResponse> get(const HttpRequest& request, const CancellationToken& cancellation) = 0;

    /// Opens a streaming GET for a large file. The caller must consume or close
    /// the stream. Failure codes: unavailable, timeout, cancelled, io_failure.
    [[nodiscard]] virtual Result<std::unique_ptr<HttpByteStream>> open(
        const HttpRequest& request, const CancellationToken& cancellation) = 0;

protected:
    HttpClient() = default;
};

} // namespace voicetyper::platform
