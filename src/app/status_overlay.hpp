#pragma once

// Qt implementation of the frameless status overlay contract
// (src/platform/api/status_overlay.hpp).
//
// What it is: a small always-on-top pill at the bottom center of the display the
// user last interacted with, showing "Захват" while the microphone is open and
// "Распознавание" while the model is working. It is the port of
// VoiceTyper.App/Overlay/StatusOverlayWindow.* (the .NET build showed exactly
// these two states and hid the overlay on idle).
//
// Decisions taken here, all of them observable and covered by
// tests/ui/ui_status_overlay_test.cpp:
//   * The pill is a Qt::Tool window with Qt::WindowDoesNotAcceptFocus and
//     Qt::WA_TransparentForMouseEvents: it is not in the taskbar, it never
//     activates and it never swallows a keystroke or a click meant for the
//     window the user is typing into.
//   * The pulse is a QTimer with the frozen kOverlayPulsePeriod (350 ms) that
//     toggles the indicator between opacity 1.0 and 0.35, reproducing the .NET
//     DispatcherTimer pulse (one full on/off cycle therefore takes 700 ms).
//   * An error NEVER reaches the pill: a missing model, a lost device or a refused
//     engine goes to the window's status line and the log, and the overlay hides.
//     Keeping the reason on screen made the pill sit on top of the window the user
//     was typing into and stay there (reported from the running build, 2026-10-11).
//     OverlayState::error stays in the port contract for parity, but renders as
//     "hidden" - the state becomes idle because nothing is shown.
//   * Geometry is computed in Qt logical pixels from the screen's
//     availableGeometry, so a 150% DPI monitor gets the same 26 px gap the .NET
//     code produced by scaling physical pixels by hand.
//   * destroy() is terminal: it is the shutdown path, and a queued worker event
//     arriving afterwards must not resurrect a window while the application is
//     exiting. hide() is the reusable one and is what every dictation uses.
//
// Localization: the two state labels are Russian literals next to the rest of
// the C++ UI. Routing them through the language setting is the localization task
// (migration plan, Phase E "Перенести localization, theme и log view"), and the
// constants below are the single place that task has to touch.

#include "app/ui_text.hpp"
#include "domain/settings.hpp"
#include "platform/api/status_overlay.hpp"

#include <atomic>
#include <string>
#include <string_view>

#include <QObject>

class QGraphicsOpacityEffect;
class QLabel;
class QTimer;
class QWidget;

namespace voicetyper::app {

/// Vertical gap between the pill and the bottom of the working area, in logical
/// pixels. Frozen from the .NET overlay (26 px at 100% scaling).
inline constexpr int kOverlayBottomGapPx = 26;
/// Indicator dot diameter, logical pixels (.NET Ellipse 11x11).
inline constexpr int kOverlayDotSizePx = 11;
/// A detail text is caller-provided, so it is elided before it can stretch the
/// pill across the screen.
inline constexpr int kOverlayMaxTextWidthPx = 560;

/// The .NET overlay's Russian texts, kept as the parity reference. What is shown comes
/// from the string table (ui_text), so the pill follows the interface language.
inline constexpr std::string_view kOverlayRecordingText = "Захват";
inline constexpr std::string_view kOverlayProcessingText = "Распознавание";
inline constexpr std::string_view kOverlayErrorText = "Ошибка";

inline constexpr std::string_view kOverlayRecordingAccent = "#4C8BF5";
inline constexpr std::string_view kOverlayProcessingAccent = "#F5A623";
/// New in the port: the .NET overlay had no error state at all. The contract
/// (platform/api/status_overlay.hpp) requires one, so it needs a colour.
inline constexpr std::string_view kOverlayErrorAccent = "#E5484D";

inline constexpr std::string_view kOverlayTextColor = "#F4F4F5";
inline constexpr std::string_view kOverlayBackgroundColor = "rgba(24, 24, 27, 235)";

/// True when the overlay needs a layer-shell surface for its rules to hold.
///
/// On Wayland an ordinary xdg-toplevel lands in the window list and can be
/// activated even with Qt::Tool / Qt::WindowDoesNotAcceptFocus - reported from
/// the running build on 2026-10-11. A layer-shell surface in the overlay layer
/// has no window-list entry and never takes keyboard focus, so Wayland needs it
/// and every other platform keeps the plain window.
[[nodiscard]] bool overlay_needs_layer_shell(std::string_view platform_name);

/// True when this build links LayerShellQt, so the Wayland path can actually
/// promote the host to a layer-shell surface. False means the overlay falls back
/// to the ordinary window on every platform.
[[nodiscard]] constexpr bool layer_shell_build_available() noexcept
{
#if defined(VOICETYPER_HAS_LAYER_SHELL)
    return true;
#else
    return false;
#endif
}

/// The pill window.
///
/// Thread affinity: create/show/set_state/hide/destroy belong to the UI thread,
/// exactly as the contract requires. Worker threads (recording, engine, hotkeys)
/// publish through the thread-safe post_state(), which queues onto the UI thread
/// and is dropped once the overlay is destroyed.
class QtStatusOverlay final : public QObject, public platform::StatusOverlay {
    Q_OBJECT

public:
    explicit QtStatusOverlay(QObject* parent = nullptr);
    ~QtStatusOverlay() override;

