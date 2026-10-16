#pragma once

#include <cstdint>
#include <functional>

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

namespace voicetyper::ui {

/// What the tray tooltip says when the pointer rests on the icon. A state, not a
/// ready-made string: the tooltip is re-lettered on every language change, and the
/// composition polls the machine and the engine far more often than they change.
enum class TrayStatus : std::uint8_t {
    /// Nothing is running and the engine can transcribe.
    ready = 0,
    /// The microphone is open (started by the button, the hotkey or the gamepad).
    recording = 1,
    /// The engine is turning the captured audio into text.
    processing = 2,
    /// No engine yet: missing model, still loading, or a failed start.
    engine_not_ready = 3,
};

class TrayController final : public QObject {
    Q_OBJECT

public:
    explicit TrayController(QSystemTrayIcon& icon, QObject* parent = nullptr);
    ~TrayController() override;

    /// The generated application icon, shared by the tray and the window.
    [[nodiscard]] static QIcon application_icon();

    void set_recording(bool recording);
    /// The tooltip is the tray's only visible status, so it carries the engine
    /// state the main window shows. Called by the composition's status poll; the
    /// text itself is built here so a language change can rebuild it.
    void set_status(TrayStatus status);

    /// Re-letters the menu and keeps the record entry's wording in step with the state.
    void set_language(domain::AppLanguage language);

    /// The context menu, for tests and for anything that needs to restyle it.
    [[nodiscard]] QMenu* menu() const { return menu_; }

signals:
    void show_requested();
    void record_requested();
    void quit_requested();

private:
    /// Rebuilds the tooltip from status_ and the current language. The one place the
    /// two are combined, so set_status() and set_language() can never disagree.
    void refresh_tooltip();

    QSystemTrayIcon& icon_;
    QMenu* menu_ = nullptr;
    QAction* show_action_ = nullptr;
    QAction* record_action_ = nullptr;
    domain::AppLanguage language_ = domain::AppLanguage::ru;
    bool recording_ = false;
    /// The engine is not ready until the composition's poll says otherwise: a tooltip
    /// promising readiness while the model still loads is the one thing it must not do.
    TrayStatus status_ = TrayStatus::engine_not_ready;
    QAction* quit_action_ = nullptr;
};

/// True when this process owns the single-instance slot. False means another
/// instance is already running: the caller should ask it to show its window and
/// exit without touching the microphone or the hotkeys.
bool claim_single_instance();

/// Tells the instance that owns the single-instance channel to come forward. True when it
/// answered, in which case the caller exits instead of starting a second recorder.
bool notify_running_instance();

/// Called when another launch asks this instance to show itself. The window is created after
/// the claim, so a request arriving earlier is remembered for take_pending_activation().
void set_activation_hook(std::function<void()> hook);

/// True once, when a launch asked to show before the window existed.
[[nodiscard]] bool take_pending_activation();

} // namespace voicetyper::ui
