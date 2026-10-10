#include "app/application_font.hpp"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QString>
#include <QStringList>

namespace voicetyper::app {

void install_application_font()
{
    // Regular, Semibold and Bold cover every weight the window asks for (400 body,
    // 600 headings, 700 emphasis). Selawik is Microsoft's open (SIL OFL 1.1),
    // metrically Segoe UI-compatible typeface, so one bundled file set serves both
    // platforms and the layout keeps the metrics it was measured against; Segoe UI
    // itself cannot be shipped (proprietary licence) and does not exist on Linux.
    static const QStringList kResources{
        QStringLiteral(":/fonts/Selawik-Regular.ttf"),
        QStringLiteral(":/fonts/Selawik-Semibold.ttf"),
        QStringLiteral(":/fonts/Selawik-Bold.ttf"),
    };
    bool registered = false;
    for (const QString& resource : kResources) {
        if (QFontDatabase::addApplicationFont(resource) >= 0) {
            registered = true;
        }
    }
    if (!registered) {
        return;
    }
    // The point size here is only a fallback for widgets the stylesheet does not
    // cover (menus, dialogs, tooltips); the window sets its own sizes in the sheet.
    QFont font(QStringLiteral("Selawik"));
    font.setPointSize(10);
    QApplication::setFont(font);
}

} // namespace voicetyper::app
