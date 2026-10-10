// UI contract: the frameless status overlay really is the pill the contract
// describes.
//
// The test drives the real widget rather than a copy of its logic, because the
// observable behaviour is exactly what the contract freezes: a frameless,
// always-on-top pill that does not enter the taskbar, does not take focus and
// does not swallow keystrokes or clicks, shows recording/processing/error,
// pulses while recording with the frozen 350 ms timer, and reports its state.
//
// It runs offscreen: the window flags, the geometry rule and the pulse are
// platform-independent Qt behaviour, so the test must not depend on a desktop
// session or on a real display being attached (CI has neither).

#include "app/status_overlay.hpp"

#include <QApplication>
#include <QCursor>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QScreen>
#include <QTest>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr auto kRecordingText = "Захват";
constexpr auto kProcessingText = "Распознавание";
constexpr auto kRecordingAccent = "#4c8bf5";
constexpr auto kProcessingAccent = "#f5a623";

QWidget* dot_of(QWidget* pill)
{
    return pill == nullptr ? nullptr : pill->findChild<QWidget*>(QStringLiteral("statusOverlayDot"));
}

QLabel* text_of(QWidget* pill)
{
    return pill == nullptr ? nullptr : pill->findChild<QLabel*>(QStringLiteral("statusOverlayText"));
}

double opacity_of(QWidget* dot)
{
    if (dot == nullptr) {
        return -1.0;
    }
    const auto* effect = qobject_cast<QGraphicsOpacityEffect*>(dot->graphicsEffect());
    return effect == nullptr ? -1.0 : effect->opacity();
}

} // namespace

class StatusOverlayTest : public QObject {
    Q_OBJECT

private slots:
    void create_is_idempotent_and_starts_hidden()
    {
        voicetyper::app::QtStatusOverlay overlay;
        QCOMPARE(overlay.current_state(), voicetyper::platform::OverlayState::idle);

        QVERIFY(overlay.create().is_ok());
        QVERIFY(overlay.create().is_ok());
        QVERIFY(overlay.widget() != nullptr);
        QVERIFY(!overlay.widget()->isVisible());
        QCOMPARE(overlay.current_state(), voicetyper::platform::OverlayState::idle);
    }

    void recording_is_a_frameless_topmost_pill_that_never_takes_input()
    {
        voicetyper::app::QtStatusOverlay overlay;
        QVERIFY(overlay.show(voicetyper::platform::OverlayState::recording).is_ok());
        QTest::qWait(30);

        QWidget* pill = overlay.widget();
        QVERIFY(pill != nullptr);
        QVERIFY(pill->isVisible());
        QCOMPARE(overlay.current_state(), voicetyper::platform::OverlayState::recording);

        // The window flags belong to the overlay's host window, not to the pill:
        // on Wayland a client cannot place its own top-level window, so the pill
        // is a child of a full-screen, click-through host that the compositor
        // puts at the screen origin (see QtStatusOverlay::build()).
        QWidget* host = pill->window();
        QVERIFY(host != nullptr);
        const Qt::WindowFlags flags = host->windowFlags();
        QVERIFY2((flags & Qt::FramelessWindowHint) != 0, "the overlay must be frameless");
        QVERIFY2((flags & Qt::WindowStaysOnTopHint) != 0, "the overlay must stay above the window being typed into");
        QVERIFY2((flags & Qt::Tool) != 0, "Qt::Tool keeps the overlay out of the taskbar");
        QVERIFY2((flags & Qt::WindowDoesNotAcceptFocus) != 0, "typing must never be interrupted by the overlay");
        QVERIFY2(host->testAttribute(Qt::WA_ShowWithoutActivating), "showing the overlay must not activate it");
        QVERIFY2(host->testAttribute(Qt::WA_TransparentForMouseEvents), "the overlay must not swallow clicks");
        QVERIFY2(pill->testAttribute(Qt::WA_TransparentForMouseEvents), "the pill must not swallow clicks");
        // isActiveWindow() is deliberately not asserted here: the offscreen
        // platform activates the only window in its session, so the value says
        // nothing about a desktop with a window the user is typing into. The
        // three attributes above plus the focus state below are the portable
        // part of "this window never takes input"; the physical Windows smoke
        // checks the real activation behaviour.
        QCOMPARE(pill->focusPolicy(), Qt::NoFocus);
        QVERIFY(pill->focusWidget() == nullptr);

        // Window flags alone do not keep a window out of the window list on
        // Wayland; a layer-shell surface does. This test runs offscreen by
        // default, where the host stays an ordinary window; run it with
        // QT_QPA_PLATFORM=wayland in a session to see the promoted path.
        const bool on_wayland = voicetyper::app::overlay_needs_layer_shell(
            QGuiApplication::platformName().toStdString());
        QCOMPARE(overlay.layer_shell_active(),
            on_wayland && voicetyper::app::layer_shell_build_available());

        QLabel* text = text_of(pill);
        QVERIFY(text != nullptr);
        QCOMPARE(text->text(), QString::fromUtf8(kRecordingText));
        QVERIFY(pill->findChild<QWidget*>(QStringLiteral("statusOverlayDot")) != nullptr);
    }

