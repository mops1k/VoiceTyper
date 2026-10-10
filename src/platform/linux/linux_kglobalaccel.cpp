#include "platform/linux/linux_kglobalaccel.hpp"

#include "platform/linux/linux_keymap.hpp"

#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <functional>
#include <mutex>
#include <utility>

// The keys of setShortcut() are an "ai" on the wire; QVariant needs the metatype
// declared to carry them.
Q_DECLARE_METATYPE(QList<int>)

namespace voicetyper::platform::linuxos {
namespace {

constexpr const char* kService = "org.kde.kglobalaccel";
constexpr const char* kServicePath = "/kglobalaccel";
constexpr const char* kServiceInterface = "org.kde.KGlobalAccel";
constexpr const char* kComponentInterface = "org.kde.kglobalaccel.Component";
constexpr const char* kIntrospectInterface = "org.freedesktop.DBus.Introspectable";

constexpr const char* kComponentUnique = "voicetyper";
constexpr const char* kComponentFriendly = "VoiceTyper";
constexpr const char* kRecordUnique = "record";
constexpr const char* kRecordFriendly = "Record dictation";
constexpr const char* kCancelUnique = "cancel";
constexpr const char* kCancelFriendly = "Cancel dictation";

/// SetShortcutFlag from kglobalaccel_p.h. SetPresent is what makes the shortcut
/// active: without it kglobalaccel stores the keys but keeps the component
/// inactive and emits no signal (verified live 2026-10-11).
constexpr unsigned kSetPresent = 2;
/// The keys come from the user's settings, so the stored value must not win.
constexpr unsigned kNoAutoloading = 4;
constexpr unsigned kSetShortcutFlags = kSetPresent | kNoAutoloading;

QStringList action_id(const char* unique, const char* friendly)
{
    return QStringList{QString::fromLatin1(kComponentUnique), QString::fromLatin1(unique),
        QString::fromLatin1(kComponentFriendly), QString::fromLatin1(friendly)};
}

QString component_path()
{
    return QStringLiteral("/component/") + QString::fromLatin1(kComponentUnique);
}

/// True when the component interface of `path` exposes globalShortcutReleased.
bool introspect_has_release_signal(const QDBusConnection& connection, const QString& path)
{
    QDBusInterface introspectable(QString::fromLatin1(kService), path,
        QString::fromLatin1(kIntrospectInterface), connection);
    if (!introspectable.isValid()) {
        return false;
    }
    const QDBusReply<QString> xml = introspectable.call(QStringLiteral("Introspect"));
    return xml.isValid() && xml.value().contains(QStringLiteral("globalShortcutReleased"));
}

} // namespace

/// Receives the Component signals. QDBusConnection::connect() in Qt 6 takes a
/// slot, not a functor, so the subscription needs a QObject with moc.
class KGlobalAccelSignalReceiver : public QObject {
    Q_OBJECT

public:
    KGlobalAccelSignalReceiver(std::function<void(const QString&)> pressed,
        std::function<void(const QString&)> released)
        : pressed_(std::move(pressed))
        , released_(std::move(released))
    {
    }

public Q_SLOTS:
    void shortcut_pressed(const QString& component, const QString& shortcut, qlonglong timestamp)
    {
        Q_UNUSED(component);
        Q_UNUSED(timestamp);
        if (pressed_) {
            pressed_(shortcut);
        }
    }

    void shortcut_released(const QString& component, const QString& shortcut, qlonglong timestamp)
    {
        Q_UNUSED(component);
        Q_UNUSED(timestamp);
        if (released_) {
            released_(shortcut);
        }
    }

private:
    std::function<void(const QString&)> pressed_;
    std::function<void(const QString&)> released_;
};

struct LinuxKGlobalAccelHotkeys::Impl {
    QDBusConnection connection = QDBusConnection::sessionBus();
    mutable std::mutex mutex;
    platform::HotkeyEventSink sink;
    std::unique_ptr<KGlobalAccelSignalReceiver> receiver;
    std::int32_t record_code = 0;
    std::int32_t cancel_code = 0;
    bool registered = false;
    bool subscribed = false;
    std::string diagnostics;
    /// Observed from the bus (or overridden by the contract test). Defaults to
    /// false so an unobserved service is never advertised as push-to-talk ready.
    bool release_signal = false;
    bool release_signal_known = false;

