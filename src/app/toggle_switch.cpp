#include "app/toggle_switch.hpp"

#include <QPainter>
#include <QPaintEvent>

namespace voicetyper::app {

namespace {
/// `Width="40" Height="22"` and `CornerRadius="11"` from the .NET style.
constexpr int kTrackWidth = 40;
constexpr int kTrackHeight = 22;
constexpr qreal kTrackRadius = 11.0;
constexpr qreal kThumbSize = 18.0;
constexpr qreal kThumbInset = 2.0;
} // namespace

ToggleSwitch::ToggleSwitch(QWidget* parent)
    : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    // An explicit size, not only a size hint: the switch must never end up
    // invisible because a layout decided its hint was zero.
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(kTrackWidth, kTrackHeight);
}

QSize ToggleSwitch::sizeHint() const
{
    return QSize(kTrackWidth, kTrackHeight);
}

QSize ToggleSwitch::minimumSizeHint() const
{
    return sizeHint();
}

void ToggleSwitch::set_colors(const QColor& accent, const QColor& track_off, const QColor& border, const QColor& thumb)
{
    accent_ = accent;
    track_off_ = track_off;
    border_ = border;
    thumb_ = thumb;
    update();
}

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF track(0.0, (static_cast<qreal>(height()) - kTrackHeight) / 2.0,
        static_cast<qreal>(kTrackWidth), static_cast<qreal>(kTrackHeight));
    painter.setPen(QPen(isChecked() ? accent_ : border_, 1.0));
    painter.setBrush(isChecked() ? accent_ : track_off_);
    painter.drawRoundedRect(track, kTrackRadius, kTrackRadius);

    const qreal thumb_left = isChecked()
        ? track.right() - kThumbSize - kThumbInset
        : track.left() + kThumbInset;
    painter.setPen(Qt::NoPen);
    painter.setBrush(isChecked() ? QColor(Qt::white) : thumb_);
    painter.drawEllipse(QRectF(thumb_left, track.top() + kThumbInset, kThumbSize, kThumbSize));

    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(accent_, 1.0, Qt::DotLine));
        painter.drawRoundedRect(track.adjusted(-2.0, -2.0, 2.0, 2.0), kTrackRadius + 2.0, kTrackRadius + 2.0);
    }
}

} // namespace voicetyper::app