    void only_wayland_needs_a_layer_shell_surface()
    {
        // On Wayland an ordinary xdg-toplevel ends up in the window list and can
        // be activated, which is exactly what was reported from the running build
        // (2026-10-11); the overlay therefore promotes its host to a layer-shell
        // surface there. Every other platform keeps the plain window.
        QVERIFY(voicetyper::app::overlay_needs_layer_shell("wayland"));
        QVERIFY(!voicetyper::app::overlay_needs_layer_shell("xcb"));
        QVERIFY(!voicetyper::app::overlay_needs_layer_shell("offscreen"));
        QVERIFY(!voicetyper::app::overlay_needs_layer_shell(""));
    }

    void recording_pulses_with_the_frozen_period()
    {
        voicetyper::app::QtStatusOverlay overlay;
        QVERIFY(overlay.show(voicetyper::platform::OverlayState::recording).is_ok());
        QWidget* dot = dot_of(overlay.widget());
        QVERIFY(dot != nullptr);

        const double first = opacity_of(dot);
        QVERIFY2(first == 1.0 || first == 0.35, "the pulse only ever uses the two frozen opacities");
        // The tick is awaited rather than assumed to land inside one period: the timer is
        // coarse and the suite runs other tests beside this one, so under load a sample
        // could be taken twice in the same phase - which is exactly how this failed once
        // on Windows while passing on its own.
        QTRY_VERIFY_WITH_TIMEOUT(opacity_of(dot) != first,
            voicetyper::platform::kOverlayPulsePeriod.count() + 600);
        // Coming back to the very first value also proves the timer keeps running for as
        // long as the state does.
        QTRY_VERIFY_WITH_TIMEOUT(opacity_of(dot) == first,
            3 * voicetyper::platform::kOverlayPulsePeriod.count() + 600);
    }

    void processing_changes_the_accent_and_stops_the_pulse()
    {
        voicetyper::app::QtStatusOverlay overlay;
        QVERIFY(overlay.show(voicetyper::platform::OverlayState::recording).is_ok());
        QWidget* dot = dot_of(overlay.widget());
        QVERIFY(dot != nullptr);
        QVERIFY(dot->styleSheet().toLower().contains(QString::fromLatin1(kRecordingAccent)));

        QVERIFY(overlay.set_state(voicetyper::platform::OverlayState::processing).is_ok());
        QCOMPARE(overlay.current_state(), voicetyper::platform::OverlayState::processing);
        QVERIFY(overlay.widget()->isVisible());
        QCOMPARE(text_of(overlay.widget())->text(), QString::fromUtf8(kProcessingText));
        QVERIFY2(dot->styleSheet().toLower().contains(QString::fromLatin1(kProcessingAccent)),
            "processing has its own accent colour");

        const double settled = opacity_of(dot);
        QCOMPARE(settled, 1.0);
        QTest::qWait(3 * voicetyper::platform::kOverlayPulsePeriod.count());
        QCOMPARE(opacity_of(dot), 1.0);
    }

    void an_error_hides_the_overlay_instead_of_showing_it()
    {
        voicetyper::app::QtStatusOverlay overlay;
        const std::string detail = "модель не загружена";
        QVERIFY(overlay.show(voicetyper::platform::OverlayState::error, detail).is_ok());
        // The overlay is not an error channel: a missing model, a lost device or a
        // refused engine belongs in the window's status line and the log, never on
        // top of the window the user is typing into - and it must not stay there
        // (reported from the running build, 2026-10-11).
        QVERIFY(!overlay.widget()->isVisible());
        QCOMPARE(overlay.current_state(), voicetyper::platform::OverlayState::idle);

        // A failed session ends in idle; that must not resurrect the overlay.
        QVERIFY(overlay.set_state(voicetyper::platform::OverlayState::idle).is_ok());
        QVERIFY(!overlay.widget()->isVisible());

        // The next dictation shows the normal pill again.
        QVERIFY(overlay.set_state(voicetyper::platform::OverlayState::recording).is_ok());
        QVERIFY(overlay.widget()->isVisible());
        QCOMPARE(text_of(overlay.widget())->text(), QString::fromUtf8(kRecordingText));
    }

