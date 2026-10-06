#include "app/application_font.hpp"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QString>
#include <QStringList>

namespace voicetyper::app {

void install_application_font()
{
    // Regular, Medium, SemiBold and Bold are enough for every weight the window asks
    // for (400 body, 600 headings, 700 emphasis); Qt maps the stylesheet weights onto
    // them because they all register under the one "Inter" family.
    static const QStringList kResources{
        QStringLiteral(":/fonts/Inter-Regular.ttf"),
        QStringLiteral(":/fonts/Inter-Medium.ttf"),
        QStringLiteral(":/fonts/Inter-SemiBold.ttf"),
        QStringLiteral(":/fonts/Inter-Bold.ttf"),
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
    QFont font(QStringLiteral("Inter"));
    font.setPointSize(10);
    QApplication::setFont(font);
}

} // namespace voicetyper::app
