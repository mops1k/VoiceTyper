#include "platform/linux/linux_notification.hpp"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <utility>

namespace voicetyper::platform::linuxos {
namespace {

constexpr const char* kService = "org.freedesktop.Notifications";
constexpr const char* kServicePath = "/org/freedesktop/Notifications";
constexpr const char* kServiceInterface = "org.freedesktop.Notifications";
constexpr const char* kActionInvoked = "ActionInvoked";

/// The freedesktop spelling of "the body click invokes this action". Without it a click is
/// a dismissal and no ActionInvoked is emitted (that is the whole reason this class exists).
const QString kDefaultAction = QStringLiteral("default");

/// The action label the daemon shows next to the notification body.
QStringList actions_for(const QString& action_label)
{
    return QStringList{kDefaultAction, action_label};
}

} // namespace

DesktopNotification::DesktopNotification(QObject* parent)
    : QObject(parent)
    , connection_(QDBusConnection::sessionBus())
{
    subscribe();
}

DesktopNotification::DesktopNotification(QDBusConnection connection, QObject* parent)
    : QObject(parent)
    , connection_(std::move(connection))
{
    subscribe();
}

DesktopNotification::~DesktopNotification() = default;

void DesktopNotification::subscribe()
{
    if (!connection_.isConnected()) {
        return;
    }
    // A string-based connect: QDBusConnection::connect() in Qt 6 takes a slot, not a
    // functor (the same constraint the KGlobalAccel fallback works around).
    static_cast<void>(connection_.connect(QString::fromLatin1(kService), QString::fromLatin1(kServicePath),
        QString::fromLatin1(kServiceInterface), QString::fromLatin1(kActionInvoked), this,
        SLOT(handle_action_invoked(uint, QString))));
}

bool DesktopNotification::show(const QString& title, const QString& body, const QString& action_label,
    int msecs)
{
    if (!connection_.isConnected()) {
        return false;
    }
    // The fallback decision needs a synchronous answer, and NameHasOwner goes to the bus
    // daemon itself - a local socket that is always there. The call below does NOT: a hung
    // notification daemon must never freeze the interface thread, which is where this runs.
    QDBusConnectionInterface* bus = connection_.interface();
    if (bus == nullptr || !bus->isServiceRegistered(QString::fromLatin1(kService))) {
        return false;
    }
    // A hand-built message rather than QDBusInterface: the convenience class introspects the
    // remote object and refuses to call an interface that introspection did not list, while a
    // notification daemon is free to answer introspection however it likes.
    QDBusMessage call = QDBusMessage::createMethodCall(QString::fromLatin1(kService),
        QString::fromLatin1(kServicePath), QString::fromLatin1(kServiceInterface),
        QStringLiteral("Notify"));
    QVariantMap hints;
    // urgency: 1 = normal. The notification carries no image, so the daemon picks the icon.
    hints.insert(QStringLiteral("urgency"), QVariant(1));
    // app_name, replaces_id, app_icon, summary, body, actions, hints, expire_timeout
    call << QCoreApplication::applicationName() << QVariant::fromValue<uint>(0) << QString() << title
         << body << actions_for(action_label) << hints << msecs;
    auto* watcher = new QDBusPendingCallWatcher(connection_.asyncCall(call), this);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this](QDBusPendingCallWatcher* finished) {
            const QDBusPendingReply<uint> reply = *finished;
            finished->deleteLater();
            if (!reply.isValid()) {
                return;
            }
            // 0 means "no notification was created" in the specification, so no ActionInvoked
            // will ever name it.
            notification_id_ = reply.value();
        });
    return true;
}

void DesktopNotification::handle_action_invoked(uint id, const QString& action)
{
    static_cast<void>(action);
    if (notification_id_ == 0 || id != notification_id_) {
        // Somebody else's notification: the session bus carries every application's
        // ActionInvoked, and a click on one of them must not raise our window.
        return;
    }
    emit clicked();
}

} // namespace voicetyper::platform::linuxos
