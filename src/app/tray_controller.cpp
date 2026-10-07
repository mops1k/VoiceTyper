// Tray icon and the single-instance guard.
//
// Two small Windows/Qt behaviours the .NET app has and that are easy to lose:
//   * the tray icon is the app's real presence - the main window may be closed
//     while the app keeps running, and quitting is an explicit tray action;
//   * a second launch focuses/raises the existing window instead of starting a
//     second recorder that would fight over the microphone and the clipboard.

#include "app/tray_controller.hpp"
#include "app/ui_text.hpp"

#include <QApplication>
#include <QAction>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QSystemTrayIcon>

#include <memory>

#if defined(_WIN32)
// Before any namespace: including windows.h inside voicetyper::app declared the Win32
// API inside it, which is why CreateMutexW was not found.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace voicetyper::app {
namespace {

constexpr auto kSingleInstanceChannel = "VoiceTyper-single-instance";

/// Raised in this process when another launch asks the window to come forward. The window is
/// created after the single-instance claim, so such a request can arrive before there is
/// anything to raise; it is remembered and taken once the window exists.
std::function<void()> g_activation_hook;
bool g_activation_pending = false;

void request_activation()
{
    if (g_activation_hook) {
        g_activation_hook();
        return;
    }
    g_activation_pending = true;
}

/// The product icon, embedded as a Qt resource so no file has to be deployed
/// next to the executable. Alexander supplied it on 2026-10-06
/// (assets/voiceTyper.png); the drawn microphone below is the fallback for a
/// build that somehow lost the resource, because an invisible tray icon is how a
/// user loses a running application.
QIcon shipped_application_icon()
{
    QIcon icon(QStringLiteral(":/assets/voiceTyper.png"));
    if (!icon.isNull()) {
        return icon;
    }
    return QIcon();
}

/// The drawn fallback icon: a stylised microphone, the app's one function and
/// recognisable at 16 px.
QIcon draw_application_icon()
{
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(QColor(0x2D, 0x6C, 0xDF));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(6, 6, 52, 52, 14, 14);
    painter.setPen(QPen(QColor(0xFF, 0xFF, 0xFF), 7, Qt::SolidLine, Qt::RoundCap));
    // A stylised microphone: the app's one function, recognisable at 16 px.
    painter.drawRoundedRect(27, 16, 10, 20, 5, 5);
    painter.drawArc(20, 28, 24, 24, 0, 180 * 16);
    painter.drawLine(32, 48, 32, 54);
    painter.end();
    return QIcon(pixmap);
}

} // namespace

QIcon TrayController::application_icon()
{
    const QIcon shipped = shipped_application_icon();
    return shipped.isNull() ? draw_application_icon() : shipped;
}

TrayController::TrayController(QSystemTrayIcon& icon, QObject* parent)
    : QObject(parent)
    , icon_(icon)
{
    menu_ = new QMenu();
    // Текст пунктов упирался в правый край, а вертикальные отступы были тесными: у
    // пунктов теперь запас справа и по высоте (замечание Александра, 06.10.2026).
    // Только запас справа: текст упирался в правый край, а всё остальное в меню
    // устраивало (уточнение Александра, 06.10.2026 - вертикальные отступы он просил
    // не в меню трея, а в списке разделов внутри окна настроек).
    menu_->setStyleSheet(QStringLiteral("QMenu::item { padding-right: 26px; }"));
    show_action_ = menu_->addAction(ui_text(UiKey::k80, language_));
    QObject::connect(show_action_, &QAction::triggered, this, &TrayController::show_requested);
    menu_->addSeparator();
    record_action_ = menu_->addAction(ui_text(UiKey::k81, language_));
    QObject::connect(record_action_, &QAction::triggered, this, &TrayController::record_requested);
    menu_->addSeparator();
    quit_action_ = menu_->addAction(ui_text(UiKey::k83, language_));
    QObject::connect(quit_action_, &QAction::triggered, this, &TrayController::quit_requested);
    icon_.setContextMenu(menu_);
    QObject::connect(&icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            emit show_requested();
        }
    });
}

