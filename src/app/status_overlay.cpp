// Qt status overlay. See src/app/status_overlay.hpp for the decisions this file
// implements; everything observable is asserted by tests/ui/ui_status_overlay_test.cpp.

#include "app/status_overlay.hpp"

#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <utility>

namespace voicetyper::app {
namespace {

using domain::ErrorCode;

/// The dim half of the pulse, frozen from the .NET overlay's OnPulseTick.
constexpr double kPulseLowOpacity = 0.35;

} // namespace

QtStatusOverlay::QtStatusOverlay(QObject* parent)
    : QObject(parent)
{
    pulse_timer_ = new QTimer(this);
    pulse_timer_->setInterval(static_cast<int>(platform::kOverlayPulsePeriod.count()));
    connect(pulse_timer_, &QTimer::timeout, this, [this] {
        pulse_phase_ = !pulse_phase_;
        set_pulse_opacity(pulse_phase_ ? 1.0 : kPulseLowOpacity);
    });

    // The pill follows the display the user last interacted with, so a display
    // being added or removed moves it instead of leaving it on a dead screen.
    // QCoreApplication::instance() is the only accessor guaranteed to exist
    // before QApplication is fully constructed, hence the cast.
    if (auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance()); gui != nullptr) {
        const auto reposition_now = [this] { reposition(); };
        connect(gui, &QGuiApplication::screenAdded, this, [reposition_now](QScreen*) { reposition_now(); });
        connect(gui, &QGuiApplication::screenRemoved, this, [reposition_now](QScreen*) { reposition_now(); });
        connect(gui, &QGuiApplication::primaryScreenChanged, this, [reposition_now](QScreen*) { reposition_now(); });
    }
}

QtStatusOverlay::~QtStatusOverlay()
{
    stop_pulse();
    // The pill is a top-level widget with no parent QObject, so it is owned here.
    delete pill_;
    pill_ = nullptr;
    dot_ = nullptr;
    status_text_ = nullptr;
    dot_opacity_ = nullptr;
}

platform::Status QtStatusOverlay::create()
{
    if (shutdown_.load()) {
        return platform::Status::failure(ErrorCode::invalid_state, "the status overlay has been destroyed");
    }
    if (pill_ != nullptr) {
        return platform::Status::success();
    }
    if (QGuiApplication::instance() == nullptr) {
        return platform::Status::failure(ErrorCode::unavailable, "no GUI application");
    }
    if (QThread::currentThread() != thread()) {
        return platform::Status::failure(
            ErrorCode::invalid_state, "the status overlay must be created on the UI thread");
    }

    build();
    return platform::Status::success();
}

platform::Status QtStatusOverlay::show(platform::OverlayState state, std::string_view detail)
{
    const auto created = create();
    if (created.is_error()) {
        return created;
    }
    apply_state(state, std::string(detail));
    return platform::Status::success();
}

platform::Status QtStatusOverlay::set_state(platform::OverlayState state, std::string_view detail)
{
    // The contract splits show() and set_state() for callers that already know
    // the window exists. Here they behave the same on purpose: create() is
    // idempotent, so a state change can never fail merely because the pill was
    // hidden, and every state change re-runs the geometry rule.
    return show(state, detail);
}

platform::Status QtStatusOverlay::hide()
{
    if (shutdown_.load()) {
        return platform::Status::failure(ErrorCode::invalid_state, "the status overlay has been destroyed");
    }
    if (QThread::currentThread() != thread()) {
        return platform::Status::failure(
            ErrorCode::invalid_state, "the status overlay must be hidden on the UI thread");
    }

    stop_pulse();
    if (pill_ != nullptr) {
        pill_->hide();
    }
    state_ = platform::OverlayState::idle;
    return platform::Status::success();
}

platform::OverlayState QtStatusOverlay::current_state() const noexcept
{
    return state_;
}

