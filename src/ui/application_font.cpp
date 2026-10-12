#include "ui/application_font.hpp"

#include <QApplication>
#include <QDebug>
#include <QFont>
#include <QFontDatabase>
#include <QString>
#include <QStringList>

namespace voicetyper::ui {
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
    // 600 headings, 700 emphasis). Source Sans 3 (Adobe, SIL OFL 1.1) is the single
    // typeface of the window: unlike Selawik it carries Cyrillic as well as Latin, so
    // Russian and English are drawn from one bundled file set and the metrics no longer
    // depend on the host (before, Cyrillic fell through to Segoe UI on Windows and
    // DejaVu on Linux: 13.6% wider on Linux, with a larger x-height than the Latin
    // text beside it - measured 12.10.2026, Alexander's decision).
    static const QStringList kResources{
        QStringLiteral(":/fonts/SourceSans3-Regular.ttf"),
        QStringLiteral(":/fonts/SourceSans3-Semibold.ttf"),
        QStringLiteral(":/fonts/SourceSans3-Bold.ttf"),
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
    QFont font(QStringLiteral("Source Sans 3"));
    font.setPointSize(10);
    QApplication::setFont(font);
}

} // namespace voicetyper::ui