    [[nodiscard]] bool service_available() const;
    [[nodiscard]] bool observe_release_signal() const;
    void subscribe();
    void emit_action(platform::HotkeyAction action);
    void on_shortcut_pressed(const QString& shortcut);
    void on_shortcut_released(const QString& shortcut);
};

bool LinuxKGlobalAccelHotkeys::Impl::service_available() const
{
    if (!connection.isConnected()) {
        return false;
    }
    QDBusConnectionInterface* bus = connection.interface();
    if (bus == nullptr) {
        return false;
    }
    const QDBusReply<bool> reply = bus->isServiceRegistered(QString::fromLatin1(kService));
    return reply.isValid() && reply.value();
}

bool LinuxKGlobalAccelHotkeys::Impl::observe_release_signal() const
{
    // The signal lives on the Component interface, so any existing component
    // answers the question; our own appears only after doRegister().
    QDBusInterface kglobal(QString::fromLatin1(kService), QString::fromLatin1(kServicePath),
        QString::fromLatin1(kServiceInterface), connection);
    if (!kglobal.isValid()) {
        return false;
    }
    const QDBusReply<QList<QDBusObjectPath>> components = kglobal.call(QStringLiteral("allComponents"));
    if (!components.isValid()) {
        return false;
    }
    for (const QDBusObjectPath& path : components.value()) {
        if (introspect_has_release_signal(connection, path.path())) {
            return true;
        }
    }
    return false;
}

void LinuxKGlobalAccelHotkeys::Impl::subscribe()
{
    if (subscribed) {
        return;
    }
    receiver = std::make_unique<KGlobalAccelSignalReceiver>(
        [this](const QString& shortcut) { on_shortcut_pressed(shortcut); },
        [this](const QString& shortcut) { on_shortcut_released(shortcut); });

    const QString service = QString::fromLatin1(kService);
    const QString path = component_path();
    const QString interface = QString::fromLatin1(kComponentInterface);
    connection.connect(service, path, interface, QStringLiteral("globalShortcutPressed"), receiver.get(),
        SLOT(shortcut_pressed(QString, QString, qlonglong)));
    if (release_signal) {
        connection.connect(service, path, interface, QStringLiteral("globalShortcutReleased"), receiver.get(),
            SLOT(shortcut_released(QString, QString, qlonglong)));
    }
    subscribed = true;
}

void LinuxKGlobalAccelHotkeys::Impl::on_shortcut_pressed(const QString& shortcut)
{
    {
        // A signal that arrives after unregister_all() must not fire: the
        // contract promises no callback once it has returned.
        std::lock_guard<std::mutex> lock(mutex);
        if (!registered) {
            return;
        }
    }
    if (shortcut == QString::fromLatin1(kRecordUnique)) {
        emit_action(platform::HotkeyAction::record_pressed);
    } else if (shortcut == QString::fromLatin1(kCancelUnique)) {
        emit_action(platform::HotkeyAction::cancel_pressed);
    }
}

void LinuxKGlobalAccelHotkeys::Impl::on_shortcut_released(const QString& shortcut)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!registered || !release_signal) {
            return;
        }
    }
    if (shortcut == QString::fromLatin1(kRecordUnique)) {
        emit_action(platform::HotkeyAction::record_released);
    }
}

void LinuxKGlobalAccelHotkeys::Impl::emit_action(platform::HotkeyAction action)
{
    platform::HotkeyEventSink sink;
    {
        std::lock_guard<std::mutex> lock(mutex);
        sink = this->sink;
    }
    if (sink) {
        try {
            sink(action);
        } catch (...) {
            // An exception must never escape into the D-Bus dispatch.
        }
    }
}

LinuxKGlobalAccelHotkeys::LinuxKGlobalAccelHotkeys()
    : LinuxKGlobalAccelHotkeys(QDBusConnection::sessionBus())
{
}

LinuxKGlobalAccelHotkeys::LinuxKGlobalAccelHotkeys(QDBusConnection connection)
    : impl_(std::make_unique<Impl>())
{
    qDBusRegisterMetaType<QList<int>>();
    impl_->connection = std::move(connection);
}

LinuxKGlobalAccelHotkeys::~LinuxKGlobalAccelHotkeys()
{
    static_cast<void>(unregister_all());
}

Status LinuxKGlobalAccelHotkeys::set_event_sink(platform::HotkeyEventSink sink)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sink = std::move(sink);
    return Status::success();
}