platform::Status QtStatusOverlay::destroy()
{
    if (QGuiApplication::instance() != nullptr && QThread::currentThread() != thread()) {
        return platform::Status::failure(
            ErrorCode::invalid_state, "the status overlay must be destroyed on the UI thread");
    }

    stop_pulse();
    delete pill_;
    pill_ = nullptr;
    dot_ = nullptr;
    status_text_ = nullptr;
    dot_opacity_ = nullptr;
    state_ = platform::OverlayState::idle;
    // Terminal by design: this is the shutdown path, and a queued worker event
    // arriving afterwards must not create a new window while the application is
    // exiting.
    shutdown_.store(true);
    return platform::Status::success();
}

void QtStatusOverlay::post_state(platform::OverlayState state, std::string detail)
{
    // Only `shutdown_` is touched here: reading the widgets from a worker thread
    // would be a data race and, worse, a Qt hard error.
    if (shutdown_.load()) {
        return;
    }
    QMetaObject::invokeMethod(
        this,
        [this, state, detail = std::move(detail)] {
            if (shutdown_.load()) {
                return;
            }
            static_cast<void>(show(state, detail));
        },
        Qt::QueuedConnection);
}

void QtStatusOverlay::build()
{
    // Qt::Tool keeps the pill out of the taskbar (and out of Alt+Tab), the
    // focus flags keep typing in the target window untouched, and
    // WA_TransparentForMouseEvents keeps clicks out of it.
    pill_ = new QWidget(nullptr,
        Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus
            | Qt::NoDropShadowWindowHint);
    pill_->setObjectName(QStringLiteral("statusOverlay"));
    pill_->setAttribute(Qt::WA_ShowWithoutActivating, true);
    pill_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    pill_->setAttribute(Qt::WA_StyledBackground, true);
    pill_->setAttribute(Qt::WA_TranslucentBackground, true);
    pill_->setFocusPolicy(Qt::NoFocus);

    auto* layout = new QHBoxLayout(pill_);
    layout->setContentsMargins(20, 11, 20, 11);
    layout->setSpacing(11);

    dot_ = new QWidget(pill_);
    dot_->setObjectName(QStringLiteral("statusOverlayDot"));
    dot_->setFixedSize(kOverlayDotSizePx, kOverlayDotSizePx);
    dot_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    dot_opacity_ = new QGraphicsOpacityEffect(dot_);
    dot_opacity_->setOpacity(1.0);
    dot_->setGraphicsEffect(dot_opacity_);
    layout->addWidget(dot_);

    status_text_ = new QLabel(pill_);
    status_text_->setObjectName(QStringLiteral("statusOverlayText"));
    status_text_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    status_text_->setMaximumWidth(kOverlayMaxTextWidthPx);
    QFont font = status_text_->font();
    font.setPixelSize(15);
    font.setWeight(QFont::DemiBold);
    status_text_->setFont(font);
    background_colour_ = QString::fromLatin1(kOverlayBackgroundColor.data(),
        static_cast<int>(kOverlayBackgroundColor.size()));
    text_colour_ = QString::fromLatin1(kOverlayTextColor.data(),
        static_cast<int>(kOverlayTextColor.size()));
    dot_colour_ = QString::fromLatin1(kOverlayRecordingAccent.data(),
        static_cast<int>(kOverlayRecordingAccent.size()));
    apply_colours();
    layout->addWidget(status_text_);

    set_accent(kOverlayRecordingAccent);
    set_pill_text(std::string(kOverlayRecordingText));
    pill_->adjustSize();
}

