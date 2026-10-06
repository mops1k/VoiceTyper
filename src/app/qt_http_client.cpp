#include "app/qt_http_client.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <cstdint>

namespace voicetyper::app {

namespace {

QNetworkRequest to_qt_request(const platform::HttpRequest& request)
{
    QNetworkRequest qt_request{QUrl(QString::fromStdString(request.url))};
    qt_request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    for (const auto& header : request.headers) {
        qt_request.setRawHeader(QByteArray::fromStdString(header.name),
            QByteArray::fromStdString(header.value));
    }
    // Qt sets its own User-Agent default; the frozen one must win when the caller asked
    // for it, and Qt's default must not appear when the caller did not.
    if (request.find_header("User-Agent") == nullptr) {
        qt_request.setRawHeader("User-Agent", QByteArray());
    }
    return qt_request;
}

/// Waits for a reply, honouring the request timeout and the cancellation token.
/// Returns false when the wait ended because of the cancellation.
bool wait_for_reply(QNetworkReply& reply, const platform::HttpRequest& request,
    const platform::CancellationToken& cancellation)
{
    QEventLoop loop;
    QObject::connect(&reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timeout_timer;
    const std::chrono::seconds timeout = request.timeout.count() > 0 ? request.timeout
                                                                    : platform::kHttpDefaultTimeout;
    timeout_timer.setSingleShot(true);
    timeout_timer.setInterval(static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()));
    QObject::connect(&timeout_timer, &QTimer::timeout, &reply, &QNetworkReply::abort);
    timeout_timer.start();

    QTimer cancellation_timer;
    if (cancellation.is_cancellation_requested()) {
        reply.abort();
    }
    cancellation_timer.setInterval(200);
    QObject::connect(&cancellation_timer, &QTimer::timeout, &reply, [&reply, &cancellation] {
        if (cancellation.is_cancellation_requested()) {
            reply.abort();
        }
    });
    cancellation_timer.start();

    loop.exec();
    return !cancellation.is_cancellation_requested();
}

std::vector<platform::HttpHeader> collect_headers(const QNetworkReply& reply)
{
    std::vector<platform::HttpHeader> headers;
    const auto pairs = reply.rawHeaderPairs();
    headers.reserve(static_cast<std::size_t>(pairs.size()));
    for (const auto& pair : pairs) {
        headers.push_back(platform::HttpHeader{pair.first.toStdString(), pair.second.toStdString()});
    }
    return headers;
}

platform::ErrorCode classify(QNetworkReply::NetworkError error)
{
    switch (error) {
    case QNetworkReply::TimeoutError:
        return platform::ErrorCode::timeout;
    case QNetworkReply::OperationCanceledError:
        return platform::ErrorCode::cancelled;
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::SslHandshakeFailedError:
        return platform::ErrorCode::unavailable;
    default:
        return platform::ErrorCode::io_failure;
    }
}

} // namespace

QtHttpClient::QtHttpClient() = default;

QtHttpClient::~QtHttpClient() = default;

platform::Result<platform::HttpResponse> QtHttpClient::get(
    const platform::HttpRequest& request, const platform::CancellationToken& cancellation)
{
    QNetworkReply* reply = manager_.get(to_qt_request(request));
    const bool completed = wait_for_reply(*reply, request, cancellation);

    platform::HttpResponse response;
    response.status_code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.headers = collect_headers(*reply);
    const QByteArray body = reply->readAll();
    response.body.assign(body.constData(), static_cast<std::size_t>(body.size()));

    const QNetworkReply::NetworkError error = reply->error();
    const QString error_text = reply->errorString();
    reply->deleteLater();

    // A non-2xx response is not an error for this caller: the status and body are the
    // answer, and the update service maps them. A transport failure is.
    if (!completed) {
        return platform::Result<platform::HttpResponse>::failure(
            platform::ErrorCode::cancelled, "the request was cancelled");
    }
    if (response.status_code == 0 && error != QNetworkReply::NoError) {
        return platform::Result<platform::HttpResponse>::failure(
            classify(error), error_text.toStdString());
    }
    return platform::Result<platform::HttpResponse>(std::move(response));
}

namespace {

/// The streaming side: reads the reply chunk by chunk, waiting on the calling thread's
/// event loop for each one.
class QtByteStream final : public platform::HttpByteStream {
public:
    QtByteStream(QNetworkReply* reply, int status, std::vector<platform::HttpHeader> headers,
        std::optional<std::uint64_t> length, const platform::CancellationToken& cancellation)
        : reply_(reply)
        , status_(status)
        , headers_(std::move(headers))
        , length_(length)
        , cancellation_(cancellation)
    {
    }