Result<platform::HotkeyRegistrationReport> LinuxKGlobalAccelHotkeys::apply_settings(const AppSettings& settings)
{
    platform::HotkeyRegistrationReport report;
    platform::HotkeyRegistration record_entry;
    platform::HotkeyRegistration cancel_entry;
    record_entry.action = platform::HotkeyAction::record_pressed;
    cancel_entry.action = platform::HotkeyAction::cancel_pressed;

    const auto record_gesture = domain::parse_hotkey(settings.record_hotkey);
    const auto cancel_gesture = domain::parse_hotkey(settings.cancel_hotkey);
    if (record_gesture.is_ok()) {
        record_entry.gesture = record_gesture.value();
    } else {
        record_entry.error = record_gesture.error().message();
    }
    if (cancel_gesture.is_ok()) {
        cancel_entry.gesture = cancel_gesture.value();
    } else {
        cancel_entry.error = cancel_gesture.error().message();
    }

    static_cast<void>(unregister_all());

    if (!impl_->service_available()) {
        const std::string reason = "org.kde.kglobalaccel is not available on this session bus";
        if (record_gesture.is_ok()) {
            record_entry.registered = false;
            record_entry.error = reason;
        }
        if (cancel_gesture.is_ok()) {
            cancel_entry.registered = false;
            cancel_entry.error = reason;
        }
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->diagnostics = reason;
        }
        report.record.push_back(std::move(record_entry));
        report.cancel.push_back(std::move(cancel_entry));
        return report;
    }

    QDBusInterface kglobal(QString::fromLatin1(kService), QString::fromLatin1(kServicePath),
        QString::fromLatin1(kServiceInterface), impl_->connection);

    const auto register_one = [&kglobal](const char* unique, const char* friendly,
                                      const HotkeyGesture& gesture,
                                      platform::HotkeyRegistration& entry) {
        if (!gesture.is_valid()) {
            return;
        }
        const std::int32_t key = hotkey_qt_key_code(gesture.key);
        if (key == 0) {
            entry.registered = false;
            entry.error = "the key name is not known to KGlobalAccel: " + gesture.key;
            return;
        }
        const QStringList id = action_id(unique, friendly);
        kglobal.call(QStringLiteral("doRegister"), id);
        const std::int32_t combined = hotkey_qt_modifier_flags(gesture.modifiers) | key;
        const QDBusReply<QList<int>> reply = kglobal.call(QStringLiteral("setShortcut"), id,
            QVariant::fromValue(QList<int>{static_cast<int>(combined)}), kSetShortcutFlags);
        if (!reply.isValid()) {
            entry.registered = false;
            entry.error = reply.error().message().toStdString();
            return;
        }
        entry.registered = true;
        entry.native_key_code = key;
    };

    if (record_gesture.is_ok()) {
        register_one(kRecordUnique, kRecordFriendly, record_gesture.value(), record_entry);
    }
    if (cancel_gesture.is_ok()) {
        register_one(kCancelUnique, kCancelFriendly, cancel_gesture.value(), cancel_entry);
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->record_code = record_entry.registered ? record_entry.native_key_code : 0;
        impl_->cancel_code = cancel_entry.registered ? cancel_entry.native_key_code : 0;
        impl_->registered = record_entry.registered || cancel_entry.registered;
        impl_->release_signal = impl_->observe_release_signal();
        impl_->release_signal_known = true;
        impl_->diagnostics = "registered through org.kde.kglobalaccel ("
            + std::string(impl_->release_signal ? "press+release" : "press only") + ")";
    }
    impl_->subscribe();

    report.record.push_back(std::move(record_entry));
    report.cancel.push_back(std::move(cancel_entry));
    return report;
}

Status LinuxKGlobalAccelHotkeys::unregister_all()
{
    if (impl_->service_available()) {
        QDBusInterface kglobal(QString::fromLatin1(kService), QString::fromLatin1(kServicePath),
            QString::fromLatin1(kServiceInterface), impl_->connection);
        // Best effort: a component that is already gone is not an error.
        static_cast<void>(kglobal.call(QStringLiteral("unRegister"), action_id(kRecordUnique, kRecordFriendly)));
        static_cast<void>(kglobal.call(QStringLiteral("unRegister"), action_id(kCancelUnique, kCancelFriendly)));
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->record_code = 0;
    impl_->cancel_code = 0;
    impl_->registered = false;
    return Status::success();
}

std::int32_t LinuxKGlobalAccelHotkeys::record_key_code() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->record_code;
}

bool LinuxKGlobalAccelHotkeys::available() const
{
    return impl_->service_available();
}

HotkeyCapability LinuxKGlobalAccelHotkeys::capability() const
{
    if (!impl_->service_available()) {
        return HotkeyCapability::none;
    }
    bool release_signal = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->release_signal_known) {
            release_signal = impl_->release_signal;
        } else {
            release_signal = impl_->observe_release_signal();
        }
    }
    return release_signal ? HotkeyCapability::kglobal_accel : HotkeyCapability::kglobal_accel_press_only;
}

std::string LinuxKGlobalAccelHotkeys::diagnostics() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->diagnostics;
}

void LinuxKGlobalAccelHotkeys::handle_shortcut_pressed(std::string_view action_unique)
{
    if (action_unique == kRecordUnique) {
        impl_->emit_action(platform::HotkeyAction::record_pressed);
    } else if (action_unique == kCancelUnique) {
        impl_->emit_action(platform::HotkeyAction::cancel_pressed);
    }
}

void LinuxKGlobalAccelHotkeys::handle_shortcut_released(std::string_view action_unique)
{
    if (action_unique != kRecordUnique) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->release_signal) {
            // Press-only kglobalaccel: a release edge cannot be trusted, and
            // emitting one would leave the state machine waiting for a stop.
            return;
        }
    }
    impl_->emit_action(platform::HotkeyAction::record_released);
}

void LinuxKGlobalAccelHotkeys::set_release_signal_available_for_test(bool available)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->release_signal = available;
    impl_->release_signal_known = true;
}

} // namespace voicetyper::platform::linuxos

#include "linux_kglobalaccel.moc"
