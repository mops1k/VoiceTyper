#pragma once

// The platform::HttpClient the application actually uses: Qt Network.
//
// Why Qt and not WinHTTP: Qt is already linked (the deploy ships Qt6Network.dll), it is
// the same code on Windows and Linux, and the update flow is the only traffic this
// application makes besides model downloads. The frozen protocol rules - the 30 minute
// timeout, the User-Agent, the GitHub Accept header - live in platform/api/http.hpp and
// are applied by the callers of this client (core/support/update_service.cpp), so this
// class only performs the transfer.
//
// Threading: every call blocks until the transfer finishes, waiting on the Qt event
// loop of the calling thread. Callers must therefore run it on a worker that has an
// event loop (the composition does), never on the UI thread, or the window freezes for
// the duration of a 60 MB download.

#include "platform/api/http.hpp"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>

#include <memory>

namespace voicetyper::app {

class QtHttpClient final : public platform::HttpClient {
public:
    QtHttpClient();
    ~QtHttpClient() override;

    QtHttpClient(const QtHttpClient&) = delete;
    QtHttpClient& operator=(const QtHttpClient&) = delete;

    [[nodiscard]] platform::Result<platform::HttpResponse> get(
        const platform::HttpRequest& request, const platform::CancellationToken& cancellation) override;

    [[nodiscard]] platform::Result<std::unique_ptr<platform::HttpByteStream>> open(
        const platform::HttpRequest& request, const platform::CancellationToken& cancellation) override;

private:
    QNetworkAccessManager manager_;
};

} // namespace voicetyper::app
