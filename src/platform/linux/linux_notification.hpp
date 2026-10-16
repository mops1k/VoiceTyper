#pragma once

// The desktop notification that reports its own click (VT-UI-606).
//
// Why it exists at all: the tray balloon of QSystemTrayIcon is not enough on Linux.
// QDBusTrayIcon::showMessage() only puts an action into the freedesktop Notify call for
// MessageIcon::Critical (verified in the Qt sources, 6.12: `notificationActions` stays
// empty for Information), and QDBusTrayIcon::actionInvoked() - the only writer of
// messageClicked() - is reached solely through that action. With no action, clicking the
// notification body is a plain dismissal, so an "an update is available" balloon could
// never take the user to the About page. This class sends the notification itself with the
// "default" action and matches ActionInvoked against the id the service returned.
//
// A session without a notification service (no org.freedesktop.Notifications on the bus)
// answers false from show(); the composition then keeps the tray balloon, whose click is
// simply not observable there.

#include <QDBusConnection>
#include <QObject>
#include <QString>

namespace voicetyper::platform::linuxos {

class DesktopNotification final : public QObject {
    Q_OBJECT

public:
    explicit DesktopNotification(QObject* parent = nullptr);
    /// Uses an explicit connection. The contract test passes a bus that does not exist,
    /// so nothing is ever sent to the developer's own session.
    explicit DesktopNotification(QDBusConnection connection, QObject* parent = nullptr);
    ~DesktopNotification() override;

    DesktopNotification(const DesktopNotification&) = delete;
    DesktopNotification& operator=(const DesktopNotification&) = delete;

    /// Sends the notification with the "default" action labelled `action_label`. True when
    /// the session's notification service accepted it; false when there is none, so the
    /// caller can fall back instead of staying silent.
    bool show(const QString& title, const QString& body, const QString& action_label, int msecs = 10000);

signals:
    /// The user clicked the notification body or its action button. Emitted only for the
    /// notification this object sent: another application's ActionInvoked belongs to it.
    void clicked();

public slots:
    /// Feeds one ActionInvoked signal, exactly as D-Bus delivers it. The signature is the
    /// wire one (`uint`, not std::uint32_t), because the connection is made by name; the
    /// method is public so the contract test can prove the id matching without a real
    /// notification daemon.
    void handle_action_invoked(uint id, const QString& action);

private:
    /// Subscribes to ActionInvoked on the connection. Called once, on construction.
    void subscribe();

    QDBusConnection connection_;
    /// The id the notification service returned; ActionInvoked carries it.
    std::uint32_t notification_id_ = 0;
};

} // namespace voicetyper::platform::linuxos
