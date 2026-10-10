#include "platform/linux/linux_clipboard.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QSemaphore>
#include <QString>
#include <QThread>

#include <optional>
#include <utility>

namespace voicetyper::platform::linuxos {
namespace {

/// The GUI application this process is running under, or nullptr when the
/// backend is used without one (a GUI-off build, a contract test that never
/// created an application).
QGuiApplication* gui_application()
{
    return qobject_cast<QGuiApplication*>(QGuiApplication::instance());
}

} // namespace

LinuxClipboard::LinuxClipboard() = default;

LinuxClipboard::~LinuxClipboard() = default;

Status LinuxClipboard::set_text(std::string_view text, const CancellationToken& cancellation)
{
    if (cancellation.is_cancellation_requested()) {
        return Status::failure(ErrorCode::cancelled, "clipboard write cancelled");
    }
    QGuiApplication* application = gui_application();
    if (application == nullptr) {
        // The .NET gap this backend exists to close: no clipboard owner is an
        // explicit failure, never a silent success.
        return Status::failure(ErrorCode::unavailable,
            "no QGuiApplication: there is no clipboard to write to");
    }
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return Status::failure(ErrorCode::unavailable, "the GUI application has no clipboard");
    }

    const QString owned = QString::fromUtf8(text.data(), static_cast<int>(text.size()));
    bool written = false;
    const auto write = [clipboard, &owned, &written] {
        clipboard->setText(owned);
        written = true;
    };

    // A blocking queued call from the GUI thread itself would deadlock, so the
    // same-thread case runs directly.
    if (QThread::currentThread() == application->thread()) {
        write();
    } else {
        QSemaphore done;
        const bool invoked = QMetaObject::invokeMethod(application, [&write, &done] {
            write();
            done.release();
        }, Qt::QueuedConnection);
        if (!invoked) {
            return Status::failure(ErrorCode::unavailable, "the GUI thread refused the clipboard call");
        }
        if (!done.tryAcquire(1, kClipboardMarshalTimeout.count())) {
            return Status::failure(ErrorCode::unavailable,
                "the GUI thread did not answer the clipboard call within "
                    + std::to_string(kClipboardMarshalTimeout.count()) + " ms");
        }
    }

    if (!written) {
        return Status::failure(ErrorCode::permission_denied, "the compositor refused the clipboard write");
    }
    return Status::success();
}

Result<std::optional<std::string>> LinuxClipboard::get_text() const
{
    QGuiApplication* application = gui_application();
    if (application == nullptr) {
        return Result<std::optional<std::string>>::failure(ErrorCode::unavailable,
            "no QGuiApplication: there is no clipboard to read");
    }
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return Result<std::optional<std::string>>::failure(ErrorCode::unavailable,
            "the GUI application has no clipboard");
    }

    std::optional<std::string> value;
    bool read = false;
    const auto read_clipboard = [clipboard, &value, &read] {
        const QString text = clipboard->text();
        // A clipboard holding an empty string is "no text", which is the same
        // thing to every caller here - exactly how the Windows backend reads it.
        if (!text.isNull() && !text.isEmpty()) {
            const QByteArray utf8 = text.toUtf8();
            value = std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
        }
        read = true;
    };

    if (QThread::currentThread() == application->thread()) {
        read_clipboard();
    } else {
        QSemaphore done;
        const bool invoked = QMetaObject::invokeMethod(application, [&read_clipboard, &done] {
            read_clipboard();
            done.release();
        }, Qt::QueuedConnection);
        if (!invoked) {
            return Result<std::optional<std::string>>::failure(ErrorCode::unavailable,
                "the GUI thread refused the clipboard call");
        }
        if (!done.tryAcquire(1, kClipboardMarshalTimeout.count())) {
            return Result<std::optional<std::string>>::failure(ErrorCode::unavailable,
                "the GUI thread did not answer the clipboard call within "
                    + std::to_string(kClipboardMarshalTimeout.count()) + " ms");
        }
    }

    if (!read) {
        return Result<std::optional<std::string>>::failure(ErrorCode::permission_denied,
            "the compositor refused the clipboard read");
    }
    return value;
}

bool LinuxClipboard::has_text() const
{
    const auto text = get_text();
    return text.is_ok() && text.value().has_value() && !text.value()->empty();
}

} // namespace voicetyper::platform::linuxos