void QtStatusOverlay::apply_state(platform::OverlayState state, const std::string& detail)
{
    if (pill_ == nullptr) {
        return;
    }

    if (state == platform::OverlayState::idle) {
        if (state_ == platform::OverlayState::error) {
            // Deliberate: the reason for a failed dictation stays on screen until
            // the next dictation replaces it. Hiding it here would reproduce the
            // .NET behaviour of showing nothing at all.
            return;
        }
        stop_pulse();
        pill_->hide();
        state_ = platform::OverlayState::idle;
        return;
    }

    switch (state) {
    case platform::OverlayState::recording:
        set_pill_text(std::string(kOverlayRecordingText));
        set_accent(kOverlayRecordingAccent);
        start_pulse();
        break;
    case platform::OverlayState::processing:
        set_pill_text(std::string(kOverlayProcessingText));
        set_accent(kOverlayProcessingAccent);
        stop_pulse();
        break;
    case platform::OverlayState::error:
        set_pill_text(detail.empty() ? std::string(kOverlayErrorText) : detail);
        set_accent(kOverlayErrorAccent);
        stop_pulse();
        break;
    case platform::OverlayState::idle:
        break;
    }

    state_ = state;
    reposition();
    if (!pill_->isVisible()) {
        pill_->show(); // WA_ShowWithoutActivating: the user keeps typing
        reposition();  // the final size is only known after the first layout
    } else {
        pill_->raise();
    }
}

void QtStatusOverlay::set_pill_text(const std::string& text)
{
    if (status_text_ == nullptr) {
        return;
    }
    // A detail string is caller-provided (an error message), so it is elided
    // rather than allowed to stretch the pill across the screen.
    const QFontMetrics metrics(status_text_->font());
    status_text_->setText(metrics.elidedText(
        QString::fromStdString(text), Qt::ElideRight, kOverlayMaxTextWidthPx));
    if (pill_ != nullptr) {
        pill_->adjustSize();
    }
}

void QtStatusOverlay::set_accent(std::string_view accent)
{
    if (pill_ == nullptr || dot_ == nullptr) {
        return;
    }
    dot_colour_ = QString::fromLatin1(accent.data(), static_cast<int>(accent.size()));
    apply_colours();
}

void QtStatusOverlay::apply_colours()
{
    if (pill_ == nullptr || dot_ == nullptr || status_text_ == nullptr) {
        return;
    }
    // Padding and a small radius: the background must not hug the glyphs, and a fully
    // rounded pill read as a chip rather than the status line the user asked for
    // (Alexander, 2026-10-06).
    pill_->setStyleSheet(QStringLiteral("#statusOverlay { background-color: %1; border: 1.5px solid %2; "
                                        "border-radius: 8px; padding: 7px 14px; }")
                             .arg(background_colour_, dot_colour_));
    dot_->setStyleSheet(QStringLiteral("#statusOverlayDot { background-color: %1; border-radius: %2px; }")
                            .arg(dot_colour_)
                            .arg(kOverlayDotSizePx / 2));
    status_text_->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(text_colour_));
}

void QtStatusOverlay::set_theme(domain::AppTheme theme)
{
    // "System" follows the palette the application installed, which is how the window
    // resolved it too.
    const bool dark = theme == domain::AppTheme::dark
        || (theme == domain::AppTheme::system
            && QGuiApplication::palette().color(QPalette::Window).lightness() < 128);
    background_colour_ = dark ? QStringLiteral("#202020") : QStringLiteral("#FFFFFF");
    text_colour_ = dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#1B1B1B");
    apply_colours();
}

void QtStatusOverlay::reposition()
{
    if (pill_ == nullptr) {
        return;
    }
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }

    const QRect available = screen->availableGeometry();
    if (available.isEmpty()) {
        return;
    }

    pill_->adjustSize();
    const QSize size = pill_->size();
    const int x = available.x() + (available.width() - size.width()) / 2;
    const int y = available.y() + available.height() - size.height() - kOverlayBottomGapPx;
    pill_->move(x, y);
}

void QtStatusOverlay::start_pulse()
{
    if (pulse_timer_ == nullptr) {
        return;
    }
    pulse_phase_ = true;
    set_pulse_opacity(1.0);
    pulse_timer_->start();
}

void QtStatusOverlay::stop_pulse()
{
    if (pulse_timer_ != nullptr) {
        pulse_timer_->stop();
    }
    pulse_phase_ = true;
    set_pulse_opacity(1.0);
}

void QtStatusOverlay::set_pulse_opacity(double value)
{
    if (dot_opacity_ != nullptr) {
        dot_opacity_->setOpacity(value);
    }
}

} // namespace voicetyper::app