void TrayController::set_language(domain::AppLanguage language)
{
    language_ = language;
    show_action_->setText(ui_text(UiKey::k80, language_));
    record_action_->setText(ui_text(recording_ ? UiKey::k82 : UiKey::k81, language_));
    quit_action_->setText(ui_text(UiKey::k83, language_));
}

void TrayController::set_recording(bool recording)
{
    recording_ = recording;
    record_action_->setText(ui_text(recording_ ? UiKey::k82 : UiKey::k81, language_));
    record_action_->setEnabled(true);
}

void TrayController::set_status(const QString& text)
{
    icon_.setToolTip(text);
}

TrayController::~TrayController()
{
    delete menu_;
}

#if defined(_WIN32)
namespace {

/// The name the Inno Setup installer waits on (`AppMutex` in installer.iss). It is how
/// "close the running application before replacing its files" works, exactly as it did
/// for the .NET build, which held a mutex with this very name: without it the installer
/// cannot tell that this application is running and the update stalls on a locked exe.
constexpr wchar_t kInstallerMutexName[] = L"Global\\VoiceTyper_SingleInstance";
HANDLE g_installer_mutex = nullptr;

} // namespace
#endif

bool claim_single_instance()
{
#if defined(_WIN32)
    // Created once and held for the life of the process; the handle is deliberately not
    // closed, because the mutex must disappear exactly when the process does.
    if (g_installer_mutex == nullptr) {
        // CreateMutexW leaves the last error untouched when it creates the object, so the
        // value has to be cleared first: otherwise a stale ERROR_ALREADY_EXISTS from an
        // unrelated earlier call would make the very first launch believe somebody owns the
        // name (exactly what the Windows test run showed).
        ::SetLastError(ERROR_SUCCESS);
        g_installer_mutex = ::CreateMutexW(nullptr, FALSE, kInstallerMutexName);
        if (g_installer_mutex != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS) {
            // Somebody already owns the name: a live instance, or one that is starting this
            // very moment. The kernel decides who created it first, so two simultaneous
            // launches cannot both continue - and the channel below is deliberately left
            // untouched, because touching it is what could steal a running instance.
            return false;
        }
    }
#endif
    // Ask whether somebody is alive before touching the channel: removing the socket first is
    // exactly what steals a running instance on platforms where that socket is a real file.
    auto* probe = new QLocalSocket;
    probe->connectToServer(QString::fromLatin1(kSingleInstanceChannel));
    const bool alive = probe->waitForConnected(200);
    probe->abort();
    delete probe;
    if (alive) {
        return false;
    }
    // Nobody answered, so any socket left behind belongs to a crashed run and can go.
    QLocalServer::removeServer(QString::fromLatin1(kSingleInstanceChannel));
    auto* server = new QLocalServer(qApp);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(QString::fromLatin1(kSingleInstanceChannel))) {
        delete server;
        // We own the name and still cannot listen, so the channel is unusable. Refusing to
        // start is the safe answer: a second recorder would fight the first one for the
        // microphone and the global hotkeys, which is worse than not starting at all.
        qWarning() << "single instance: cannot listen on the instance channel";
        return false;
    }
    QObject::connect(server, &QLocalServer::newConnection, [server] {
        auto* incoming = server->nextPendingConnection();
        if (incoming == nullptr) {
            return;
        }
        incoming->waitForReadyRead(200);
        const QByteArray request = incoming->readAll();
        incoming->disconnectFromServer();
        incoming->deleteLater();
        // A launch connecting here means "show yourself": that is the whole point of the
        // channel, and a reply written back to the caller would be read by nobody.
        if (request.startsWith("show")) {
            request_activation();
        }
    });
    return true;
}

/// Tells the instance that owns the channel to come forward.
bool notify_running_instance()
{
    QLocalSocket socket;
    socket.connectToServer(QString::fromLatin1(kSingleInstanceChannel));
    if (!socket.waitForConnected(500)) {
        return false;
    }
    socket.write("show", 4);
    socket.flush();
    static_cast<void>(socket.waitForBytesWritten(500));
    socket.disconnectFromServer();
    return true;
}

void set_activation_hook(std::function<void()> hook)
{
    g_activation_hook = std::move(hook);
}

bool take_pending_activation()
{
    const bool pending = g_activation_pending;
    g_activation_pending = false;
    return pending;
}

} // namespace voicetyper::app
