// Linux-only contract test for the clickable update notification (VT-UI-606).
//
// What this proves, and what it deliberately does not:
//
//   Proves, without touching the developer's desktop session:
//     * a bus that cannot be reached reports false from show(): the caller keeps its
//       fallback instead of believing a notification was shown;
//     * with a notification service on the bus, the request carries the "default" action
//       together with the label of the action button, plus the title and the body - that
//       action is the whole reason this class exists, because Qt's own tray balloon never
//       passes one for an Information message on Linux;
//     * ActionInvoked is matched by notification id: the id the service returned raises
//       clicked() once, somebody else's id raises nothing.
//
//   The stub never takes the name org.freedesktop.Notifications from a running
//   notification daemon: if the name is owned, the stub half reports a skip. ctest wraps
//   the test in dbus-run-session where that tool exists, so the half always runs there.

#include "platform/linux/linux_notification.hpp"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QEventLoop>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <iostream>
#include <string>

namespace {

using voicetyper::platform::linuxos::DesktopNotification;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

constexpr const char* kService = "org.freedesktop.Notifications";
constexpr const char* kPath = "/org/freedesktop/Notifications";
constexpr const char* kInterface = "org.freedesktop.Notifications";
constexpr uint kNotificationId = 4242;
constexpr uint kForeignNotificationId = 99;

/// A bus address that cannot exist: nothing is ever sent to the real session, which is
/// what makes this half safe to run anywhere.
QDBusConnection isolated_bus()
{
    return QDBusConnection::connectToBus(
        QStringLiteral("unix:path=/tmp/voicetyper-no-such-bus"), QStringLiteral("vt-notification-isolated"));
}

/// The notification daemon, reduced to what this contract needs: answer Notify with an id
/// and remember what was asked, then play a click back when the test says so.
class StubNotificationService final : public QDBusVirtualObject {
public:
    QString introspect(const QString& path) const override
    {
        static_cast<void>(path);        // A minimal node: QDBusInterface::call() for Notify needs no introspection, and the
        // test never asks for more.
        return QStringLiteral(
            "<node><interface name=\"org.freedesktop.Notifications\">"
            "<method name=\"Notify\"><arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/>"
            "<arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"as\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/>"
            "<arg type=\"u\" direction=\"out\"/></method>"
            "<signal name=\"ActionInvoked\"><arg type=\"u\"/><arg type=\"s\"/></signal>"
            "</interface></node>");
    }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override
    {
        if (message.member() != QStringLiteral("Notify")) {
            return false;
        }
        const QList<QVariant> arguments = message.arguments();
        // app_name, replaces_id, app_icon, summary, body, actions, hints, expire_timeout
        app_name = arguments.value(0).toString();
        summary = arguments.value(3).toString();
        body = arguments.value(4).toString();
        actions = arguments.value(5).toStringList();
        // a{sv} arrives as a QDBusArgument, not as a QVariantMap, and has to be unpacked.
        hints.clear();
        const QVariant raw_hints = arguments.value(6);
        if (raw_hints.canConvert<QDBusArgument>()) {
            const QDBusArgument argument = raw_hints.value<QDBusArgument>();
            argument >> hints;
        }
        expire_timeout = arguments.value(7).toInt();
        notify_calls++;
        connection.send(message.createReply(QVariant::fromValue<uint>(kNotificationId)));
        return true;
    }

    /// Plays one ActionInvoked back, exactly as a daemon delivers it to every listener.
    void send_action_invoked(const QDBusConnection& connection, uint id, const QString& action)
    {
        QDBusMessage signal = QDBusMessage::createSignal(
            QString::fromLatin1(kPath), QString::fromLatin1(kInterface), QStringLiteral("ActionInvoked"));
        signal << id << action;
        static_cast<void>(connection.send(signal));
    }

