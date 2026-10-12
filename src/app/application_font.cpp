#include "app/application_font.hpp"

#include <QApplication>
#include <QDebug>
#include <QFont>
#include <QFontDatabase>
#include <QString>
#include <QStringList>

namespace voicetyper::app {
namespace {

/// The navigation-glyph family: FluentSystemIcons (MIT, Microsoft), the cross-platform
/// redrawing of the Segoe MDL2 Assets glyphs the .NET window used. One file, one family
/// name, shipped as a Qt resource (CMakeLists.txt: voicetyper_fonts).
constexpr const char* kIconFontResource = ":/fonts/FluentSystemIcons-Regular.ttf";
constexpr const char* kIconFontFamily = "FluentSystemIcons-Regular";

} // namespace

QString icon_font_family()
{
    return QString::fromLatin1(kIconFontFamily);
}

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
    // The icon family is registered independently of the UI faces: the navigation has to
    // draw its glyphs even when the resource set of a build is incomplete, and a
    // resource that cannot be added has to be visible in the log rather than silently
    // leave the rows without icons.
    const int icon_font = QFontDatabase::addApplicationFont(QString::fromLatin1(kIconFontResource));
    if (icon_font < 0) {
        qWarning() << "icon font" << kIconFontResource
                   << "was not registered: the navigation falls back to the UI family";
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