    QtStatusOverlay(const QtStatusOverlay&) = delete;
    QtStatusOverlay& operator=(const QtStatusOverlay&) = delete;

    /// UI thread only. Failure codes: invalid_state (wrong thread, or after
    /// destroy), unavailable (no QGuiApplication).
    platform::Status create() override;

    /// Re-letters the pill in the given interface language, keeping the state that is
    /// currently shown.
    void set_language(domain::AppLanguage language);

    /// Recolours the pill for the current application theme.
    ///
    /// The pill sets its own colours on purpose: the window's global stylesheet paints
    /// every QWidget with the page background, and in the light theme that turned the
    /// pill white while its text stayed the light grey of the dark theme - unreadable
    /// "Захват" on white (Alexander, 2026-10-06). A widget's own stylesheet wins over the
    /// application's, so the overlay is stated explicitly in both themes.
    void set_theme(domain::AppTheme theme);
    platform::Status show(platform::OverlayState state, std::string_view detail = {}) override;
    platform::Status set_state(platform::OverlayState state, std::string_view detail = {}) override;
    platform::Status hide() override;
    [[nodiscard]] platform::OverlayState current_state() const noexcept override;
    platform::Status destroy() override;

    /// Thread-safe: queues the state onto the UI thread. This is what the
    /// recording state machine and the engine call, because their callbacks run
    /// on a worker thread. A no-op after destroy().
    void post_state(platform::OverlayState state, std::string detail = {});

    /// The pill widget, or nullptr before create() and after destroy(). For the
    /// UI test and the smoke harness; production code has no reason to touch it.
    [[nodiscard]] QWidget* widget() const noexcept { return pill_; }

    /// True once destroy() has run. For tests and shutdown logging.
    [[nodiscard]] bool is_destroyed() const noexcept { return shutdown_.load(); }

    /// True when the host became a layer-shell surface (Wayland with LayerShellQt
    /// present). False on every other platform, where the ordinary window flags
    /// are the whole mechanism.
    [[nodiscard]] bool layer_shell_active() const noexcept { return layer_shell_active_; }

private:
    /// The colours of the current theme: dark pill with light text, or the reverse.
    void apply_colours();

    QString background_colour_;
    QString text_colour_;
    QString dot_colour_;

public:
private:
    void build();
    void apply_state(platform::OverlayState state, const std::string& detail);
    void set_pill_text(const std::string& text);
    void set_accent(std::string_view accent);
    void reposition();
    void start_pulse();
    void stop_pulse();
    void set_pulse_opacity(double value);

    /// The full-screen, click-through host window the compositor places at the
    /// screen origin; the pill lives inside it, because on Wayland a client
    /// cannot position its own top-level windows.
    QWidget* host_ = nullptr;
    QWidget* pill_ = nullptr;
    QWidget* dot_ = nullptr;
    QLabel* status_text_ = nullptr;
    QGraphicsOpacityEffect* dot_opacity_ = nullptr;
    QTimer* pulse_timer_ = nullptr;
    bool pulse_phase_ = true;
    /// Written on the UI thread by destroy() and read by worker threads in
    /// post_state(), so it is atomic rather than a plain bool.
    std::atomic<bool> shutdown_{false};
    /// True when the host is a layer-shell surface; see layer_shell_active().
    bool layer_shell_active_ = false;
    platform::OverlayState state_ = platform::OverlayState::idle;
    /// The state shown now, with its detail, so a language change can repeat it.
    std::string detail_;
    domain::AppLanguage language_ = domain::AppLanguage::ru;
};

} // namespace voicetyper::app
