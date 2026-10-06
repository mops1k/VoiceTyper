#pragma once

// The settings window. It is a thin Qt view over SettingsPresenter: every edit
// goes through presenter.update(...), which applies the change immediately and
// restarts the 700 ms autosave debounce. Services that are not registered (no
// hotkey backend, no microphone, no engine) leave their control disabled with a
// visible reason, so the window still opens on a machine that is not fully
// assembled — that is the difference between "the app starts" and "the app is
// ready to dictate".

#include "app/settings_presenter.hpp"
#include "app/toggle_switch.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QMainWindow>
#include <QPoint>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QSlider;
class QListWidget;
class QStackedWidget;
class QTextEdit;
class QPushButton;
class QPixmap;
class QTimer;
class QComboBox;
class QLineEdit;
class QDoubleSpinBox;
class QSpinBox;

namespace voicetyper::app {

class MainWindow;

/// Thread-safe hand-off from a service thread to the UI thread.
///
/// The recording, capture and engine threads publish events, and a QWidget may
/// only be touched from the UI thread, so every message crosses through here.
/// The target is set on construction and cleared on destruction, and post() is
/// safe to call from any thread at any time, including after the window is gone.
class StatusChannel final {
public:
    explicit StatusChannel(MainWindow* target = nullptr);
    ~StatusChannel();

    StatusChannel(const StatusChannel&) = delete;
    StatusChannel& operator=(const StatusChannel&) = delete;

    /// Shows `text` in the window's status area, asynchronously. A no-op once the
    /// window is gone.
    void post(const QString& text);