    ~QtByteStream() override { close(); }

    [[nodiscard]] int status_code() const noexcept override { return status_; }
    [[nodiscard]] const std::vector<platform::HttpHeader>& headers() const noexcept override
    {
        return headers_;
    }
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept override { return length_; }

    platform::Status read_into(const platform::HttpChunkSink& sink,
        const platform::CancellationToken& cancellation) override
    {
        while (true) {
            if (cancellation.is_cancellation_requested()) {
                close();
                return platform::Status::failure(platform::ErrorCode::cancelled, "the download was cancelled");
            }
            const QByteArray chunk = reply_->read(platform::kHttpCopyBufferBytes);
            if (!chunk.isEmpty()) {
                if (!sink(chunk.constData(), static_cast<std::size_t>(chunk.size()))) {
                    close();
                    return platform::Status::failure(platform::ErrorCode::io_failure,
                        "the download sink stopped the transfer");
                }
                continue;
            }
            if (reply_->isFinished()) {
                break;
            }
            QEventLoop loop;
            QTimer poll;
            poll.setInterval(100);
            QObject::connect(&poll, &QTimer::timeout, &loop, &QEventLoop::quit);
            QObject::connect(reply_, &QNetworkReply::readyRead, &loop, &QEventLoop::quit);
            QObject::connect(reply_, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            poll.start();
            loop.exec();
        }
        const QNetworkReply::NetworkError error = reply_->error();
        if (error != QNetworkReply::NoError && error != QNetworkReply::OperationCanceledError) {
            const QString text = reply_->errorString();
            close();
            return platform::Status::failure(classify(error), text.toStdString());
        }
        close();
        return platform::Status::success();
    }

    void close() override
    {
        if (reply_ != nullptr) {
            reply_->abort();
            reply_->deleteLater();
            reply_ = nullptr;
        }
    }

private:
    QNetworkReply* reply_ = nullptr;
    int status_ = 0;
    std::vector<platform::HttpHeader> headers_;
    std::optional<std::uint64_t> length_;
    platform::CancellationToken cancellation_;
};

} // namespace

platform::Result<std::unique_ptr<platform::HttpByteStream>> QtHttpClient::open(
    const platform::HttpRequest& request, const platform::CancellationToken& cancellation)
{
    QNetworkReply* reply = manager_.get(to_qt_request(request));
    // Only the response head is needed before the body is streamed.
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::errorOccurred, &loop, &QEventLoop::quit);
    QTimer poll;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    poll.start();
    while (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isNull() && !reply->isFinished()) {
        loop.exec();
        if (cancellation.is_cancellation_requested()) {
            break;
        }
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0) {
        const QString text = reply->errorString();
        const auto error = classify(reply->error());
        reply->deleteLater();
        return platform::Result<std::unique_ptr<platform::HttpByteStream>>::failure(error, text.toStdString());
    }
    if (cancellation.is_cancellation_requested()) {
        reply->abort();
        reply->deleteLater();
        return platform::Result<std::unique_ptr<platform::HttpByteStream>>::failure(
            platform::ErrorCode::cancelled, "the download was cancelled before it started");
    }

    std::optional<std::uint64_t> length;
    if (const QVariant header = reply->header(QNetworkRequest::ContentLengthHeader); header.isValid()) {
        length = static_cast<std::uint64_t>(header.toULongLong());
    }
    auto stream = std::make_unique<QtByteStream>(reply, status, collect_headers(*reply), length, cancellation);
    return platform::Result<std::unique_ptr<platform::HttpByteStream>>(std::move(stream));
}

} // namespace voicetyper::app
