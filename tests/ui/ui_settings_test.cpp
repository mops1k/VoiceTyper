// UI contract: the settings window really writes what the user changed.
//
// This is the one test that answers "do my settings survive a restart", so it
// drives the real widgets (not the presenter) and then reads the file back from
// disk: change the theme, let the debounce fire, read settings.json, then build
// a second window on the same file and check it shows the stored value.

#include "app/application_font.hpp"
#include "app/status_overlay.hpp"
#include "platform/api/status_overlay.hpp"
#include "app/tray_controller.hpp"
#include "app/ui_text.hpp"
#include "app/main_window.hpp"
#include "app/settings_presenter.hpp"
#include "platform/portable/portable_runtime.hpp"

#include <QApplication>
#include "app/toggle_switch.hpp"
#include <QComboBox>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QSlider>
#include <QScrollBar>
#include <QTextEdit>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QObject>
#include <QPushButton>
#include <QTest>

#include <cstdio>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string read_all(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

} // namespace

class SettingsWindowTest : public QObject {
    Q_OBJECT

private slots:
    void edit_is_written_and_reloaded()
    {
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-settings-test.json";
        std::filesystem::remove(path);

        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::MainWindow window(presenter, voicetyper::app::WindowServices{});
        window.show();

        const auto theme_index = [&window] {
            for (int i = 0; i < window.page_count(); ++i) {
                if (window.tab_title(i) == QObject::tr("Внешний вид")) {
                    return i;
                }
            }
            return -1;
        }();
        QVERIFY(theme_index >= 0);
        window.show_page(theme_index);

        auto* theme = window.page_widget(theme_index)->findChild<QComboBox*>();
        QVERIFY(theme != nullptr);
        // Index 1 is the dark theme in the appearance page.
        theme->setCurrentIndex(1);
        QVERIFY(presenter.dirty());

        // The window's own debounce timer writes 700 ms after the last edit; the
        // test waits past it instead of calling save_now() so the autosave path
        // itself is what is under test.
        QTest::qWait(1200);
        QVERIFY(!presenter.dirty());
        QVERIFY(std::filesystem::exists(path));
        const auto written = read_all(path);
        QVERIFY2(written.find("dark") != std::string::npos,
            "the changed theme must be on disk, otherwise a restart loses it");

        // Restart: a fresh presenter and window on the same file show the value.
        voicetyper::app::SettingsPresenter reloaded(path, file_system, clock);
        static_cast<void>(reloaded.load());
        QVERIFY(reloaded.settings().theme == voicetyper::domain::AppTheme::dark);
        voicetyper::app::MainWindow second(reloaded, voicetyper::app::WindowServices{});
        QVERIFY(second.tab_title(theme_index) == QObject::tr("Внешний вид"));

        std::filesystem::remove(path);
    }

    void engine_change_reaches_the_services()
    {
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-apply-test.json";
        std::filesystem::remove(path);
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        int applied = 0;
        voicetyper::app::WindowServices services;
        services.settings_applied = [&applied](const voicetyper::domain::AppSettings& settings) {
            ++applied;
            Q_UNUSED(settings)
        };
        voicetyper::app::MainWindow window(presenter, services);

        int models_index = -1;
        for (int i = 0; i < window.page_count(); ++i) {
            if (window.tab_title(i) == QObject::tr("Модели")) {
                models_index = i;
            }
        }
        QVERIFY(models_index >= 0);
        window.show_page(models_index);

        // Two edits inside the debounce window must produce exactly ONE apply:
        // a model reload per keystroke would be a visible stall.
        auto* engine = window.page_widget(models_index)->findChild<QComboBox*>();
        QVERIFY(engine != nullptr);
        engine->setCurrentIndex(1); // Parakeet
        engine->setCurrentIndex(0); // back to Whisper
        QTest::qWait(1200);
        QCOMPARE(applied, 1);

        // A settings change that no running service cares about must not trigger it.
        int general_index = -1;
        for (int i = 0; i < window.page_count(); ++i) {
            if (window.tab_title(i) == QObject::tr("Общие")) {
                general_index = i;
            }
        }
        QVERIFY(general_index >= 0);
        window.show_page(general_index);
        auto* general = window.page_widget(general_index)->findChild<voicetyper::app::ToggleSwitch*>();
        QVERIFY(general != nullptr);
        general->setChecked(!general->isChecked());
        QTest::qWait(1200);
        QCOMPARE(applied, 1);

        std::filesystem::remove(path);
    }

    void silence_threshold_is_editable_and_reaches_the_services()
    {
        // A control that is created but never put on a form is a setting that
        // saves nowhere, so the threshold has to be reachable and applied.
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-vad-test.json";
        std::filesystem::remove(path);
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        int applied = 0;
        voicetyper::app::WindowServices services;
        services.settings_applied = [&applied](const voicetyper::domain::AppSettings&) { ++applied; };
        voicetyper::app::MainWindow window(presenter, services);

        int general_index = -1;
        for (int i = 0; i < window.page_count(); ++i) {
            if (window.tab_title(i) == QObject::tr("Общие")) {
                general_index = i;
            }
        }
        QVERIFY(general_index >= 0);

        const auto spins = window.page_widget(general_index)->findChildren<QSpinBox*>();
        QVERIFY(!spins.isEmpty());
        QSpinBox* threshold = nullptr;
        for (auto* box : spins) {
            if (box->suffix().contains(QString::fromUtf8("мс"))) {
                threshold = box;
            }
        }
        QVERIFY(threshold != nullptr);
        QCOMPARE(threshold->minimum(), voicetyper::domain::kSilenceThresholdMsMin);
        QCOMPARE(threshold->maximum(), voicetyper::domain::kSilenceThresholdMsMax);

        threshold->setValue(2500);
        QTest::qWait(1200);
        QCOMPARE(presenter.settings().silence_threshold_ms, 2500);
        QCOMPARE(applied, 1);
        const auto reread = voicetyper::domain::SettingsCodec::load(
            file_system.read_text(path).value_or(std::string{}));
        QCOMPARE(reread.settings.silence_threshold_ms, 2500);
        std::filesystem::remove(path);
    }