    /// Points the channel at a window that did not exist when it was created.
    void adopt(MainWindow* target) noexcept { target_.store(target); }

private:
    std::atomic<MainWindow*> target_;
};

/// Optional services the window shows. An empty callback means "not available
/// here"; the window disables the control instead of pretending.
struct WindowServices {
    /// Human-readable engine/model state for the status line.
    std::function<QString()> engine_status;
    /// Registered record hotkey, empty when registration failed.
    std::function<QString()> record_hotkey_state;
    /// Available input devices as (id, display name).
    std::function<std::vector<std::pair<std::string, std::string>>()> microphones;
    /// Start/stop a dictation through the recording state machine.
    std::function<void()> start_recording;
    std::function<void()> stop_recording;
    /// Starts a hotkey capture for the settings dialog: the user presses the
    /// combination and `report` receives its text, or nothing plus a reason when
    /// the capture was cancelled or failed. Implemented by the composition, which
    /// suspends paste injection for the duration; empty when the platform has no
    /// capture hook, and then the buttons stay disabled with a reason.
    std::function<void(std::function<void(std::optional<std::string>, QString)>)> capture_hotkey;
    /// Whether the model for an engine and size index is already on disk, and a
    /// request to delete it (the composition asks for confirmation first). Empty on a
    /// platform that cannot know, and then the rows say so.
    std::function<bool(bool whisper, int size_index)> model_is_downloaded;
    std::function<bool(bool whisper, int size_index)> model_delete;
    /// The recording endpoint's own input level in percent, and a request to change
    /// it. Unset on a platform without a level control, and then the slider is
    /// disabled and says why.
    std::function<int()> microphone_level_get;
    std::function<bool(int percent)> microphone_level_set;
    /// A short capture for the "test the microphone" button: whether sound arrived,
    /// its peak, and a detail line for the log. The callback runs on the UI thread.
    std::function<void(std::function<void(bool heard, double peak, QString detail)>)> microphone_probe;
    /// The running version, for the About page.
    std::function<QString()> application_version;
    /// Asks the release feed. The reply arrives on the UI thread; empty services mean
    /// the platform has no updater and the controls are disabled with a reason.
    std::function<void(std::function<void(bool available, QString version, QString notes, QString error)>)>
        update_check;
    /// Downloads the installer, verifies it and starts it. `percent` below zero means
    /// "unknown size"; `stage` is "download" or "done"; a non-empty `error` failed.
    std::function<void(std::function<void(int percent, QString stage, QString error)>)> update_install;
    /// Live log lines.
    std::function<QString()> log_text;
    /// Called after a settings change that affects a running service (engine,
    /// model, hotkeys, microphone). It is NOT called per keystroke: the window
    /// invokes it from the autosave tick once the 700 ms debounce has elapsed, so
    /// a model reload can never be triggered twice for one edit.
    std::function<void(const domain::AppSettings&)> settings_applied;
};

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(SettingsPresenter& presenter,
               WindowServices services,
               std::shared_ptr<StatusChannel> adopted_channel = nullptr,
               QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Visible state summary, used by the smoke harness and UI tests.
    [[nodiscard]] QString status_text() const;

    /// Shows a message from a running service in the status area. From a thread
    /// other than the UI thread, go through status_channel()->post() instead:
    /// this method touches widgets directly.
    void show_status_message(const QString& text);

    /// The channel services publish through; valid for the window's lifetime.
    [[nodiscard]] const std::shared_ptr<StatusChannel>& status_channel() const noexcept
    {
        return status_channel_;
    }
    /// Display label of the page at `index` (the .NET navigation label).
    [[nodiscard]] QString tab_title(int index) const;
    /// Number of pages the navigation list shows.
    [[nodiscard]] int page_count() const;
    /// The page widget itself, for tests that drive the real controls.
    [[nodiscard]] QWidget* page_widget(int index) const;
    /// The scroll area that wraps the page (section title + card); tests use it to
    /// check that nothing sticks out of the viewport.
    [[nodiscard]] QWidget* page_scroll(int index) const;
    /// Switches the visible page, exactly as clicking the navigation entry does.
    void show_page(int index);
    /// Flushes pending changes; the window does this on close too.
    void save_now();

private slots:
    void on_autosave_timeout();
    void on_recording_toggled();
    void on_presentation_refresh();

private:
    void build_tabs();
    /// Registers a built page under the navigation entry with this label. The
    /// page is only added to the stack when every page has been built, so the
    /// navigation order is the .NET one and not the build order.
    void add_page_to_nav(QWidget* page, const QString& label);
    /// Adds every registered page to the stack and to the navigation list, in
    /// kNavigation order.
    void finalize_pages();
    void bind_settings_to_controls();
    void apply_theme();
    /// Re-renders the navigation glyph icons for the current theme.
    void refresh_nav_icons();
    /// Redraws the minimize/close symbols of the custom title bar.
    void refresh_title_button_icons();
    /// True when Windows reports the dark app theme (the System theme option).
    [[nodiscard]] bool system_theme_is_dark() const;
    void refresh_status();
    /// The settings-derived part of the status line, used when no service
    /// message is pending.
    void refresh_status_summary();

    void refresh_devices();
    /// Captures the next combination into the record or cancel field: the .NET
    /// settings dialog does the same, and typing "Alt+Win+Space" by hand is not a
    /// thing a user can do.
    void capture_hotkey_into(bool record);
    /// Sets the status line the footer shows and repaints it immediately.
    void set_status_message(const QString& message);
    /// Selects one model in the list: the hidden combo carries the change to the
    /// presenter, and the other toggles are unchecked because the choice is
    /// exclusive.
    void select_model(bool whisper, int index);
    /// Mirrors the stored size into the toggles, and shows the card of the active
    /// engine only.
    void refresh_model_list();
    /// Shows the title of the page that is now on screen.
    void refresh_page_title(int index);
    /// Labels the model buttons for what the models really are: "Скачать" only while
    /// the file is missing, "Удалить" once it is there.
    void refresh_model_buttons();
    /// Wires the sensitivity slider and the microphone test to the services.
    void bind_microphone_controls();
    /// Re-applies every string from the table in the current language, in place: the
    /// window is never rebuilt for a language change.
    void retranslate();
    /// Wires the update controls on the About page.
    void bind_update_controls();
    /// A settings row whose left column is a live status label and whose right column
    /// is the action (used by the update rows).
    QWidget* setting_row_control(QWidget* status, QWidget* control);

    void resizeEvent(QResizeEvent* event) override;
    /// Handles the focus-loss hiding (the "hide on focus loss" setting).
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    /// True when the current dirty change touches something a running service
    /// has to react to, so the autosave tick can re-apply it exactly once.
    bool needs_service_apply() const;

    SettingsPresenter& presenter_;
    /// The interface language the texts were built with; a language change rebuilds
    /// the window (see the composition), because every label is created once.
    voicetyper::domain::AppLanguage language_ = voicetyper::domain::AppLanguage::ru;
    WindowServices services_;
    /// The .NET window is frameless with a custom title bar, so the shell is
    /// built by hand: 32 px title row, 210 px navigation sidebar, page stack and
    /// the footer (MainWindow.axaml:130-200).
    QListWidget* nav_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    /// label -> page, in build order; finalize_pages() reorders them.
    std::vector<std::pair<QString, QWidget*>> built_pages_;
    QLabel* status_ = nullptr;
    QLabel* engine_state_ = nullptr;
    QLabel* hotkey_state_ = nullptr;
    QTextEdit* log_view_ = nullptr;
    QPushButton* record_button_ = nullptr;
    QComboBox* app_language_ = nullptr;
    QComboBox* recognition_language_ = nullptr;
    QComboBox* engine_ = nullptr;
    QComboBox* whisper_size_ = nullptr;
    QComboBox* parakeet_size_ = nullptr;
    /// The model list of the .NET page: one toggle per model, grouped per engine.
    /// The combos above stay as hidden state holders so the presenter keeps one
    /// control per engine.
    std::vector<ToggleSwitch*> whisper_model_toggles_;
    std::vector<QPushButton*> whisper_model_buttons_;
    std::vector<QPushButton*> parakeet_model_buttons_;
    std::vector<ToggleSwitch*> parakeet_model_toggles_;
    QWidget* whisper_models_card_ = nullptr;
    /// The page title band (like the reference: the title sits on its own strip and
    /// does not scroll away with the rows).
    /// The update controls (About page).
    QLabel* update_version_ = nullptr;
    QPushButton* update_check_ = nullptr;
    QLabel* update_status_ = nullptr;
    QLabel* update_notes_ = nullptr;
    QPushButton* update_install_ = nullptr;
    QProgressBar* update_progress_ = nullptr;
    /// Set when a check found something installable; the install button appears then.
    bool update_available_ = false;

    /// Microphone level and test (the page the .NET build left empty).
    QSlider* microphone_level_ = nullptr;
    QLabel* microphone_level_value_ = nullptr;
    QPushButton* microphone_test_ = nullptr;
    QLabel* microphone_test_result_ = nullptr;
    QProgressBar* microphone_level_meter_ = nullptr;
    QWidget* page_header_ = nullptr;
    QLabel* page_title_ = nullptr;
    QWidget* parakeet_models_card_ = nullptr;
    QComboBox* theme_ = nullptr;
    QComboBox* recording_mode_ = nullptr;
    QComboBox* microphone_ = nullptr;
    QLineEdit* record_hotkey_ = nullptr;
    QPushButton* record_hotkey_capture_ = nullptr;
    QPushButton* cancel_hotkey_capture_ = nullptr;
    QLineEdit* cancel_hotkey_ = nullptr;
    QPlainTextEdit* terms_ = nullptr;
    QSpinBox* silence_threshold_ = nullptr;
    QDoubleSpinBox* temperature_ = nullptr;
    QSpinBox* best_of_ = nullptr;
    ToggleSwitch* auto_paste_ = nullptr;
    ToggleSwitch* noise_reduction_ = nullptr;
    ToggleSwitch* start_with_windows_ = nullptr;
    ToggleSwitch* start_minimized_ = nullptr;
    ToggleSwitch* hide_on_focus_loss_ = nullptr;
    ToggleSwitch* condition_on_previous_text_ = nullptr;
    QTimer* autosave_ = nullptr;
    QTimer* refresh_ = nullptr;
    bool recording_ = false;
    /// Title-bar dragging for the frameless window.
    bool dragging_ = false;
    QPoint drag_offset_;
    /// Fg.Muted of the active palette, used to draw the navigation glyphs.
    QString muted_text_;
    /// (button, is_close) pairs of the custom title bar, drawn as geometry.
    std::vector<std::pair<QPushButton*, bool>> title_button_icons_;
    QString status_message_;
    std::shared_ptr<StatusChannel> status_channel_;
    SettingsChange pending_service_change_ = SettingsChange::none;
};

} // namespace voicetyper::app