    void hide_keeps_the_window_and_idle_hides_it()
    {
        voicetyper::app::QtStatusOverlay overlay;
        QVERIFY(overlay.show(voicetyper::platform::OverlayState::processing).is_ok());
        QWidget* pill = overlay.widget();
        QVERIFY(pill != nullptr);

        QVERIFY(overlay.hide().is_ok());
        QVERIFY(!pill->isVisible());
        QCOMPARE(overlay.current_state(), voicetyper::platform::OverlayState::idle);
        QVERIFY2(overlay.widget() == pill, "hiding must not destroy the window: it is shown on every recording");

        // show(idle) is the "not dictating" transition the machine produces.
        QVERIFY(overlay.show(voicetyper::platform::OverlayState::idle).is_ok());
        QVERIFY(overlay.widget() == pill);
        QVERIFY(!pill->isVisible());
    }

    void geometry_is_centered_and_26px_above_the_bottom()
    {
        voicetyper::app::QtStatusOverlay overlay;
        QVERIFY(overlay.show(voicetyper::platform::OverlayState::recording).is_ok());
        QTest::qWait(30);

        // The pill follows the display the user last interacted with.
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (screen == nullptr) {
            screen = QGuiApplication::primaryScreen();
        }
        if (screen == nullptr || screen->availableGeometry().isEmpty()) {
            QSKIP("this platform reports no usable screen geometry");
        }
        const QRect available = screen->availableGeometry();
        if (overlay.layer_shell_active()) {
            // On Wayland the compositor places the surface (anchored to the
            // bottom, centred, with the layer-shell margin), so this process does
            // not know its position. What must hold is the size: a screen-sized
            // surface in the overlay layer swallows every click, because its input
            // region would cover the whole screen (reported from the running build,
            // 2026-10-11).
            QWidget* host = overlay.widget()->window();
            QVERIFY(host != nullptr);
            QCOMPARE(host->size(), overlay.widget()->size());
            QVERIFY2(host->width() < available.width(),
                "the layer-shell surface must not cover the screen");
            return;
        }
        const QRect geometry = overlay.widget()->frameGeometry();
        QVERIFY2(qAbs(geometry.center().x() - available.center().x()) <= 1,
            "the pill sits horizontally centered");
        QVERIFY2(qAbs((available.bottom() - geometry.bottom()) - voicetyper::app::kOverlayBottomGapPx) <= 1,
            "the pill sits kOverlayBottomGapPx above the bottom of the working area");
    }

    void post_state_from_a_worker_thread_reaches_the_ui_thread()
    {
        voicetyper::app::QtStatusOverlay overlay;
        QVERIFY(overlay.create().is_ok());

        std::thread worker([&overlay] {
            overlay.post_state(voicetyper::platform::OverlayState::processing);
        });
        worker.join();
        QTRY_COMPARE_WITH_TIMEOUT(overlay.current_state(), voicetyper::platform::OverlayState::processing, 2000);
        QVERIFY(overlay.widget()->isVisible());
        QCOMPARE(text_of(overlay.widget())->text(), QString::fromUtf8(kProcessingText));

        // A late event after destruction must not crash and must not resurrect
        // the window.
        QVERIFY(overlay.destroy().is_ok());
        std::thread late([&overlay] { overlay.post_state(voicetyper::platform::OverlayState::recording); });
        late.join();
        QTest::qWait(100);
        QVERIFY(overlay.widget() == nullptr);
    }

    void destroy_is_terminal_and_idempotent()
    {
        voicetyper::app::QtStatusOverlay overlay;
        QVERIFY(overlay.show(voicetyper::platform::OverlayState::recording).is_ok());
        QVERIFY(overlay.destroy().is_ok());
        QVERIFY(overlay.widget() == nullptr);
        QCOMPARE(overlay.current_state(), voicetyper::platform::OverlayState::idle);
        QVERIFY(overlay.is_destroyed());

        // Idempotent, and terminal: shutdown must not be undone by a later call.
        QVERIFY(overlay.destroy().is_ok());
        QVERIFY(overlay.show(voicetyper::platform::OverlayState::processing).is_error());
        QVERIFY(overlay.hide().is_error());
        QVERIFY(overlay.widget() == nullptr);
    }
};

// Same reason as the settings UI test: this Qt build routes QTest's own logger
// to nowhere on Windows, so the pass criterion is the exit code plus a marker
// printed here.
int main(int argc, char** argv)
{
    // Deterministic on every host: the overlay's observable behaviour does not
    // need a desktop session, and CI has none.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication application(argc, argv);
    StatusOverlayTest test;
    const int failures = QTest::qExec(&test, argc, argv);
    std::cout << "ui-status-overlay-test: result=" << (failures == 0 ? "passed" : "failed")
              << " failures=" << failures << '\n';
    std::cout.flush();
    return failures;
}

#include "ui_status_overlay_test.moc"