    void stored_microphone_survives_a_launch_that_sees_no_devices()
    {
        // The window populates its device list on construction. Turning "the
        // stored device is not in the list right now" into "no device is
        // selected" silently erases the user's choice on every launch where the
        // device is busy, unplugged or simply not enumerated yet.
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-mic-preserve-test.json";
        std::filesystem::remove(path);
        {
            std::ofstream out(path, std::ios::binary);
            out << "{\"microphoneDeviceId\":\"dev-x\",\"language\":\"ru\"}";
        }

        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());
        QCOMPARE(presenter.settings().microphone_device_id.value_or(""), std::string("dev-x"));

        voicetyper::app::WindowServices services;
        services.microphones = [] { return std::vector<std::pair<std::string, std::string>>{}; };
        voicetyper::app::MainWindow window(presenter, services);
        QTest::qWait(1200);

        QVERIFY2(presenter.settings().microphone_device_id.has_value(),
            "binding an empty device list must not clear the stored microphone");
        QCOMPARE(presenter.settings().microphone_device_id.value_or(""), std::string("dev-x"));
        QVERIFY2(!presenter.dirty(), "populating the device list is not a user edit");
        const auto written = read_all(path);
        QVERIFY2(written.find("dev-x") != std::string::npos,
            "the stored microphone must still be on disk after a launch that saw no devices");
        std::filesystem::remove(path);
    }

    void stored_microphone_survives_a_launch_with_other_devices()
    {
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-mic-other-test.json";
        std::filesystem::remove(path);
        {
            std::ofstream out(path, std::ios::binary);
            out << "{\"microphoneDeviceId\":\"dev-gone\"}";
        }
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::WindowServices services;
        services.microphones = [] {
            return std::vector<std::pair<std::string, std::string>>{{"dev-a", "Microphone A"}};
        };
        voicetyper::app::MainWindow window(presenter, services);
        QTest::qWait(1200);

        QCOMPARE(presenter.settings().microphone_device_id.value_or(""), std::string("dev-gone"));
        QVERIFY(!presenter.dirty());
        // The user must be able to see that the remembered device is the one that
        // is missing, instead of a list that silently selects something else.
        auto* combo = window.findChild<QComboBox*>(QStringLiteral("microphoneCombo"));
        QVERIFY(combo != nullptr);
        QCOMPARE(combo->currentData().toString(), QStringLiteral("dev-gone"));
        std::filesystem::remove(path);
    }

    void selecting_a_microphone_is_saved_and_applied()
    {
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-mic-apply-test.json";
        std::filesystem::remove(path);
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        int applied = 0;
        std::string applied_device;
        voicetyper::app::WindowServices services;
        services.microphones = [] {
            return std::vector<std::pair<std::string, std::string>>{
                {"dev-a", "Microphone A"}, {"dev-b", "Microphone B"}};
        };
        services.settings_applied = [&applied, &applied_device](const voicetyper::domain::AppSettings& settings) {
            ++applied;
            applied_device = settings.microphone_device_id.value_or("");
        };
        voicetyper::app::MainWindow window(presenter, services);

        auto* combo = window.findChild<QComboBox*>(QStringLiteral("microphoneCombo"));
        QVERIFY(combo != nullptr);
        QCOMPARE(combo->count(), 3); // "По умолчанию" + two devices

        combo->setCurrentIndex(1);
        QTest::qWait(1200);
        QCOMPARE(presenter.settings().microphone_device_id.value_or(""), std::string("dev-a"));
        QVERIFY2(applied == 1, "one microphone edit must reach the running services exactly once");
        QCOMPARE(applied_device, std::string("dev-a"));

        // Picking "По умолчанию" is the explicit way to go back, and then the
        // field really is cleared.
        combo->setCurrentIndex(0);
        QTest::qWait(1200);
        QVERIFY(!presenter.settings().microphone_device_id.has_value());
        QCOMPARE(applied, 2);
        std::filesystem::remove(path);
    }


    // The user-visible contract of the sidebar: navigation entry N must open the
    // page that owns its controls. Reported from the running build: the "Модели"
    // entry showed the general settings, which means the stack and the list had
    // drifted apart.
    void navigation_opens_the_matching_page()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-nav-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::MainWindow window(presenter, voicetyper::app::WindowServices{});

        const struct { const char* label; const char* control; } expected[] = {
            {"Общие", "silenceThresholdSpin"},
            {"Внешний вид", "themeCombo"},
            {"Модели", "engineCombo"},
            {"Хоткеи", "recordHotkeyEdit"},
            {"Микрофон", "microphoneCombo"},
            {"Журнал", "logView"},
        };
        for (const auto& entry : expected) {
            int row = -1;
            for (int i = 0; i < window.page_count(); ++i) {
                if (window.tab_title(i) == QString::fromUtf8(entry.label)) {
                    row = i;
                    break;
                }
            }
            QVERIFY2(row >= 0, entry.label);
            window.show_page(row);
            auto* control = window.page_widget(row)->findChild<QWidget*>(QString::fromUtf8(entry.control));
            QVERIFY2(control != nullptr, entry.label);
        }
        // The engine combo belongs to "Модели" only: reaching it from any other
        // page is exactly the reported mis-mapping.
        int models_row = -1;
        for (int i = 0; i < window.page_count(); ++i) {
            if (window.tab_title(i) == QStringLiteral("Модели")) {
                models_row = i;
                break;
            }
        }
        QVERIFY(models_row >= 0);
        QVERIFY(window.page_widget(models_row)->findChild<QWidget*>(QStringLiteral("engineCombo")) != nullptr);
        QCOMPARE(window.page_count(), 8);
        std::filesystem::remove(path);
    }

    // The footer carries the record button, the engine state and the status line.
    // It must be inside the window: a page taller than the space it has pushed it
    // below the bottom edge, which makes the button unreachable.
    void the_footer_and_the_controls_fit_the_window()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-layout-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::MainWindow window(presenter, voicetyper::app::WindowServices{});
        window.resize(980, 640);
        window.show();
        QCoreApplication::processEvents();

        auto* footer = window.findChild<QWidget*>(QStringLiteral("footer"));
        QVERIFY(footer != nullptr);
        QVERIFY2(footer->isVisible(), "the footer is not visible");
        const QPoint footer_origin = footer->mapTo(&window, QPoint(0, 0));
        QVERIFY2(footer_origin.y() >= 0, "the footer starts above the window");
        QVERIFY2(footer_origin.y() + footer->height() <= window.height() + 1,
            "the footer ends below the window and its controls cannot be clicked");

        auto* record = window.findChild<QPushButton*>(QStringLiteral("recordButton"));
        QVERIFY(record != nullptr);
        QVERIFY2(record->isVisible(), "the record button is not visible");

        // Every control must fit its scroll viewport: one wider than the viewport
        // has its right border - and its value - cut off, which is what the running
        // build showed. The check runs twice: at 980x640 and near the minimum size,
        // because the machine runs at 125% display scaling and a layout that only
        // fits at full width is exactly the bug that was reported from there.
        for (const QSize window_size : {QSize(980, 640), QSize(820, 560)}) {
        window.resize(window_size);
        QCoreApplication::processEvents();
        for (int i = 0; i < window.page_count(); ++i) {
            window.show_page(i);
            QCoreApplication::processEvents();
            auto* scroll = qobject_cast<QScrollArea*>(window.page_scroll(i));
            QVERIFY2(scroll != nullptr, "a page is not wrapped in a scroll area");
            const int viewport_width = scroll->viewport()->width();
            QWidget* page = window.page_widget(i);
            std::fprintf(stderr, "  page %s viewport=%d pageMin=%d wrapperMin=%d\n",
                qPrintable(scroll->objectName()), viewport_width,
                page->minimumSizeHint().width(),
                scroll->widget() != nullptr ? scroll->widget()->minimumSizeHint().width() : -1);
            for (auto* child : page->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
                std::fprintf(stderr, "    child %s min=%d size=%d\n",
                    child->objectName().isEmpty() ? child->metaObject()->className() : qPrintable(child->objectName()),
                    child->minimumSizeHint().width(), child->width());
            }
            // The failure message carries the numbers, because the reason a control
            // overflows decides the fix (a too-wide row minimum, a fixed width that
            // cannot shrink, or a viewport narrower than expected).
            const auto overflow_message = [viewport_width, scroll](QWidget* control, const QPoint& origin) {
                const QString name = control->objectName().isEmpty()
                    ? QString::fromLatin1(control->metaObject()->className())
                    : control->objectName();
                return QStringLiteral("overflow on %7: %1 x=%2 w=%3 right=%4 viewport=%5 minHint=%6")
                    .arg(name)
                    .arg(origin.x())
                    .arg(control->width())
                    .arg(origin.x() + control->width())
                    .arg(viewport_width)
                    .arg(control->minimumSizeHint().width())
                    .arg(scroll->objectName());
            };
            for (auto* edit : scroll->findChildren<QLineEdit*>()) {
                const QPoint origin = edit->mapTo(scroll->viewport(), QPoint(0, 0));
                QVERIFY2(origin.x() + edit->width() <= viewport_width + 1,
                    qPrintable(overflow_message(edit, origin)));
            }
            for (auto* combo : scroll->findChildren<QComboBox*>()) {
                const QPoint origin = combo->mapTo(scroll->viewport(), QPoint(0, 0));
                QVERIFY2(origin.x() + combo->width() <= viewport_width + 1,
                    qPrintable(overflow_message(combo, origin)));
            }

            // A wrapping label must get the height for the lines it actually shows:
            // a row pinned to its size hint clipped the second line of every long
            // label (reported from the running build).
            for (auto* label : page->findChildren<QLabel*>()) {
                if (!label->wordWrap() || !label->isVisible()) {
                    continue;
                }
                const int needed = label->heightForWidth(label->width());
                QVERIFY2(needed <= label->height() + 1,
                    qPrintable(QStringLiteral("%1: a wrapped label needs %2 px but has %3 (its text is "
                                              "clipped): %4")
                                   .arg(scroll->objectName())
                                   .arg(needed)
                                   .arg(label->height())
                                   .arg(label->text().left(40))));
            }

            // The reference draws a hairline under every settings row, and a plain
            // QWidget only paints a stylesheet border when it is told to.
            const auto rows_with_separator
                = page->findChildren<QWidget*>(QStringLiteral("settingsRow"));
            for (auto* row : rows_with_separator) {
                QVERIFY2(row->testAttribute(Qt::WA_StyledBackground),
                    "a settings row would not paint its separator line");
            }

            // The settings must start at the top edge of the page, never float in
            // the middle of it: a row that is allowed to grow absorbs the page's
            // leftover height and the whole group drifts downwards.
            const auto direct_rows = page->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
            auto* card = page->parentWidget();
            QVERIFY(card != nullptr);
            if (!direct_rows.isEmpty() && card->height() > 40) {
                const int first_top = direct_rows.first()->y();
                QVERIFY2(first_top < card->height() / 2,
                    qPrintable(QStringLiteral("%1: the first row starts at y=%2 of a %3 px page, so the "
                                              "content is centred instead of top-aligned")
                                   .arg(scroll->objectName())
                                   .arg(first_top)
                                   .arg(card->height())));
            }
        }
        } // window sizes
        window.resize(980, 640);
        window.hide();
        std::filesystem::remove(path);
    }

    // The capture button is the only workable way to set a combination: typing
    // "Alt+Win+Space" by hand is not something a user can do. The window asks the
    // platform service, fills the field with what came back, and the value travels
    // the normal path - presenter, autosave - like a typed one.
    void the_capture_button_fills_the_hotkey_field()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-capture-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::WindowServices services;
        int applied = 0;
        std::string applied_hotkey;
        services.settings_applied = [&](const voicetyper::domain::AppSettings& settings) {
            ++applied;
            applied_hotkey = settings.record_hotkey;
        };
        int captures = 0;
        services.capture_hotkey = [&](std::function<void(std::optional<std::string>, QString)> report) {
            ++captures;
            report(std::string("Ctrl+Shift+F12"), QString());
        };

        voicetyper::app::MainWindow window(presenter, std::move(services));
        auto* button = window.findChild<QPushButton*>(QStringLiteral("recordHotkeyCapture"));
        QVERIFY(button != nullptr);
        QVERIFY2(button->isEnabled(), "the capture button is disabled although the platform offers a hook");
        button->click();
        QCoreApplication::processEvents();
        QCOMPARE(captures, 1);

        auto* field = window.findChild<QLineEdit*>(QStringLiteral("recordHotkeyEdit"));
        QVERIFY(field != nullptr);
        QCOMPARE(field->text(), QStringLiteral("Ctrl+Shift+F12"));

        // The captured combination is stored and applied exactly like a typed one.
        QTest::qWait(1200);
        QCOMPARE(presenter.settings().record_hotkey, std::string("Ctrl+Shift+F12"));
        QVERIFY2(applied >= 1, "the captured hotkey reached the running services");
        QCOMPARE(applied_hotkey, std::string("Ctrl+Shift+F12"));

        // Without the platform service the button says why instead of doing nothing.
        voicetyper::app::SettingsPresenter second_presenter(path, file_system, clock);
        static_cast<void>(second_presenter.load());
        voicetyper::app::MainWindow second(second_presenter, voicetyper::app::WindowServices{});
        auto* disabled = second.findChild<QPushButton*>(QStringLiteral("recordHotkeyCapture"));
        QVERIFY(disabled != nullptr);
        QVERIFY2(!disabled->isEnabled(), "without a capture hook the button must be disabled");
        QVERIFY2(!disabled->toolTip().isEmpty(), "a disabled capture button must say why");
        std::filesystem::remove(path);
    }

    // The model page picks a model from a list - the .NET page carries the size,
    // speed, quality and description of every model - instead of asking the user to
    // choose a "size" in a combo box.
    void the_model_list_selects_and_follows_the_engine()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-models-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::MainWindow window(presenter, voicetyper::app::WindowServices{});
        // The window must be shown for isVisible() to mean anything: a child of a
        // hidden window is never visible.
        window.resize(980, 640);
        window.show();
        int models_row = -1;
        for (int i = 0; i < window.page_count(); ++i) {
            if (window.tab_title(i) == QStringLiteral("Модели")) {
                models_row = i;
                break;
            }
        }
        QVERIFY(models_row >= 0);
        window.show_page(models_row);
        QCoreApplication::processEvents();

        auto* page = window.page_widget(models_row);
        const auto whisper_toggles = page->findChildren<voicetyper::app::ToggleSwitch*>(
            QRegularExpression(QStringLiteral("^whisperModelToggle")));
        const auto parakeet_toggles = page->findChildren<voicetyper::app::ToggleSwitch*>(
            QRegularExpression(QStringLiteral("^parakeetModelToggle")));
        QCOMPARE(whisper_toggles.size(), 5);
        QCOMPARE(parakeet_toggles.size(), 4);

        // The card of the active engine is the only one on screen.
        auto* whisper_card = page->findChild<QWidget*>(QStringLiteral("whisperModelsCard"));
        auto* parakeet_card = page->findChild<QWidget*>(QStringLiteral("parakeetModelsCard"));
        QVERIFY(whisper_card != nullptr && parakeet_card != nullptr);
        auto* engine = page->findChild<QComboBox*>(QStringLiteral("engineCombo"));
        QVERIFY(engine != nullptr);
        QCOMPARE(engine->currentData().toInt(),
            static_cast<int>(voicetyper::domain::TranscriptionEngine::whisper));
        QVERIFY2(whisper_card->isVisible(), "the whisper model list is not shown for the whisper engine");
        QVERIFY2(!parakeet_card->isVisible(), "the parakeet model list is shown for the whisper engine");

        // Picking a model is exclusive and reaches the stored setting.
        whisper_toggles[3]->click();
        QCoreApplication::processEvents();
        QVERIFY(whisper_toggles[3]->isChecked());
        QVERIFY2(!whisper_toggles[0]->isChecked(), "a model choice must be exclusive");
        QVERIFY2(!whisper_toggles[4]->isChecked(), "a model choice must be exclusive");
        QTest::qWait(1200);
        QCOMPARE(static_cast<int>(presenter.settings().model_size),
            static_cast<int>(voicetyper::domain::ModelSize::medium));

        // Switching the engine switches the list, and the previous choice is kept.
        engine->setCurrentIndex(1);
        QCoreApplication::processEvents();
        QVERIFY2(!whisper_card->isVisible(), "the whisper list is still shown for the parakeet engine");
        QVERIFY2(parakeet_card->isVisible(), "the parakeet list is not shown for the parakeet engine");
        parakeet_toggles[1]->click();
        QCoreApplication::processEvents();
        QTest::qWait(1200);
        QCOMPARE(static_cast<int>(presenter.settings().parakeet_model_size),
            static_cast<int>(voicetyper::domain::ParakeetModelSize::q5k));
        QCOMPARE(static_cast<int>(presenter.settings().model_size),
            static_cast<int>(voicetyper::domain::ModelSize::medium));
        window.hide();
        std::filesystem::remove(path);
    }

    // Noise reduction describes the input device, so it lives on the microphone page
    // (Alexander, 2026-10-06); it used to sit with the general settings.
    void noise_reduction_lives_on_the_microphone_page()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-micnoise-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());
        voicetyper::app::MainWindow window(presenter, voicetyper::app::WindowServices{});

        int microphone_row = -1;
        int general_row = -1;
        for (int i = 0; i < window.page_count(); ++i) {
            if (window.tab_title(i) == QStringLiteral("Микрофон")) {
                microphone_row = i;
            }
            if (window.tab_title(i) == QStringLiteral("Общие")) {
                general_row = i;
            }
        }
        QVERIFY(microphone_row >= 0 && general_row >= 0);
        QVERIFY2(window.page_widget(microphone_row)
                     ->findChild<QWidget*>(QStringLiteral("noiseReductionToggle")) != nullptr,
            "the microphone page does not own the noise reduction switch");
        QVERIFY2(window.page_widget(general_row)
                     ->findChild<QWidget*>(QStringLiteral("noiseReductionToggle")) == nullptr,
            "the general page still shows the noise reduction switch");
        std::filesystem::remove(path);
    }

    // A model that is already on disk must offer "Удалить", not a pointless
    // "Скачать" (reported from the running build about the working models).
    void downloaded_models_offer_delete()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-modeldelete-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::WindowServices services;
        int deleted_index = -1;
        services.model_is_downloaded = [](bool, int size_index) { return size_index == 2; };
        services.model_delete = [&deleted_index](bool, int size_index) {
            deleted_index = size_index;
            return true;
        };
        voicetyper::app::MainWindow window(presenter, std::move(services));
        window.resize(980, 640);
        window.show();

        auto* downloaded = window.findChild<QPushButton*>(QStringLiteral("whisperModelButton2"));
        auto* missing = window.findChild<QPushButton*>(QStringLiteral("whisperModelButton0"));
        QVERIFY(downloaded != nullptr && missing != nullptr);
        QCOMPARE(downloaded->text(), QStringLiteral("Удалить"));
        QVERIFY2(downloaded->isEnabled(), "a downloaded model must offer a working delete");
        QCOMPARE(missing->text(), QStringLiteral("Скачать"));
        QVERIFY2(!missing->isEnabled(), "a model that is not on disk cannot be downloaded yet");
        QVERIFY2(!missing->toolTip().isEmpty(), "the disabled download button must say why");

        downloaded->click();
        QCoreApplication::processEvents();
        QCOMPARE(deleted_index, 2);
        window.hide();
        std::filesystem::remove(path);
    }

    // The log used to jump back to its first line on every refresh tick, so nothing
    // could be read (reported from the running build).
    void the_log_keeps_its_scroll_position()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-logscroll-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::WindowServices services;
        QString log;
        for (int line = 0; line < 400; ++line) {
            log += QStringLiteral("line %1 of the application log\n").arg(line);
        }
        services.log_text = [&log] { return log; };
        voicetyper::app::MainWindow window(presenter, std::move(services));
        window.resize(980, 640);
        window.show();
        window.show_page(6); // Журнал
        QCoreApplication::processEvents();
        QTest::qWait(700);

        auto* log_view = window.findChild<QTextEdit*>(QStringLiteral("logView"));
        QVERIFY(log_view != nullptr);
        QScrollBar* bar = log_view->verticalScrollBar();
        QVERIFY(bar->maximum() > 20);
        bar->setValue(bar->maximum() / 3);
        const int chosen = bar->value();
        QTest::qWait(1200); // several refresh ticks
        QCOMPARE(bar->value(), chosen);
        window.hide();
        std::filesystem::remove(path);
    }

    // Switching the interface language in the settings did nothing at all: every
    // label was hard-coded Russian (reported from the running build). The window now
    // builds its texts from the string table in the language that was stored.
    void the_interface_language_follows_the_setting()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-language-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());
        presenter.update(voicetyper::app::SettingsChange::language, [](voicetyper::domain::AppSettings& settings) {
            settings.app_language = voicetyper::domain::AppLanguage::en;
        });
        voicetyper::app::MainWindow window(presenter, voicetyper::app::WindowServices{});

        QStringList titles;
        bool english_general = false;
        for (int i = 0; i < window.page_count(); ++i) {
            titles << window.tab_title(i);
            english_general = english_general || window.tab_title(i) == QStringLiteral("General");
        }
        QVERIFY2(english_general,
            qPrintable(QStringLiteral("the navigation is still not English: %1").arg(titles.join(QStringLiteral(", ")))));

        bool english_label = false;
        bool russian_left = false;
        for (auto* label : window.findChildren<QLabel*>()) {
            english_label = english_label || label->text() == QStringLiteral("Interface language");
            russian_left = russian_left || label->text() == QStringLiteral("Язык интерфейса");
        }
        QVERIFY2(english_label, "the settings rows are not translated");
        QVERIFY2(!russian_left, "a Russian row label is still shown in English mode");

        // And the same window in Russian keeps the Russian texts.
        voicetyper::app::SettingsPresenter russian_presenter(path, file_system, clock);
        static_cast<void>(russian_presenter.load());
        russian_presenter.update(voicetyper::app::SettingsChange::language,
            [](voicetyper::domain::AppSettings& settings) {
                settings.app_language = voicetyper::domain::AppLanguage::ru;
            });
        voicetyper::app::MainWindow russian(russian_presenter, voicetyper::app::WindowServices{});
        bool russian_label = false;
        for (auto* label : russian.findChildren<QLabel*>()) {
            russian_label = russian_label || label->text() == QStringLiteral("Язык интерфейса");
        }
        QVERIFY2(russian_label, "the Russian interface lost its own texts");
        std::filesystem::remove(path);
    }

    // A language change is applied in place: the window is not rebuilt (Alexander asked
    // for exactly that after the first attempt closed and reopened it), so what must hold
    // is that the setting is stored AND the visible strings change on the same window.
    void the_interface_language_changes_in_place()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-languageapply-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::MainWindow window(presenter, voicetyper::app::WindowServices{});
        window.resize(980, 640);
        window.show();
        QCoreApplication::processEvents();

        const auto find_row_label = [&window](const QString& text) {
            for (auto* label : window.findChildren<QLabel*>()) {
                if (label->text() == text) {
                    return label;
                }
            }
            return static_cast<QLabel*>(nullptr);
        };
        QVERIFY2(find_row_label(QStringLiteral("Язык интерфейса")) != nullptr,
            "the Russian label is missing before the change");

        auto* combo = window.findChild<QComboBox*>(QStringLiteral("appLanguageCombo"));
        QVERIFY(combo != nullptr);
        combo->setCurrentIndex(1); // English
        QCoreApplication::processEvents();

        QCOMPARE(static_cast<int>(presenter.settings().app_language),
            static_cast<int>(voicetyper::domain::AppLanguage::en));
        QVERIFY2(find_row_label(QStringLiteral("Interface language")) != nullptr,
            "the window did not re-letter itself: the English label is missing");
        QVERIFY2(find_row_label(QStringLiteral("Язык интерфейса")) == nullptr,
            "the Russian label is still there after switching to English");
        // The navigation follows too.
        bool english_nav = false;
        for (int i = 0; i < window.page_count(); ++i) {
            english_nav = english_nav || window.tab_title(i) == QStringLiteral("General");
        }
        QVERIFY2(english_nav, "the navigation did not switch to English");
        window.hide();
        std::filesystem::remove(path);
    }

    // The string table is index-based, so one entry without a key shifts every later
    // label - which happened while the update strings were added, and nothing failed.
    // This walks the whole table instead of trusting the eye.
    void every_ui_key_has_a_text_in_both_languages()
    {
        using voicetyper::app::UiKey;
        using voicetyper::app::ui_text;
        using voicetyper::domain::AppLanguage;
        for (int index = 0; index < static_cast<int>(UiKey::kCount); ++index) {
            const auto key = static_cast<UiKey>(index);
            const QString russian = ui_text(key, AppLanguage::ru);
            const QString english = ui_text(key, AppLanguage::en);
            QVERIFY2(!russian.isEmpty(), qPrintable(QStringLiteral("no Russian text for key %1").arg(index)));
            QVERIFY2(!english.isEmpty(), qPrintable(QStringLiteral("no English text for key %1").arg(index)));
        }
        // A few exact texts, including the boundary where the table once slipped.
        QCOMPARE(ui_text(UiKey::k63, AppLanguage::ru), QStringLiteral("Версия"));
        QCOMPARE(ui_text(UiKey::k64, AppLanguage::ru), QStringLiteral("Проверить обновления"));
        QCOMPARE(ui_text(UiKey::k66, AppLanguage::ru), QStringLiteral("Обновлений нет"));
        QCOMPARE(ui_text(UiKey::k73, AppLanguage::ru), QStringLiteral("Обновления ещё не проверялись"));
        QCOMPARE(ui_text(UiKey::k76, AppLanguage::ru), QStringLiteral("Обновление"));
        QCOMPARE(ui_text(UiKey::k73, AppLanguage::en), QStringLiteral("Updates have not been checked yet"));
    }

    // The About page is the update surface: the running version, a check that reports
    // what it found, the release notes and the installer. The .NET build had the same
    // flow on the same page.
    void the_about_page_offers_the_update_controls()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-update-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::WindowServices services;
        services.application_version = [] { return QStringLiteral("1.2.3"); };
        int checks = 0;
        services.update_check = [&checks](std::function<void(bool, QString, QString, QString)> report) {
            ++checks;
            report(true, QStringLiteral("9.9.9"), QStringLiteral("Заметки к выпуску"), QString());
        };
        int installs = 0;
        services.update_install = [&installs](std::function<void(int, QString, QString)> progress) {
            ++installs;
            progress(50, QStringLiteral("download"), QString());
        };
        voicetyper::app::MainWindow window(presenter, std::move(services));
        window.resize(980, 640);
        window.show();

        int about_row = -1;
        for (int i = 0; i < window.page_count(); ++i) {
            if (window.tab_title(i) == QStringLiteral("О программе")) {
                about_row = i;
            }
        }
        QVERIFY(about_row >= 0);
        window.show_page(about_row);
        QCoreApplication::processEvents();

        auto* version = window.findChild<QLabel*>(QStringLiteral("updateVersion"));
        QVERIFY(version != nullptr);
        QCOMPARE(version->text(), QStringLiteral("1.2.3"));

        auto* status = window.findChild<QLabel*>(QStringLiteral("updateStatus"));
        QVERIFY(status != nullptr);
        QVERIFY2(!status->text().isEmpty(), "the update status starts empty");

        auto* check = window.findChild<QPushButton*>(QStringLiteral("updateCheckButton"));
        QVERIFY(check != nullptr);
        auto* install = window.findChild<QPushButton*>(QStringLiteral("updateInstallButton"));
        QVERIFY(install != nullptr);
        QVERIFY2(!install->isVisible(), "the install button appears only when something is available");

        check->click();
        QTest::qWait(200);
        QCOMPARE(checks, 1);
        QVERIFY2(status->text().contains(QStringLiteral("9.9.9")),
            qPrintable(QStringLiteral("the status does not name the available version: %1").arg(status->text())));
        auto* notes = window.findChild<QLabel*>(QStringLiteral("updateNotes"));
        QVERIFY(notes != nullptr);
        QVERIFY2(notes->isVisible(), "the release notes are hidden after a check found an update");
        QVERIFY2(install->isVisible(), "the install button did not appear");

        install->click();
        QTest::qWait(200);
        QCOMPARE(installs, 1);
        auto* progress = window.findChild<QProgressBar*>(QStringLiteral("updateProgress"));
        QVERIFY(progress != nullptr);
        QVERIFY2(progress->isVisible(), "the progress bar did not appear");
        QCOMPARE(progress->value(), 50);
        window.hide();
        std::filesystem::remove(path);
    }

    // «Скрывать при потере фокуса» did nothing at all: the value was stored and never
    // used (Alexander, 2026-10-06). It now sends the window to the tray.
    void hide_on_focus_loss_hides_the_window()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-focusloss-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());
        voicetyper::app::MainWindow window(presenter, voicetyper::app::WindowServices{});
        window.resize(980, 640);
        window.show();
        QCoreApplication::processEvents();

        auto* toggle = window.findChild<QAbstractButton*>(QStringLiteral("hideOnFocusLossToggle"));
        QVERIFY2(toggle != nullptr, "the hide-on-focus-loss switch is missing");
        QVERIFY2(!toggle->isChecked(), "the setting is off by default");

        QEvent deactivate(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(&window, &deactivate);
        QCoreApplication::processEvents();
        QVERIFY2(window.isVisible(), "the window must stay open while the setting is off");

        toggle->click();
        QCoreApplication::processEvents();
        QVERIFY(toggle->isChecked());
        window.show();
        QCoreApplication::processEvents();
        QCoreApplication::sendEvent(&window, &deactivate);
        QCoreApplication::processEvents();
        QVERIFY2(!window.isVisible(), "the window did not go to the tray on focus loss");
        window.hide();
        std::filesystem::remove(path);
    }

    // The status pill must be readable in both themes: the window's global stylesheet
    // painted it white while its text stayed the light grey of the dark theme.
    void the_status_pill_follows_the_theme()
    {
        voicetyper::app::QtStatusOverlay overlay;
        QVERIFY(overlay.create().is_ok());
        // The pill is a frameless top-level window, so it is found by name among the
        // application's top-level widgets rather than as a child of the overlay object.
        QWidget* pill = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QStringLiteral("statusOverlay")) {
                pill = widget;
            }
        }
        QVERIFY2(pill != nullptr, "the overlay pill was not created");
        // The status label is a child of the pill.
        auto* text = pill->findChild<QLabel*>(QStringLiteral("statusOverlayText"));
        QVERIFY2(text != nullptr, "the overlay text was not created");
        // The background must not hug the glyphs, and the corners are only slightly
        // rounded: that is what the padding and the radius in the sheet are for.
        QVERIFY2(pill->styleSheet().contains(QStringLiteral("padding:")),
            qPrintable(QStringLiteral("the pill has no padding: %1").arg(pill->styleSheet())));
        QVERIFY2(pill->styleSheet().contains(QStringLiteral("border-radius: 8px")),
            qPrintable(QStringLiteral("the pill radius is not the small one: %1").arg(pill->styleSheet())));

        overlay.set_theme(voicetyper::domain::AppTheme::dark);
        QVERIFY2(pill->styleSheet().contains(QStringLiteral("#202020")),
            qPrintable(QStringLiteral("dark pill: %1").arg(pill->styleSheet())));
        QVERIFY2(text->styleSheet().contains(QStringLiteral("#FFFFFF")),
            qPrintable(QStringLiteral("dark text: %1").arg(text->styleSheet())));

        overlay.set_theme(voicetyper::domain::AppTheme::light);
        QVERIFY2(pill->styleSheet().contains(QStringLiteral("#FFFFFF")),
            qPrintable(QStringLiteral("light pill: %1").arg(pill->styleSheet())));
        QVERIFY2(text->styleSheet().contains(QStringLiteral("#1B1B1B")),
            qPrintable(QStringLiteral("light text: %1").arg(text->styleSheet())));
    }

    // Клик по иконке в трее должен выводить окно настроек на передний план. Здесь
    // закрепляется та часть, которую видно без оконного менеджера: свёрнутое окно
    // возвращается и снова видимо. Порядок окон на экране offscreen-платформа не
    // сообщает, поэтому его проверяют на устройстве (клик по иконке).
    void showing_the_window_from_the_tray_restores_it()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-showfromtray-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::MainWindow window(presenter, voicetyper::app::WindowServices{});
        window.resize(980, 640);
        window.show();
        QCoreApplication::processEvents();
        QVERIFY(window.isVisible());

        // An offscreen platform does not have to honour minimisation, so it is not
        // assumed: where it happened, restoring is checked; the hidden-window path below
        // is the one a tray click really meets when the window was closed to the tray.
        window.showMinimized();
        QCoreApplication::processEvents();
        window.bring_to_front();
        QCoreApplication::processEvents();
        QVERIFY2(!window.isMinimized(), "окно осталось свёрнутым после показа из трея");
        QVERIFY2(window.isVisible(), "окно не показалось после показа из трея");

        window.hide();
        QCoreApplication::processEvents();
        QVERIFY2(!window.isVisible(), "окно не скрылось, проверка бессмысленна");
        window.bring_to_front();
        QCoreApplication::processEvents();
        QVERIFY2(window.isVisible(), "скрытое окно не показалось по клику в трее");
        std::filesystem::remove(path);
    }

    // В контекстном меню трея текст упирался в правый край: у пунктов должен быть
    // запас справа (замечание Александра, 06.10.2026).
    void the_tray_menu_keeps_room_on_the_right()
    {
        QSystemTrayIcon icon;
        voicetyper::app::TrayController tray(icon);
        QVERIFY2(tray.menu() != nullptr, "у контроллера трея нет меню");
        const QString sheet = tray.menu()->styleSheet();
        QVERIFY2(sheet.contains(QStringLiteral("QMenu::item")),
            qPrintable(QStringLiteral("меню без правил для пунктов: %1").arg(sheet)));
        QVERIFY2(sheet.contains(QStringLiteral("padding")),
            qPrintable(QStringLiteral("у пунктов нет отступов: %1").arg(sheet)));
    }

    // Оверлей и меню трея не переводились вообще: их строки были заданы прямо в
    // Qt-слое, поэтому смена языка их не касалась (Александр, 06.10.2026).
    void the_status_overlay_follows_the_language()
    {
        voicetyper::app::QtStatusOverlay overlay;
        QVERIFY(overlay.create().is_ok());
        QWidget* pill = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QStringLiteral("statusOverlay")) {
                pill = widget;
            }
        }
        QVERIFY(pill != nullptr);
        auto* text = pill->findChild<QLabel*>(QStringLiteral("statusOverlayText"));
        QVERIFY(text != nullptr);

        overlay.set_state(voicetyper::platform::OverlayState::recording);
        QCOMPARE(text->text(), QStringLiteral("Захват"));
        overlay.set_language(voicetyper::domain::AppLanguage::en);
        QCOMPARE(text->text(), QStringLiteral("Capture"));
        overlay.set_state(voicetyper::platform::OverlayState::processing);
        QCOMPARE(text->text(), QStringLiteral("Recognizing"));
        overlay.set_language(voicetyper::domain::AppLanguage::ru);
        QCOMPARE(text->text(), QStringLiteral("Распознавание"));
    }

    void the_tray_menu_follows_the_language()
    {
        QSystemTrayIcon icon;
        voicetyper::app::TrayController tray(icon);
        const auto entries = [&tray] {
            QStringList out;
            for (auto* action : tray.menu()->actions()) {
                out << action->text();
            }
            return out;
        };
        QVERIFY2(entries().contains(QStringLiteral("Открыть настройки")),
            qPrintable(entries().join(QLatin1Char('|'))));
        tray.set_language(voicetyper::domain::AppLanguage::en);
        QVERIFY2(entries().contains(QStringLiteral("Open settings")),
            qPrintable(entries().join(QLatin1Char('|'))));
        QVERIFY2(entries().contains(QStringLiteral("Quit")),
            qPrintable(entries().join(QLatin1Char('|'))));
        tray.set_recording(true);
        QVERIFY2(entries().contains(QStringLiteral("Stop")),
            qPrintable(entries().join(QLatin1Char('|'))));
        tray.set_language(voicetyper::domain::AppLanguage::ru);
        QVERIFY2(entries().contains(QStringLiteral("Остановить")),
            qPrintable(entries().join(QLatin1Char('|'))));
    }

    // Страница «Модели» держала свои тексты (размер, скорость, точность, описание) прямо
    // в таблице кода, поэтому в английском интерфейсе оставалась русской (Александр,
    // 06.10.2026). Проверяется и то, что до переключения русский там есть, - иначе тест
    // ничего не доказывал бы.
    void the_models_page_follows_the_language()
    {
        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-models-language-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::MainWindow window(presenter, voicetyper::app::WindowServices{});
        window.resize(980, 640);
        window.show();
        QCoreApplication::processEvents();

        const auto has_cyrillic = [](const QString& text) {
            for (const QChar character : text) {
                if (character.script() == QChar::Script_Cyrillic) {
                    return true;
                }
            }
            return false;
        };
        const auto card_texts = [&window] {
            QStringList out;
            auto* card = window.findChild<QWidget*>(QStringLiteral("whisperModelsCard"));
            if (card != nullptr) {
                for (auto* label : card->findChildren<QLabel*>()) {
                    out << label->text();
                }
            }
            return out;
        };
        const auto any_cyrillic = [&has_cyrillic](const QStringList& texts) {
            for (const QString& text : texts) {
                if (has_cyrillic(text)) {
                    return true;
                }
            }
            return false;
        };
        QVERIFY2(any_cyrillic(card_texts()), "до переключения на английский русский текст обязан быть");

        auto* combo = window.findChild<QComboBox*>(QStringLiteral("appLanguageCombo"));
        QVERIFY(combo != nullptr);
        combo->setCurrentIndex(1); // English
        QCoreApplication::processEvents();

        const QStringList after = card_texts();
        QVERIFY(!after.isEmpty());
        QVERIFY2(!any_cyrillic(after), qPrintable(after.join(QLatin1Char('|'))));
        auto* hint = window.findChild<QLabel*>(QStringLiteral("engineHint"));
        QVERIFY(hint != nullptr);
        QVERIFY2(!has_cyrillic(hint->text()), qPrintable(hint->text()));
        window.hide();
        std::filesystem::remove(path);
    }

    void record_button_is_disabled_without_a_backend()
    {        voicetyper::platform::PortableClock clock;
        voicetyper::platform::PortableFileSystem file_system;
        const auto path = std::filesystem::temp_directory_path() / "voicetyper-ui-record-test.json";
        std::filesystem::remove(path);
        voicetyper::app::SettingsPresenter presenter(path, file_system, clock);
        static_cast<void>(presenter.load());

        voicetyper::app::MainWindow window(presenter, voicetyper::app::WindowServices{});
        auto* record = window.findChild<QPushButton*>(QStringLiteral("recordButton"));
        QVERIFY(record != nullptr);
        // No recording backend is registered on this host, so the button must be
        // disabled instead of starting a session that cannot work.
        QVERIFY(!record->isEnabled());
        QVERIFY(!record->toolTip().isEmpty());
        std::filesystem::remove(path);
    }
};

// QTest's own plain logger emits nothing on the Windows build of this Qt SDK,
// so a PASS_REGULAR_EXPRESSION on "Totals: ..." could never match even though the
// test really ran and passed. The pass criterion is therefore the exit code plus
// a marker printed here, which keeps the "it actually executed" guarantee
// without depending on where Qt routes its output.
int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    // Build every window in the shipped typeface, as the application does.
    voicetyper::app::install_application_font();
    SettingsWindowTest test;
    const int failures = QTest::qExec(&test, argc, argv);
    std::cout << "ui-settings-test: result=" << (failures == 0 ? "passed" : "failed")
              << " failures=" << failures << '\n';
    std::cout.flush();
    return failures;
}

#include "ui_settings_test.moc"