    QString app_name;
    QString summary;
    QString body;
    QStringList actions;
    QVariantMap hints;
    int expire_timeout = -1;
    int notify_calls = 0;
};

/// Delivers bus traffic for a while: the click travels through the daemon and comes back
/// as a signal, so a single processEvents() is not enough.
void pump(std::function<bool()> until, int timeout_ms = 3000)
{
    QEventLoop loop;
    QTimer poll;
    QTimer deadline;
    poll.setInterval(20);
    deadline.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (until()) {
            loop.quit();
        }
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    deadline.start(timeout_ms);
    loop.exec();
}

void test_unreachable_bus_is_reported()
{
    QDBusConnection bus = isolated_bus();
    check(!bus.isConnected(), "the isolated bus address must not connect");
    DesktopNotification notification(bus);
    int clicks = 0;
    QObject::connect(&notification, &DesktopNotification::clicked, [&clicks] { ++clicks; });
    check(!notification.show(QStringLiteral("t"), QStringLiteral("b"), QStringLiteral("Открыть")),
        "show() must report false when there is no notification service");
    notification.handle_action_invoked(kNotificationId, QStringLiteral("default"));
    check(clicks == 0, "a click must not be reported when nothing was shown");
    QDBusConnection::disconnectFromBus(QStringLiteral("vt-notification-isolated"));
}

void test_action_and_click_with_a_stub_service()
{
    const QByteArray address = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    if (address.isEmpty()) {
        std::cout << "linux-notification-contract: no session bus, stub half skipped\n";
        return;
    }
    QDBusConnection stub = QDBusConnection::connectToBus(
        QString::fromLocal8Bit(address), QStringLiteral("vt-notification-stub"));
    if (!stub.isConnected() || !stub.registerService(QString::fromLatin1(kService))) {
        // A real daemon owns the name: never take it from a running session.
        std::cout << "linux-notification-contract: " << kService
                  << " is owned by the session, stub half skipped\n";
        QDBusConnection::disconnectFromBus(QStringLiteral("vt-notification-stub"));
        return;
    }
    StubNotificationService service;
    check(stub.registerVirtualObject(QString::fromLatin1(kPath), &service),
        "the stub notification object must register");

    DesktopNotification notification(QDBusConnection::sessionBus());
    int clicks = 0;
    QObject::connect(&notification, &DesktopNotification::clicked, [&clicks] { ++clicks; });

    check(notification.show(QStringLiteral("Доступно обновление VoiceTyper"),
              QStringLiteral("Доступна версия 3.2.1"), QStringLiteral("Открыть")),
        "show() must report true when the session has a notification service");
    // The Notify call is asynchronous on purpose (a hung daemon must not freeze the caller),
    // so the request reaches the stub through the event loop.
    pump([&service] { return service.notify_calls > 0; });
    check(service.notify_calls == 1, "the notification service must be called exactly once");
    check(service.actions == QStringList({QStringLiteral("default"), QStringLiteral("Открыть")}),
        "the request must carry the default action with its label");
    check(service.summary == QStringLiteral("Доступно обновление VoiceTyper"),
        "the title must reach the service");
    check(service.body == QStringLiteral("Доступна версия 3.2.1"), "the body must reach the service");
    check(service.expire_timeout == 10000, "the expiry the caller asked for must reach the service");
    check(service.hints.value(QStringLiteral("urgency")).toInt() == 1,
        "the notification must be a normal-urgency message");

    // Somebody else's notification: a click on it must not raise our window.
    service.send_action_invoked(stub, kForeignNotificationId, QStringLiteral("default"));
    pump([&clicks] { return true; }, 200);
    check(clicks == 0, "another application's notification must not be reported as our click");

    // Ours: the id the service returned is the one that counts.
    service.send_action_invoked(stub, kNotificationId, QStringLiteral("default"));
    pump([&clicks] { return clicks > 0; });
    check(clicks == 1, "the click on our notification must be reported exactly once");

    static_cast<void>(stub.unregisterObject(QString::fromLatin1(kPath)));
    static_cast<void>(stub.unregisterService(QString::fromLatin1(kService)));
    QDBusConnection::disconnectFromBus(QStringLiteral("vt-notification-stub"));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);

    test_unreachable_bus_is_reported();
    test_action_and_click_with_a_stub_service();

    if (failures != 0) {
        std::cout << "linux-notification-contract: failures=" << failures << '\n';
        return 1;
    }
    std::cout << "linux-notification-contract: OK" << '\n';
    return 0;
}
