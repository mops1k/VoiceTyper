// UI snapshot tool: renders the settings window to PNG files, one per page.
//
// Why not a screen capture: a screen grab shows whatever window is on top (a chat
// window ended up in one), and PrintWindow missed parts of the Qt scene. QWidget::grab()
// renders the widget tree itself, so what is in the file is what the layout really
// produced - and it works head-less on Linux and Windows alike.
//
// Usage: voicetyper-ui-snapshot [output-directory] [settings-file]
//        The settings file is optional: pointing it at a real settings.json renders
//        the pages with the user's theme and values (the default is a fresh temp
//        file, which means the default - light - theme).
// Env:   QT_QPA_PLATFORM=offscreen      (required: no screen is needed)
//
// It writes <dir>/page-<Key>.png for every navigation entry and prints the file
// names, so a reviewer (or a subagent) can look at each page.

#include "app/application_font.hpp"
#include "app/main_window.hpp"
#include "app/settings_presenter.hpp"
#include "platform/portable/portable_runtime.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QWidget>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    voicetyper::app::install_application_font();

    const QString output_directory = argc > 1 ? QString::fromUtf8(argv[1])
                                              : QStringLiteral("ui-snapshots");
    if (!QDir().mkpath(output_directory)) {
        std::fprintf(stderr, "cannot create %s\n", qPrintable(output_directory));
        return 2;
    }

    voicetyper::platform::PortableClock clock;
    voicetyper::platform::PortableFileSystem file_system;
    // A settings file may be supplied to render the user's own theme and values;
    // without it a fresh temp file means the default theme.
    const bool borrowed_settings = argc > 2;
    const auto settings_path = borrowed_settings
        ? std::filesystem::path(argv[2])
        : std::filesystem::path(std::filesystem::temp_directory_path() / "voicetyper-ui-snapshot.json");
    if (!borrowed_settings) {
        std::filesystem::remove(settings_path);
    }

    voicetyper::app::SettingsPresenter presenter(settings_path, file_system, clock);
    static_cast<void>(presenter.load());

    voicetyper::app::MainWindow window(presenter, voicetyper::app::WindowServices{});
    window.resize(980, 640);
    window.show();
    QApplication::processEvents();

    int written = 0;
    for (int index = 0; index < window.page_count(); ++index) {
        window.show_page(index);
        QApplication::processEvents();
        const QString title = window.tab_title(index);
        const QString file = QDir(output_directory).filePath(
            QStringLiteral("page-%1.png").arg(index));
        const QPixmap pixmap = window.grab();
        if (!pixmap.save(file)) {
            std::fprintf(stderr, "FAIL %s\n", qPrintable(file));
            continue;
        }
        ++written;
        std::printf("OK %s %dx%d nav=%d label=%s\n", qPrintable(file), pixmap.width(), pixmap.height(),
            index, qPrintable(title));
    }

    window.hide();
    if (!borrowed_settings) {
        std::filesystem::remove(settings_path);
    }
    std::printf("snapshot: %d page(s) written to %s\n", written, qPrintable(output_directory));
    return written == window.page_count() ? 0 : 1;
}
