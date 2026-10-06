#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QSize>

namespace voicetyper::app {

/// The .NET ToggleSwitch (`MainWindow.axaml:96-121`) as a Qt widget: a 40x22
/// track with an 18x18 thumb, Accent when on, `Bg.ControlHover` + `Border` when
/// off.
///
/// A checkbox is the wrong control here: Alexander asked for switch buttons
/// (2026-10-01), and the .NET build the port replaces shows switches for every
/// boolean setting.
class ToggleSwitch final : public QAbstractButton {
    Q_OBJECT

public:
    explicit ToggleSwitch(QWidget* parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

    /// Theme colours, applied from MainWindow::apply_theme().
    void set_colors(const QColor& accent, const QColor& track_off, const QColor& border, const QColor& thumb);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor accent_{QStringLiteral("#4C8BF5")};
    QColor track_off_{QStringLiteral("#3A3A3A")};
    QColor border_{QStringLiteral("#555555")};
    QColor thumb_{QStringLiteral("#9A9A9A")};
};

} // namespace voicetyper::app
