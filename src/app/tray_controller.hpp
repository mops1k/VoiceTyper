#pragma once

// Tray presence and the single-instance guard.
//
// The .NET app keeps running with no main window: closing the window hides it
// and the tray icon is the way back. Quitting is an explicit action, because a
// running instance owns the microphone and the global hotkeys.

#include <QIcon>
#include <QObject>

#include "domain/settings.hpp"
#include <QString>

class QAction;
class QMenu;
class QSystemTrayIcon;

namespace voicetyper::app {

class TrayController final : public QObject {
    Q_OBJECT

public:
    explicit TrayController(QSystemTrayIcon& icon, QObject* parent = nullptr);
    ~TrayController() override;

    /// The generated application icon, shared by the tray and the window.
    [[nodiscard]] static QIcon application_icon();

    void set_recording(bool recording);
    /// The tooltip is the tray's only visible status, so it carries the engine
    /// state the main window shows.
    void set_status(const QString& text);

    /// Re-letters the menu and keeps the record entry's wording in step with the state.
    void set_language(domain::AppLanguage language);

    /// The context menu, for tests and for anything that needs to restyle it.
    [[nodiscard]] QMenu* menu() const { return menu_; }

signals:
    void show_requested();
    void record_requested();
    void quit_requested();

private:
    QSystemTrayIcon& icon_;
    QMenu* menu_ = nullptr;
    QAction* show_action_ = nullptr;
    QAction* record_action_ = nullptr;
    domain::AppLanguage language_ = domain::AppLanguage::ru;
    bool recording_ = false;
    QAction* quit_action_ = nullptr;
};

/// True when this process owns the single-instance slot. False means another
/// instance is already running: the caller should ask it to show its window and
/// exit without touching the microphone or the hotkeys.
bool claim_single_instance();

} // namespace voicetyper::app
