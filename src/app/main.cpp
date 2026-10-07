// Entry point. Windows gets the full composition with real backends; every other
// host gets the portable fallbacks so the window still opens and can show
// settings, instead of the process failing to start.

#include "domain/version.hpp"

#include <QApplication>
#include <QIcon>
#include <QString>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>

namespace voicetyper::app {
/// Assembles the real Windows backends and runs the application.
int run(int argc, char** argv);
} // namespace voicetyper::app

#ifdef _WIN32
int main(int argc, char** argv)
{
    return voicetyper::app::run(argc, argv);
}
#else
#include "app/application_font.hpp"
#include "app/main_window.hpp"
#include "app/ui_text.hpp"
#include "app/settings_presenter.hpp"
#include "platform/portable/portable_runtime.hpp"

namespace {

std::filesystem::path settings_path()
{
    if (const auto* from_env = std::getenv("VOICETYPER_SETTINGS_PATH"); from_env != nullptr
        && *from_env != '\0') {
        return std::filesystem::path(from_env);
    }
    if (const auto* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "VoiceTyper" / "settings.json";
    }
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local" / "share" / "VoiceTyper" / "settings.json";
    }
    return std::filesystem::current_path() / "settings.json";
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    // The bundled typeface, before any widget is built.
    voicetyper::app::install_application_font();
    application.setApplicationName(QStringLiteral("VoiceTyper"));
    // The product icon, embedded as a Qt resource: the window, the taskbar and the
    // tray all show the same image.
    if (const QIcon product_icon(QStringLiteral(":/assets/voiceTyper.png")); !product_icon.isNull()) {
        application.setWindowIcon(product_icon);
    }
    const auto version = voicetyper::domain::version();
    application.setApplicationVersion(QString::fromUtf8(version.data(), static_cast<int>(version.size())));
    application.setQuitOnLastWindowClosed(false);

    voicetyper::platform::PortableClock clock;
    voicetyper::platform::PortableFileSystem file_system;
    voicetyper::app::SettingsPresenter presenter(settings_path(), file_system, clock);
    const auto report = presenter.load();
    if (report.used_defaults && !report.diagnostics.empty()) {
        std::cerr << "settings: could not be read, using defaults\n";
        for (const auto& diagnostic : report.diagnostics) {
            std::cerr << "  " << diagnostic.field << ": " << diagnostic.message << '\n';
        }
    }

    voicetyper::app::WindowServices services;
    // Read at call time, so these follow a language change like the rest of the window.
    voicetyper::app::set_current_language(presenter.settings().app_language);
    // Read at call time, so these follow a language change like the rest of the window.
    services.engine_status = [] { return voicetyper::app::ui_text(voicetyper::app::UiKey::k101, voicetyper::app::current_language()); };
    services.record_hotkey_state = [] { return voicetyper::app::ui_text(voicetyper::app::UiKey::k102, voicetyper::app::current_language()); };

    voicetyper::app::MainWindow window(presenter, std::move(services));
    window.show();
    return application.exec();
}
#endif
