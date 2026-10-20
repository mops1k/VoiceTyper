// Windows composition root: the single place where the real backends meet the
// UI. Everything is constructed here and nowhere else, so the order of shutdown
// is explicit and no service outlives another that it depends on.
//
// Shutdown order is the .NET one and it matters: stop the hotkeys first (no new
// session can start), cancel and stop capture, then release the engine, then
// the window services. Freeing an engine while an inference is in flight is the
// use-after-free this port must never reproduce.

#include <QCoreApplication>
#include <QStandardPaths>
#include <QMessageBox>

#include <cmath>
#include <algorithm>
#include <functional>

#include "ui/application_font.hpp"
#include "app/cleanup_wiring.hpp"
#include "app/qt_http_client.hpp"
#include "app/microphone_probe.hpp"
#include "ui/ui_text.hpp"
#include "core/support/model_download_service.hpp"
#include "core/support/update_service.hpp"
#include "platform/api/capture_guard.hpp"
#include "platform/windows/win32_update_launcher.hpp"
#include "platform/api/microphone_level.hpp"
#include "ui/main_window.hpp"
#include "ui/status_overlay.hpp"
#include "ui/tray_controller.hpp"
#include "app/settings_presenter.hpp"
#include "asr/engine_host.hpp"
#include "asr/gigaam_transcriber.hpp"
#include "asr/native_engine_registry.hpp"
#include "asr/native_transcribers.hpp"
#include "asr/silero_segmenter.hpp"
#include "domain/app_paths.hpp"
#include "domain/file_logger.hpp"
#include "domain/recording_state_machine.hpp"
#include "audio/silence_trimming_port.hpp"
#include "domain/terms_dictionary.hpp"
#include "domain/filler_words_port.hpp"
#include "domain/llm_cleanup_port.hpp"
#include "domain/terms_dictionary_port.hpp"
#include "domain/transcript_debug_port.hpp"
#include "domain/version.hpp"
#include "domain/text_output.hpp"
#include "platform/windows/win32_clock.hpp"
#include "platform/windows/win32_clipboard.hpp"
#include "platform/windows/win32_executor.hpp"
#include "platform/windows/win32_file_system.hpp"
#include "platform/windows/win32_gamepad.hpp"
#include "platform/windows/win32_hotkeys.hpp"
#include "platform/windows/win32_paste.hpp"
#include "platform/windows/win32_startup.hpp"
#include "platform/windows/win32_text_target.hpp"
#include "platform/windows/windows_audio_capture.hpp"
#include "platform/windows/windows_microphone.hpp"

#include <QApplication>
#include <QString>
#include <QIcon>
#include <QLocalSocket>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTimer>

#include <chrono>
#include <thread>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <filesystem>
#include <iostream>
#include <memory>

namespace voicetyper::app {
namespace {

/// The model pages show five Whisper sizes and four Parakeet quants; the cancel table is
/// indexed by engine and row, so it is sized for the larger of the two.
constexpr int kModelRowsPerEngine = 5;
/// One slot per (engine, row) so a transfer of one engine can never be cancelled
/// through another engine's row.
constexpr int kModelRowSlots = kModelRowsPerEngine * 3;

/// The shell's status strings follow the interface language of the settings: the window
/// re-letters itself, and these messages have to match it (Alexander, 06.10.2026).
QString localized(ui::UiKey key)
{
    return ui::ui_text(key, ui::current_language());
}

/// The marker an installer must carry to be allowed to update THIS build: the native
/// package is published as VoiceTyper-<version>-win64-Setup.exe (the frozen asset regex
/// `^VoiceTyper-\d[^/]*?-Setup\.exe$` still matches it, and the older .NET asset
/// `VoiceTyper-1.1.3-Setup.exe` does not), so the installer that replaced the .NET
/// build can never be installed over the native one by mistake.
constexpr std::string_view kNativeInstallerMarker = "win64";

/// Stable engine name for logs. A ternary chain here silently reported Parakeet
/// for every non-Whisper engine, which is exactly the kind of log that makes a
/// new engine look absent while it is running.
constexpr const char* engine_name(domain::TranscriptionEngine engine) noexcept
{
    switch (engine) {
    case domain::TranscriptionEngine::whisper: return "whisper";
    case domain::TranscriptionEngine::parakeet: return "parakeet";
    case domain::TranscriptionEngine::gigaam: return "gigaam";
    }
    return "whisper";
}

/// File name of one model row inside the models directory. The index is the row
/// index of the engine's own list, which is why the engine is required: the same
/// index means a different file in every list, and a GigaAM row must never be
/// resolved to a Parakeet quant.
std::string model_file_name_for(domain::TranscriptionEngine engine, int size_index)
{
    switch (engine) {
    case domain::TranscriptionEngine::whisper:
        return std::string(core::support::whisper_model_file_name(
            static_cast<domain::ModelSize>(size_index)));
    case domain::TranscriptionEngine::parakeet:
        return std::string(core::support::parakeet_model_file_name(
            static_cast<domain::ParakeetModelSize>(size_index)));
    case domain::TranscriptionEngine::gigaam:
        return std::string(core::support::gigaam_model_file_name(
            static_cast<domain::GigaamModelSize>(size_index)));
    }
    return {};
}

/// Which download repository serves an engine's models.
core::support::ModelEngine download_model_engine(domain::TranscriptionEngine engine)
{
    switch (engine) {
    case domain::TranscriptionEngine::whisper: return core::support::ModelEngine::whisper;
    case domain::TranscriptionEngine::parakeet: return core::support::ModelEngine::parakeet;
    case domain::TranscriptionEngine::gigaam: return core::support::ModelEngine::gigaam;
    }
    return core::support::ModelEngine::whisper;
}

std::optional<std::string> environment_variable(std::string_view name)
{
    const std::string key(name);
    const auto* value = std::getenv(key.c_str());
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

/// Directory of the running executable: the application root for native
/// libraries and the last fallback when the environment has no user profile.
std::filesystem::path executable_directory()
{
    const auto module = platform::win32::module_file_path();
    if (module.is_ok() && !module.value().empty()) {
        return std::filesystem::path(module.value()).parent_path();
    }
    return std::filesystem::current_path();
}

// The three path decisions below are overrides on top of the shared resolver.
// The default comes from domain::AppPaths, which is the single place the Windows
// layout is defined: settings in the Roaming profile (%APPDATA%), models, logs
// and updates in the Local one (%LOCALAPPDATA%). Deciding that here as well is
// how the first version of this composition put settings.json into
// %LOCALAPPDATA% and silently ignored the real user's file.
std::filesystem::path settings_path(const domain::AppPaths& paths)
{
    if (const auto override_path = environment_variable("VOICETYPER_SETTINGS_PATH");
        override_path.has_value()) {
        return std::filesystem::path(*override_path);
    }
    return paths.settings_file();
}

std::filesystem::path log_directory(const domain::AppPaths& paths)
{
    if (const auto override_path = environment_variable("VOICETYPER_LOG_DIR"); override_path.has_value()) {
        return std::filesystem::path(*override_path);
    }
    return paths.logs_directory();
}

std::filesystem::path models_directory(const domain::AppPaths& paths)
{
    if (const auto override_path = environment_variable("VOICETYPER_MODELS_DIR");
        override_path.has_value()) {
        return std::filesystem::path(*override_path);
    }
    return paths.models_directory();
}

/// Capture decorator: records what each session actually delivered.
///
/// "Recording started but nothing was transcribed" has two very different
/// causes - the machine saw an empty buffer (so it never calls the transcriber)
/// or the transcriber returned blank text (so the output port is a no-op). Both
/// look identical in the UI and in the log, and both were reported from the
/// target machine, so the numbers are logged here: sample count, duration,
/// peak and RMS of every session, plus the device and backend that were opened.
class LoggedRecordingPort final : public domain::RecordingPort {
public:
    LoggedRecordingPort(domain::RecordingPort& inner, platform::Logger& logger,
        std::string requested_device, std::function<std::string()> backend_name = {},
        platform::CaptureGuard* guard = nullptr)
        : inner_(inner)
        , logger_(logger)
        , requested_device_(std::move(requested_device))
        , backend_name_(std::move(backend_name))
        , guard_(guard)
    {
    }

    domain::Status start(const domain::CancellationToken& cancellation) override
    {
        // The elapsed time is logged because "the recording starts late" cannot be
        // checked any other way: the press is handled in milliseconds, so any delay
        // the user feels lives in this call (WASAPI create + Initialize + Start).
        const auto started = std::chrono::steady_clock::now();
        const auto status = inner_.start(cancellation);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (status.is_ok() && guard_ != nullptr) {
            // The fuse is armed for exactly as long as a device is open. If the process
            // dies from an unhandled exception, an abort or a fatal signal, the crash
            // hook releases the capture - a device left allocated inside the audio
            // driver blocks the microphone for every application on the machine.
            guard_->arm([this] { static_cast<void>(inner_.cancel()); });
        }
        static_cast<void>(logger_.write(status.is_ok() ? platform::LogLevel::info : platform::LogLevel::error,
            "capture start " + std::to_string(elapsed) + " ms",
            status.is_ok() ? ("requested device=" + requested_device_ + " backend="
                                 + (backend_name_ ? backend_name_() : std::string("unknown")))
                            : status.error().to_string()));
        return status;
    }

    domain::Result<audio::SampleBuffer> stop() override
    {
        if (guard_ != nullptr) {
            guard_->disarm();
        }
        auto result = inner_.stop();
        if (result.is_error()) {
            static_cast<void>(logger_.write(
                platform::LogLevel::error, "capture stop failed", result.error().to_string()));
            return result;
        }
        const auto& samples = result.value().samples();
        double sum_squares = 0.0;
        float peak = 0.0F;
        for (const float sample : samples) {
            sum_squares += static_cast<double>(sample) * static_cast<double>(sample);
            const float magnitude = sample < 0.0F ? -sample : sample;
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        const double rms = samples.empty() ? 0.0 : std::sqrt(sum_squares / static_cast<double>(samples.size()));
        static_cast<void>(logger_.write(samples.empty() ? platform::LogLevel::warn : platform::LogLevel::info,
            samples.empty() ? "capture delivered no samples (nothing to transcribe)" : "capture delivered audio",
            "samples=" + std::to_string(samples.size()) + " seconds="
                + std::to_string(static_cast<double>(samples.size()) / 16000.0) + " peak=" + std::to_string(peak)
                + " rms=" + std::to_string(rms) + " backend="
                + (backend_name_ ? backend_name_() : std::string("unknown"))));
        return result;
    }

    domain::Status cancel() override
    {
        if (guard_ != nullptr) {
            guard_->disarm();
        }
        return inner_.cancel();
    }

    domain::Result<audio::SampleBuffer> drain() override { return inner_.drain(); }

private:
    domain::RecordingPort& inner_;
    platform::Logger& logger_;
    std::string requested_device_;
    /// Which backend actually serves the session (native library or WASAPI): the
    /// whole point of the log line is to tell those two apart without guessing.
    std::function<std::string()> backend_name_;
    /// The fuse for the session this wrapper is running; null when the composition does
    /// not install one (tests).
    platform::CaptureGuard* guard_ = nullptr;
};


} // namespace

int run(int argc, char** argv)
{
    // --selftest is the automated launch proof: it builds every service, prints
    // what it found and exits without entering the event loop, so CI can check
    // "the app starts and can see the machine" without a human.
    const bool selftest = argc > 1 && std::string_view(argv[1]) == "--selftest";
    // Read the launch switch before QApplication gets the chance to rewrite
    // argv. The autostart entry writes exactly this switch, so an autostarted
    // app and a manually started one have to agree on the spelling.
    const bool start_minimized_switch = platform::win32::wants_start_minimized(argc, argv);
    QApplication application(argc, argv);
    // The bundled typeface, before any widget is built.
    voicetyper::ui::install_application_font();
    // The product icon, embedded as a Qt resource: the window, the taskbar and the
    // tray all show the same image.
    if (const QIcon product_icon(QStringLiteral(":/assets/voiceTyper.png")); !product_icon.isNull()) {
        application.setWindowIcon(product_icon);
    }
    if (!selftest && !ui::claim_single_instance()) {
        // A live instance already owns the microphone and the global hotkeys; starting a
        // second recorder would fight it. Ask the first one to show itself and leave. The
        // owner logs the request, so nothing is logged here (the log is not open yet either).
        static_cast<void>(ui::notify_running_instance());
        return 0;
    }
    application.setApplicationName(QStringLiteral("VoiceTyper"));
    const auto version = voicetyper::domain::version();
    application.setApplicationVersion(QString::fromUtf8(version.data(), static_cast<int>(version.size())));
    // Closing the window hides the app; the tray owns the real shutdown, exactly
    // like the .NET build where the main window may be absent entirely.
    application.setQuitOnLastWindowClosed(false);

    // Every per-user location comes from one resolver. The .NET layout is not a
    // detail: settings live in the Roaming profile and models/logs/updates in the
    // Local one (compatibility-contracts.md §2), and only the shared resolver is
    // allowed to make that decision.
    const domain::AppPaths paths(
        domain::windows_app_path_roots(environment_variable, executable_directory()));

    platform::Win32Clock clock;
    platform::Win32FileSystem file_system;
    // A real log file from the first line of the run: a silent log is how a
    // failed dictation looks like a working app.
    domain::FileLogger logger(log_directory(paths));

    // Prove the filesystem backend can actually write before anything depends on
    // it. Settings saving goes through this port, so a backend that only returns
    // "unsupported" would otherwise look like "settings did not save" with no
    // explanation at all.
    const auto probe_path = log_directory(paths) / "startup-probe.tmp";
    const auto probe = file_system.atomic_write(probe_path, "probe");
    // The probe file is evidence, not a log: leaving a stray .tmp in the log
    // directory every launch is exactly the kind of litter the D9 log policy
    // exists to prevent.
    static_cast<void>(file_system.remove_file(probe_path));
    if (probe.is_error()) {
        static_cast<void>(logger.write(
            platform::LogLevel::error,
            "filesystem backend is not functional; settings will NOT be saved",
            std::string(error_code_name(probe.code())) + ": " + probe.message()));
        std::cerr << "filesystem backend is not functional (" << error_code_name(probe.code())
                  << "): settings cannot be saved\n";
    } else {
        static_cast<void>(logger.write(
            platform::LogLevel::info,
            "filesystem backend verified",
            std::string(error_code_name(probe.code()))));
    }

    SettingsPresenter presenter(settings_path(paths), file_system, clock);
    const auto load_report = presenter.load();
    if (load_report.used_defaults) {
        // First launch (there was no settings file): open in the language of the
        // operating system, so the window speaks what the user's desktop speaks
        // (Alexander, 2026-10-06). The choice is written at once, so the next launch
        // reads a real preference instead of guessing again.
        const QLocale system_locale = QLocale::system();
        const auto language = system_locale.language() == QLocale::Russian
            ? domain::AppLanguage::ru
            : domain::AppLanguage::en;
        presenter.update(SettingsChange::language, [language](domain::AppSettings& settings) {
            settings.app_language = language;
        });
        static_cast<void>(presenter.flush());
        static_cast<void>(logger.write(platform::LogLevel::info, "first launch language",
            system_locale.name().toStdString() + " -> "
                + (language == domain::AppLanguage::ru ? "ru" : "en")));
    }

    // --- autostart -----------------------------------------------------------
    // The Run key is the machine's truth about "start with Windows", and the
    // stored setting can disagree with it: another tool or user removed the
    // value, the app was moved to another directory, a packaged build wrote
    // nothing. So the entry is *reconciled* against the setting, never merely
    // displayed from it, and a refused write is logged instead of swallowed -
    // the .NET catch-all made a failed write look exactly like a successful one.
    //
    // Two copies of the product live on a development machine at once - the
    // installed one the user works with and the build being tested - and they must
    // not share one Run value: measured 2026-10-16, a build launched while the
    // canonical value happened to be absent registered itself under it, Windows
    // started the build tree at the next logon, and the installed app could not
    // take the value back (remove_entry refuses a value naming another
    // executable). An instance therefore owns the value name of its kind:
    // `VoiceTyper` for an installed copy, `VoiceTyper-dev` for a build tree.
    const auto own_executable = platform::win32::module_file_path();
    const bool installed_instance = own_executable.is_ok()
        && platform::win32::is_installed_instance(std::filesystem::path(own_executable.value()));
    platform::win32::Win32Startup startup; // the value name follows the instance kind
    // The canonical value read-only: what it holds is what starts at the next
    // logon, so a development copy reports it even though it never writes it.
    const platform::win32::Win32Startup canonical_startup{platform::win32::StartupOptions{}};

    const auto autostart_state_text = [](const platform::win32::Win32Startup& backend) {
        const auto current = backend.read();
        if (current.is_error()) {
            return "registry=unreadable (" + current.error().to_string() + ")";
        }
        if (!current.value().present) {
            // std::string, not const char*: the other two branches return
            // std::string, and a lambda has one deduced return type.
            return std::string("registry=absent");
        }
        return "registry=registered command=\"" + platform::win32::to_log_text(current.value().command_line)
            + "\"";
    };

    // Who asked for the write. `user_change` is the explicit settings change - the
    // user's word, which may also remove or repoint a value naming another
    // executable. `launch` is the app starting: it reconciles, and never
    // overwrites a living entry that belongs to another copy.
    enum class AutostartTrigger { launch, user_change };

    const auto apply_autostart = [&startup, &canonical_startup, &installed_instance, &own_executable,
                                     &logger, &autostart_state_text](
                                    const domain::AppSettings& applied, AutostartTrigger trigger) {
        const std::string requested = std::string("start_with_windows=")
            + (applied.start_with_windows ? "true" : "false") + " start_minimized="
            + (applied.start_minimized ? "true" : "false");
        const bool user_change = trigger == AutostartTrigger::user_change;

        if (!installed_instance && !user_change) {
            // A development copy registers nothing at launch and never touches the
            // canonical value: a build tree starting at logon in place of the
            // installed app is exactly what this split exists to prevent. It only
            // reports, and shouts when the canonical value names this very build.
            static_cast<void>(logger.write(platform::LogLevel::info,
                "autostart entry left to the installed copy",
                "instance=development entry=\"" + platform::win32::to_log_text(startup.value_name())
                    + "\" requested=" + (applied.start_with_windows ? "on" : "off")
                    + " canonical=" + autostart_state_text(canonical_startup)));
            const auto canonical_entry = canonical_startup.read();
            if (canonical_entry.is_ok() && canonical_entry.value().present && own_executable.is_ok()
                && platform::win32::command_line_names(
                    canonical_entry.value().command_line, own_executable.value())) {
                static_cast<void>(logger.write(platform::LogLevel::warn,
                    "the installed copy's autostart entry names this build tree",
                    "canonical=\"" + platform::win32::to_log_text(canonical_entry.value().command_line)
                        + "\" this=\"" + platform::win32::to_log_text(own_executable.value())
                        + "\" (" + localized(ui::UiKey::k14).toStdString()
                        + " in the installed app gives the entry back to it)"));
            }
            return;
        }

        if (installed_instance && trigger == AutostartTrigger::launch && applied.start_with_windows) {
            const auto current = startup.read();
            if (current.is_ok() && current.value().present && own_executable.is_ok()) {
                const std::wstring target = platform::win32::command_line_executable(current.value().command_line);
                std::error_code exists_error;
                const bool target_exists = !target.empty()
                    && std::filesystem::exists(std::filesystem::path(target), exists_error)
                    && !exists_error;
                if (!platform::win32::launch_may_rewrite(
                        current.value().command_line, own_executable.value(), target_exists)) {
                    static_cast<void>(logger.write(
                        platform::LogLevel::warn,
                        "autostart entry belongs to another installation and was left alone",
                        "registry=\"" + platform::win32::to_log_text(current.value().command_line)
                            + "\" this=\"" + platform::win32::to_log_text(own_executable.value())
                            + "\" (" + localized(ui::UiKey::k14).toStdString()
                            + " to re-register it for this build)"));
                    return;
                }
            }
        }

        // The explicit settings change may also remove or repoint a value that
        // names another executable; a launch never does (and a development copy
        // never does either, so a build cannot disable the installed copy's
        // autostart).
        const auto status = startup.set_enabled(applied.start_with_windows, applied.start_minimized,
            /*override_owner=*/user_change && installed_instance);
        if (status.is_error()) {
            static_cast<void>(logger.write(
                platform::LogLevel::error,
                "autostart entry was not applied",
                requested + " -> " + status.error().to_string()));
            return;
        }
        static_cast<void>(logger.write(platform::LogLevel::info,
            "autostart entry applied",
            requested + " -> " + autostart_state_text(startup)));
    };

    // --- text output ---------------------------------------------------------
    platform::Win32Clipboard clipboard;
    platform::Win32PasteSimulator paste;
    // Direct delivery (VT-DOM-613): the probe recognises a text field and the
    // injector types into it, so a dictation leaves the clipboard alone whenever
    // the field can be recognised.
    platform::Win32TextTargetProbe focus_probe;
    platform::Win32DirectTextInjector direct_injector;
    // VT-PLT-516: the paste chord is only a request, so the reader is what lets the
    // service tell a real paste from a window that accepted the chord and ignored it.
    platform::Win32FocusedTextField field_reader;
    platform::Win32Executor ui_executor;
    // Every report a service hands back to the window must run on the interface
    // thread: the progress of an update download is delivered on a worker, and
    // touching a QWidget from there produced "QWidget::repaint: Recursive repaint
    // detected" and killed the process in the middle of the download, so the
    // installer never ran (reproduced 2026-10-12). The Linux composition has
    // marshalled its reports this way from the start; this is the same hand-off.
    const auto post_to_ui = [&application](std::function<void()> task) {
        QMetaObject::invokeMethod(
            &application, [task = std::move(task)] { task(); }, Qt::QueuedConnection);
    };
    domain::RetryingClipboard retrying(clipboard, clock);
    domain::TextOutputService output(retrying, paste, focus_probe, direct_injector, field_reader, clock, ui_executor);
    // Why the text did or did not reach the document. "Pasted" and "only on the
    // clipboard because the focused window refused it" look identical in the UI,
    // and the target machine reported "it did not paste into the input" without
    // this line there was nothing to check it against.
    /// VT-DOM-618: tells the user the transcript is on the clipboard because the app
    /// could not confirm the paste. Assigned once the tray exists.
    std::function<void()> announce_clipboard_only;
    output.set_report_sink([&logger, post_to_ui, &announce_clipboard_only](
                               const domain::OutputReport& report) {
        const std::string_view outcome = platform::text_output_outcome_name(report.outcome);
        const std::string detail = (report.paste.has_value()
            ? (std::string(report.paste->reason) + " (injection_supported="
                  + (report.paste->injection_supported ? "true" : "false")
                  + ", direct=" + std::string(report.paste->direct) + ")")
            : std::string())
            + " duration_ms=" + std::to_string(report.duration_ms)
            + " probe_ms=" + std::to_string(report.probe_ms)
            + " insert_ms=" + std::to_string(report.insert_ms)
            + " paste_ms=" + std::to_string(report.paste_ms)
            + " confirm_ms=" + std::to_string(report.confirm_ms)
            + " leading_space=" + (report.leading_space ? "true" : "false");
        platform::LogLevel level = platform::LogLevel::info;
        if (report.outcome == platform::TextOutputOutcome::clipboard_failed) {
            level = platform::LogLevel::error;
        } else if (report.outcome == platform::TextOutputOutcome::clipboard_only) {
            level = platform::LogLevel::warn;
        }
        static_cast<void>(logger.write(level, "text output " + std::string(outcome), detail));
        // VT-DOM-618: the text is on the clipboard and the app could not see it in
        // the field, so the user has to be told where it is. Silence here is exactly
        // what made the Windows Terminal case look like a lost dictation.
        if (report.outcome == platform::TextOutputOutcome::clipboard_only && report.paste.has_value()
            && report.paste->reason == "paste_unconfirmed") {
            post_to_ui([&announce_clipboard_only] {
                if (announce_clipboard_only) {
                    announce_clipboard_only();
                }
            });
        }
    });

    // --- capture -------------------------------------------------------------
    platform::WindowsMicrophone microphone;
    auto devices = microphone.list_devices();
    if (devices.empty()) {
        // "devices=0" alone is not diagnosable: the backend knows whether COM was
        // unavailable, the enumerator could not be created, the endpoint
        // collection was empty, or the names were hidden. That reason is what
        // separates "no microphone attached" from "this session cannot see one".
        static_cast<void>(logger.write(
            platform::LogLevel::warn,
            "no microphone was enumerated",
            microphone.diagnostics().empty() ? std::string("the backend reported no reason")
                                             : microphone.diagnostics()));
    } else {
        // The device names matter in the log because the settings page shows what
        // this list contains: a report of "the microphone dropdown shows a GUID
        // instead of a name" is otherwise unverifiable from the outside.
        for (const auto& device : devices) {
            static_cast<void>(logger.write(
                platform::LogLevel::info,
                "microphone available",
                "name=\"" + device.name + "\" id=" + device.id
                    + (device.is_default ? " default=yes" : " default=no")));
        }
    }
    const auto selected = presenter.settings().microphone_device_id;
    // The microphone release fuse (platform/api/capture_guard.hpp): armed while a
    // device is open, released by the crash hook if this process dies unexpectedly.
    platform::CaptureGuard capture_guard;
    platform::install_crash_release_hook(capture_guard);

    platform::WindowsAudioCapture::Options capture_options;
    if (selected.has_value()) {
        capture_options.device_id = *selected;
    }
    platform::WindowsAudioCapture capture(capture_options);

    // Warm the capture endpoint once, before any dictation. The first WASAPI
    // start on this machine costs about a second - the microphone array wakes out
    // of a low-power state and the format is negotiated - so the first press of
    // the record hotkey lost the beginning of the speech: measured 2026-10-01, a
    // 4 s push-to-talk hold delivered only 3.01 s of audio, and the user reported
    // "the microphone does not start right after I press the hotkey". Warming it
    // here pays that cost once at startup instead of at the start of a dictation.
    {
        const auto warm_started = std::chrono::steady_clock::now();
        const auto warm_status = capture.start(domain::CancellationToken{});
        if (warm_status.is_ok()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            static_cast<void>(capture.stop());
        }
        const auto warm_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - warm_started).count();
        static_cast<void>(logger.write(
            warm_status.is_ok() ? platform::LogLevel::info : platform::LogLevel::warn,
            "capture warm-up " + std::to_string(warm_ms) + " ms",
            warm_status.is_ok() ? std::string("the endpoint is awake before the first hotkey")
                                : warm_status.error().to_string()));
    }

    // --- engine --------------------------------------------------------------
    // The overlay is declared before the machine (and before the engine status
    // timer) on purpose: stack objects are destroyed in reverse order, so every
    // service that posts to it is destroyed first and can never call into a freed
    // overlay.
    ui::QtStatusOverlay status_overlay;
    // The language the shell speaks before the first change arrives.
    ui::set_current_language(presenter.settings().app_language);
    /// Set once the tray exists: the menu is created later in this function, so the
    /// language change reaches it through this hook (the same pattern used for the
    /// window before the language was applied in place).
    std::function<void(domain::AppLanguage)> apply_language_to_tray;
    /// Set once the tray exists: the check of the launch announces a found update through
    /// this hook. The tray is created much later in this function, so the check cannot
    /// capture it, and the announcement has to happen on the interface thread anyway.
    std::function<void(const QString& version)> announce_update;
    /// Set once the window exists: the check of the launch hands its answer to the
    /// About page through this hook, which is created much later in this function
    /// (VT-UI-606), so the check cannot capture the window itself.
    std::function<void()> notify_window_of_update;
    // The pill has to be readable in both themes; the global stylesheet made it white
    // with light grey text in the light one.
    status_overlay.set_theme(presenter.settings().theme);
    // At startup as well, not only on a change: an interface already in English was
    // showing the Russian "Захват" because the overlay only heard about the language when
    // it happened to change (Alexander, 06.10.2026).
    status_overlay.set_language(presenter.settings().app_language);
    // VAD segmenter: it drives the VAD auto-stop AND cuts a dictation longer than
    // a model's input window (GigaAM is trained on ~25 s). It has to exist before
    // the engine registry below, because the GigaAM factory borrows it and that
    // factory can run as soon as the first engine is selected.
    // Silero is the product's detector from now on: it drives the VAD auto-stop,
    // the chunk cuts of a dictation longer than a model window and the silence
    // trimming below. The energy heuristic stays as an EXPLICITLY logged fallback
    // when the model file is missing - the same rule as the engines: a substitution
    // is reported, never silent.
    audio::EnergySpeechSegmenter energy_segmenter;
    std::unique_ptr<asr::SileroSegmenter> silero_segmenter;
    {
        const auto vad_model = models_directory(paths) / "ggml-silero-v6.2.0.bin";
        auto opened = asr::SileroSegmenter::open(vad_model);
        if (opened.is_ok()) {
            silero_segmenter = std::move(opened).value();
            static_cast<void>(logger.write(platform::LogLevel::info, "VAD detector",
                "silero " + vad_model.filename().string()));
        } else {
            static_cast<void>(logger.write(platform::LogLevel::warn, "VAD detector",
                "silero unavailable (" + opened.error().message()
                    + "), falling back to the energy detector"));
        }
    }
    audio::SpeechSegmenter& segmenter = silero_segmenter != nullptr
        ? static_cast<audio::SpeechSegmenter&>(*silero_segmenter)
        : static_cast<audio::SpeechSegmenter&>(energy_segmenter);
    asr::NativeEngineRegistryOptions registry_options;
    registry_options.whisper_available = true;
    registry_options.whisper_factory = [](const std::filesystem::path& model) {
        return asr::make_whisper_engine(model);
    };
    registry_options.parakeet_library = platform::parakeet_library_beside_executable();
    // The model path is forwarded, not dropped. The first version of this line
    // ignored the second argument and called make_parakeet_engine(dll, {}), so
    // every launch whose settings selected Parakeet failed with "parakeet: empty
    // model path" while Whisper worked: a whole shipped engine was dead and the
    // UI only showed a failed engine state.
    registry_options.parakeet_factory = [](const std::filesystem::path& dll,
                                            const std::filesystem::path& model) {
        return asr::make_parakeet_engine(dll, model);
    };
    registry_options.parakeet_probe = [](domain::TranscriptionEngine engine, const std::filesystem::path& model) {
        return asr::probe_parakeet_for_registry(engine, model);
    };
    // GigaAM through the pinned transcribe.cpp runtime. The library is probed
    // (version + struct sizes of the vendored header) and the model path is
    // forwarded, exactly like Parakeet above; the segmenter is borrowed so a
    // dictation longer than the model window is cut at a pause.
    registry_options.gigaam_library = asr::transcribe_library_beside_executable();
    registry_options.gigaam_factory = [&segmenter](const std::filesystem::path& dll,
                                                   const std::filesystem::path& model) {
        return asr::make_gigaam_engine(dll, model, &segmenter);
    };
    registry_options.gigaam_probe = [](domain::TranscriptionEngine engine, const std::filesystem::path& model) {
        return asr::probe_gigaam_for_registry(engine, model, asr::transcribe_library_beside_executable());
    };
    asr::NativeEngineRegistry registry(std::move(registry_options));
    asr::EngineHost engine_host(registry);

    const auto& settings = presenter.settings();
    // The engine is the one the user chose, not the one the code defaults to: a
    // stored Parakeet setting used to load Whisper at launch while the UI showed
    // the user's choice, which is exactly the silent substitution the contract
    // forbids.
    const auto startup_engine = settings.transcription_engine;
    const auto startup_model = startup_engine == domain::TranscriptionEngine::parakeet
        ? models_directory(paths) / std::filesystem::path(std::string(core::support::parakeet_model_file_name(settings.parakeet_model_size)))
        : startup_engine == domain::TranscriptionEngine::gigaam
            ? models_directory(paths) / std::filesystem::path(std::string(core::support::gigaam_model_file_name(settings.gigaam_model_size)))
            : models_directory(paths) / std::filesystem::path(std::string(core::support::whisper_model_file_name(settings.model_size)));
    const auto* model_override = std::getenv("VOICETYPER_WHISPER_MODEL");

    if (startup_engine == domain::TranscriptionEngine::whisper
        && model_override != nullptr && *model_override != '\0') {
        static_cast<void>(registry.set_model_path(startup_engine, model_override));
        engine_host.select(startup_engine, model_override);
    } else {
        static_cast<void>(registry.set_model_path(startup_engine, startup_model));
        engine_host.select(startup_engine, startup_model);
    }

    // Engine readiness is otherwise only visible in the window, so "распознавание
    // не работает" had no line in the log a user can send. Each transition is
    // logged once, and a terminal state is also shown on the overlay: a missing
    // model or an engine that cannot be loaded is exactly the recoverable problem
    // the overlay's error state exists for.
    asr::EngineReadiness last_readiness = engine_host.state().readiness;
    QTimer engine_status_timer;
    engine_status_timer.setInterval(500);
    QObject::connect(&engine_status_timer, &QTimer::timeout, &application, [&] {
        const auto snapshot = engine_host.state();
        if (snapshot.readiness == last_readiness) {
            return;
        }
        last_readiness = snapshot.readiness;
        const std::string state_name(asr::engine_readiness_name(snapshot.readiness));
        std::string detail;
        if (snapshot.readiness == asr::EngineReadiness::unavailable) {
            detail = std::string(platform::engine_availability_reason_name(snapshot.reason));
        } else if (!snapshot.last_error.empty()) {
            detail = snapshot.last_error;
        }
        const bool terminal = snapshot.readiness == asr::EngineReadiness::failed
            || snapshot.readiness == asr::EngineReadiness::unavailable
            || snapshot.readiness == asr::EngineReadiness::model_missing;
        static_cast<void>(logger.write(
            terminal ? platform::LogLevel::error : platform::LogLevel::info,
            "engine state=" + state_name, detail));
        if (terminal) {
            // An error never reaches the overlay (reported 2026-10-11): the status
            // line and the log carry it, and the pill simply goes away.
            status_overlay.post_state(platform::OverlayState::idle);
        }
    });
    engine_status_timer.start();

    // --- recording state machine --------------------------------------------
    // VAD mode needs a segmenter or recording never auto-stops. The instance is
    // declared above the engine registry (the GigaAM factory borrows it); this is
    // still the energy heuristic rather than Silero, which is the next step of
    // the plan (phase V) - the seam is already the same.
    domain::ThreadRecordingWorker worker;
    domain::RecordingStateMachineOptions machine_options;
    machine_options.mode = settings.recording_mode;
    machine_options.vad_silence_threshold_seconds =
        static_cast<double>(settings.silence_threshold_ms) / 1000.0;
    LoggedRecordingPort logged_capture(capture, logger,
        selected.has_value() ? *selected : std::string("(system default)"),
        [&capture] { return capture.backend_description(); }, &capture_guard);
    // The dictionary is applied to the engine's text by a decorator, not inside an
    // engine: the explicit "as heard=as written" pairs are the only dictionary
    // mechanism that reaches all three engines (measured 2026-10-07 - GigaAM
    // reports no vocabulary support and ignores a context prompt, and the
    // Parakeet C API has neither).
    // The trimming decorator sits between the machine and the engine, so every
    // engine receives audio without the silence around the dictation and without
    // the long pauses inside it. The dictionary decorator stays outside it: it
    // rewrites the text that comes back.
    // Amended LOG-01 (Alexander, 13.10.2026): a Debug build records what an engine
    // actually returned, before any decorator rewrites it, because that is the only
    // way to tell "the engine dropped the filler itself" from "the terms dictionary
    // replaced it". A Release build leaves debug_transcripts null and the logging
    // block is not compiled in at all.
    platform::Logger* debug_transcripts = nullptr;
#if defined(VOICETYPER_DEBUG_TRANSCRIPTS)
    platform::LoggerOptions debug_transcript_options;
    debug_transcript_options.file_name = std::string(domain::kDebugTranscriptFileName);
    domain::FileLogger debug_transcript_logger(log_directory(paths), debug_transcript_options);
    debug_transcripts = &debug_transcript_logger;
#endif
    domain::TranscriptDebugPort raw_transcript_port(engine_host, debug_transcripts, "engine-raw");
    audio::SilenceTrimmingPort silence_port(raw_transcript_port, segmenter);
    domain::TermsDictionaryPort dictionary_port(
        silence_port, [&presenter] { return presenter.settings().terms_dictionary; });
    // The filler filter wraps the dictionary port, not the engine: the dictionary
    // restores the spellings the user wrote, and only then does the filter read the
    // text (plan p_ffecadcca9af, decided 2026-10-14). It is off by default, and both
    // settings are read per call, exactly like the dictionary's.
    domain::FillerWordsPort filler_port(dictionary_port, [&presenter] {
        return domain::FillerWordsPort::Settings{presenter.settings().filler_words_level,
            domain::parse_filler_words(presenter.settings().filler_words)};
    });
    // The optional local cleaner runs last (plan p_5d68d66503ca): the model rewrites text the
    // dictionary has already corrected and the filler rules have already cleaned, and every
    // failure mode - no file, load error, timeout, a refused answer - ends with exactly that
    // text going on. cleanupModel is off by default, and make_cleanup_runtime() picks the pinned
    // llama.cpp runtime when the build carries it and the null one otherwise.
    auto cleanup_runtime = app::make_cleanup_runtime();
    domain::LlmCleanupPort cleanup_port(filler_port, *cleanup_runtime, [&presenter, &paths] {
        const auto chosen = presenter.settings().cleanup_model;
        // The user's own filler words travel with the choice: the guard must not refuse an answer
        // for dropping a word the user asked to drop (17.10.2026).
        return domain::LlmCleanupPort::Settings{
            chosen, app::cleanup_model_path(chosen, models_directory(paths)),
            presenter.settings().cleanup_prompt,
            domain::parse_filler_words(presenter.settings().filler_words)};
    });
    domain::RecordingStateMachine machine(
        logged_capture, cleanup_port, output, worker, machine_options, &segmenter);
    // The machine reads its per-session parameters and its mode from the live
    // settings, exactly like the .NET machine asking the settings view model.
    // Without this every field the user changes - language, temperature, terms
    // dictionary, auto-paste, recording mode, silence threshold - would be saved
    // correctly and then silently ignored at dictation time.
    // Every state change and every failure is surfaced. A dictation that fails
    // because the microphone was taken by another app must say so: the .NET app
    // showed a notification, and a silent failure here would look like the hotkey
    // simply not working.
    const auto describe_state = [](domain::RecordingState state) {
        switch (state) {
        case domain::RecordingState::idle: return "idle";
        case domain::RecordingState::recording: return "recording";
        case domain::RecordingState::processing: return "processing";
        }
        return "unknown";
    };
    // The window does not exist yet, so the channel is created here and adopted
    // by the window below. Posting to a null target is a no-op, so an event that
    // fires during startup is simply not shown instead of crashing.
    auto status_channel = std::make_shared<ui::StatusChannel>(nullptr);
    machine.set_state_changed_handler(
        [&logger, status_channel, &status_overlay, describe_state](domain::RecordingState state) {
            const std::string name = describe_state(state);
            static_cast<void>(logger.write(
                platform::LogLevel::info, std::string("recording state=") + name));
            // The state name is localized: the strip used to read "запись: idle", mixing a
            // Russian prefix with the enum's English name (Alexander, 08.10.2026).
            const QString localized_name = localized(state == domain::RecordingState::recording
                    ? ui::UiKey::k180
                    : state == domain::RecordingState::processing ? ui::UiKey::k7 : ui::UiKey::k179);
            status_channel->post(localized(ui::UiKey::k84) + QStringLiteral(": ") + localized_name);
            // The overlay is the frameless "Захват"/"Распознавание" indicator.
            // post_state is queued onto the UI thread: this handler runs on the
            // recording worker, and a widget must never be touched from there.
            status_overlay.post_state(state == domain::RecordingState::recording
                    ? platform::OverlayState::recording
                    : state == domain::RecordingState::processing ? platform::OverlayState::processing
                                                                  : platform::OverlayState::idle);
        });
    machine.set_failed_handler([&logger, status_channel, &status_overlay](const domain::Error& error) {
        // The transcript is never written to the log; only the classification.
        const std::string detail =
            std::string(error_code_name(error.code())) + ": " + error.message();
        static_cast<void>(logger.write(platform::LogLevel::error, "recording failed", detail));
        // A capture that cannot start is nearly always the device: Alexander hit exactly
        // this - the headset was disconnected and the app answered "io_failure: no usable
        // wasapi capture configuration", which tells the user nothing. Say what can be
        // acted on; the raw detail stays in the log and on the overlay.
        const bool device_problem = error.code() == domain::ErrorCode::io_failure;
        const QString device_message = localized(ui::UiKey::k97);
        status_channel->post(device_problem
                ? device_message
                : localized(ui::UiKey::k85) + QStringLiteral(": ")
                    + QString::fromStdString(std::string(error_code_name(error.code()))));
        // The overlay never shows an error: the status line above and the log carry
        // it. A pill that stayed on screen would sit on top of the window the user
        // is typing into, so the overlay is hidden instead (reported 2026-10-11).
        status_overlay.post_state(platform::OverlayState::idle);
    });
    machine.set_text_ready_handler([&logger, status_channel, &silence_port, &engine_host, debug_transcripts](std::string text) {
        // Amended LOG-01: the finished text is recorded next to the raw engine
        // output, so a measurement can tell which of the two changed what. In a
        // Release build this is a no-op with no logging code in the binary.
        domain::log_debug_transcript(debug_transcripts, "final", text);
        // Length only. voiceTyper.log must never contain recognised text (LOG-01).
        // What the trimming step did, in numbers only: the log never contains the
        // text (LOG-01), but "why was this dictation slow" and "did the silence
        // actually go away" have to be answerable.
        const auto& trimming = silence_port.last_report();
        static_cast<void>(logger.write(
            platform::LogLevel::info,
            "silence trimmed",
            "segments=" + std::to_string(trimming.speech_segments)
                + " removed_leading=" + std::to_string(trimming.removed_leading)
                + " removed_trailing=" + std::to_string(trimming.removed_trailing)
                + " compressed_pause=" + std::to_string(trimming.compressed_pause_samples)
                + " detect_ms=" + std::to_string(trimming.detect_ms)));
        // The split that makes a slow dictation diagnosable: how long the engine
        // itself took over how much audio, and how many sequential decodes it
        // needed (more than one means the dictation was cut to the model window).
        const auto inference = engine_host.last_inference();
        static_cast<void>(logger.write(
            platform::LogLevel::info,
            "engine inference",
            "audio_seconds=" + std::to_string(inference.audio_seconds)
                + " duration_ms=" + std::to_string(inference.duration_ms)
                + " chunks=" + std::to_string(inference.chunks)
                + " window_seconds=" + std::to_string(inference.window_seconds)));
        static_cast<void>(logger.write(
            platform::LogLevel::info,
            "text delivered",
            "characters=" + std::to_string(text.size())));
        static_cast<void>(text.size());
        status_channel->post(localized(ui::UiKey::k86));
    });

    machine.set_options_provider([&presenter, &output] {
        const auto& current = presenter.settings();
        // Direct delivery is a live setting too (VT-DOM-617): read per delivery so
        // the switch in the settings window takes effect on the next dictation.
        output.set_direct_insert(current.direct_insert);
        audio::SessionOptions options;
        // The setting the Models page calls "noise reduction".
        options.noise_suppression = presenter.settings().noise_reduction_enabled;
        options.language = current.language;
        // The prompt is the terms dictionary turned into a sentence: an engine
        // that declares no prompt support ignores it (Whisper is the only one that
        // has it today), and the explicit pairs are applied afterwards by
        // TermsDictionaryPort for every engine.
        options.prompt =
            domain::terms_initial_prompt(domain::parse_terms_dictionary(current.terms_dictionary));
        options.temperature = current.temperature;
        // The Models page control. Clamped here as well as in the UI, because a
        // hand-edited settings.json can carry any integer and the engine rejects
        // best_of outside kMinBestOf..kMaxBestOf instead of clamping it.
        options.best_of = std::clamp(current.best_of, domain::kBestOfMin, domain::kBestOfMax);
        options.condition_on_previous_text = current.condition_on_previous_text;
        options.auto_paste = current.auto_paste_enabled;
        return options;
    });
    machine.set_live_settings_provider([&presenter] {
        const auto& current = presenter.settings();
        return domain::RecordingStateMachine::LiveSettings{
            current.recording_mode,
            static_cast<double>(current.silence_threshold_ms) / 1000.0,
        };
    });

    platform::win32::Win32HotkeyService hotkeys;
    static_cast<void>(hotkeys.set_event_sink([&machine, &logger](platform::HotkeyAction action) {
        // Every edge is logged: push-to-talk is a press *and* a release, and a
        // missing release is indistinguishable from "the hotkey does nothing"
        // unless the log shows which edge arrived.
        switch (action) {
        case platform::HotkeyAction::record_pressed: {
            const auto status = machine.press_record();
            static_cast<void>(logger.write(status.is_ok() ? platform::LogLevel::info : platform::LogLevel::warn,
                "hotkey record pressed", status.is_ok() ? std::string() : status.error().to_string()));
            break;
        }
        case platform::HotkeyAction::record_released: {
            const auto status = machine.release_record();
            static_cast<void>(logger.write(status.is_ok() ? platform::LogLevel::info : platform::LogLevel::warn,
                "hotkey record released", status.is_ok() ? std::string() : status.error().to_string()));
            break;
        }
        case platform::HotkeyAction::cancel_pressed:
            machine.cancel();
            static_cast<void>(logger.write(platform::LogLevel::info, "hotkey cancel pressed"));
            break;
        }
    }));
    static_cast<void>(logger.write(
        platform::LogLevel::info,
        "composition ready",
        "engine=" + std::string(asr::engine_readiness_name(engine_host.state().readiness))
            + " devices=" + std::to_string(devices.size())));

    const auto registration = hotkeys.apply_settings(presenter.settings());
    if (registration.is_error()) {
        static_cast<void>(logger.write(
            platform::LogLevel::error, "hotkey registration failed", registration.error().message()));
        std::cerr << "hotkeys: " << registration.error().message() << '\n';
    } else {
        for (const auto& error : registration.value().errors()) {
            // Logged, not only printed: a refused hotkey used to leave no trace in
            // the log, so "the hotkey does nothing" had no evidence at all.
            static_cast<void>(
                logger.write(platform::LogLevel::warn, "hotkey was not registered", error));
            std::cerr << "hotkey: " << error << '\n';
        }
    }

    // --- gamepad -------------------------------------------------------------
    // The same Win32GamepadService the portable contract test drives, wired the
    // way the Linux composition wires its backend (VT-PLT-414): the edges reach
    // the same recording machine as the hotkeys, so a bound button is a second
    // push-to-talk. No controller attached is not an error: the poll stays idle
    // and the settings page shows empty readouts.
    platform::win32::Win32GamepadService gamepad;
    static_cast<void>(gamepad.set_event_sink([&machine, &logger](const platform::GamepadEdge& edge) {
        switch (edge.action) {
        case platform::GamepadAction::record_pressed: {
            const auto status = machine.press_record();
            static_cast<void>(logger.write(status.is_ok() ? platform::LogLevel::info : platform::LogLevel::warn,
                "gamepad record pressed", status.is_ok() ? std::string() : status.error().to_string()));
            break;
        }
        case platform::GamepadAction::record_released: {
            const auto status = machine.release_record();
            static_cast<void>(logger.write(status.is_ok() ? platform::LogLevel::info : platform::LogLevel::warn,
                "gamepad record released", status.is_ok() ? std::string() : status.error().to_string()));
            break;
        }
        case platform::GamepadAction::cancel_pressed:
            machine.cancel();
            static_cast<void>(logger.write(platform::LogLevel::info, "gamepad cancel pressed"));
            break;
        }
    }));
    const auto gamepad_status = gamepad.apply_settings(presenter.settings());
    if (gamepad_status.is_error()) {
        // A present but malformed binding is the only failure here; the user sees
        // it in the settings field and the log keeps the reason.
        static_cast<void>(logger.write(platform::LogLevel::warn, "gamepad settings refused",
            gamepad_status.error().to_string()));
        std::cerr << "gamepad: " << gamepad_status.error().to_string() << '\n';
    } else {
        // Which XInput runtime answered is the fact that separates "no controller"
        // from "this machine has no XInput at all"; without it the log could not
        // explain a gamepad that never fires.
        const std::string runtime = platform::win32::xinput_runtime_name();
        static_cast<void>(logger.write(platform::LogLevel::info, "gamepad",
            runtime.empty()
                ? std::string("no XInput runtime: ") + platform::win32::xinput_runtime_failure()
                : "XInput runtime " + runtime));
    }

    ui::WindowServices services;
    // One report, written by the check of the launch and read by the About page: the page
    // shows a found update without the user pressing anything (VT-UI-606). Both writes and
    // reads happen on the interface thread - the check marshals its answer there.
    auto startup_update = std::make_shared<ui::WindowServices::StartupUpdate>();
    services.startup_update = startup_update;
    // The footer dot is green only when the engine can really work: a missing model, a load
    // in progress or a failed start all keep it red (Alexander, 08.10.2026).
    services.engine_ready = [&engine_host] {
        return engine_host.state().readiness == asr::EngineReadiness::ready;
    };
    services.engine_status = [&engine_host] {
        const auto snapshot = engine_host.state();
        switch (snapshot.readiness) {
        case asr::EngineReadiness::ready:
            return localized(ui::UiKey::k87);
        case asr::EngineReadiness::loading:
            return localized(ui::UiKey::k88);
        case asr::EngineReadiness::warming:
            return localized(ui::UiKey::k89);
        case asr::EngineReadiness::model_missing:
            return localized(ui::UiKey::k90);
        case asr::EngineReadiness::unavailable:
            return localized(ui::UiKey::k91) + QStringLiteral(": ")
                .arg(QString::fromStdString(std::string(
                    platform::engine_availability_reason_name(snapshot.reason))));
        case asr::EngineReadiness::failed:
            return localized(ui::UiKey::k92) + QStringLiteral(": ")
                + QString::fromStdString(snapshot.last_error);
        default:
            return localized(ui::UiKey::k93);
        }
    };
    services.record_hotkey_state = [&hotkeys, &presenter] {
        const auto& current = presenter.settings();
        return hotkeys.record_key_code() != 0
            ? localized(ui::UiKey::k94) + QStringLiteral(": ")
                + QString::fromStdString(current.record_hotkey)
            : localized(ui::UiKey::k95);
    };
    services.microphones = [&microphone] {
        std::vector<std::pair<std::string, std::string>> result;
        for (const auto& device : microphone.list_devices()) {
            result.emplace_back(device.id, device.name);
        }
        return result;
    };
    // One extra worker for hotkey capture: capture_next() blocks until the user
    // presses a combination, and neither the UI thread nor the recording worker may
    // ever be parked like that.
    platform::Win32Executor capture_executor;
    // A gamepad capture waits for a button press too, and it must not queue behind
    // a hotkey capture that the user left waiting.
    platform::Win32Executor gamepad_capture_executor;
    /// Updates run off the UI thread: the release query and a 60 MB download both block.
    platform::Win32Executor update_executor;

    // Everything the Qt layer reports (qInfo/qWarning, Qt's own messages) goes into the same
    // log as the rest of the application. Without this the interface is a black box: a status
    // line could disagree with the log and there was no way to see which one was right.
    // Qt asks for a plain function pointer here, so the logger is reached through a
    // file-scope pointer that is set once, before any message can arrive.
    static platform::Logger* message_logger = nullptr;
    message_logger = &logger;
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& message) {
        if (message_logger == nullptr) {
            return;
        }
        const auto level = (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
            ? platform::LogLevel::warn
            : platform::LogLevel::info;
        static_cast<void>(message_logger->write(level, "qt", message.toStdString()));
        // Qt names the symptom when a widget is touched off the UI thread; the stack names
        // the culprit, because its own warning does not say who did it.
        if (message.contains(QStringLiteral("different thread"))) {
            platform::log_stack_trace("qt: cross-thread widget use");
        }
    });

    // The update controls of the About page. The .NET build checked the release feed
    // quietly on start and offered the installer there; the release asset naming for
    // THIS build is not settled yet (Phase 5 of the plan), so the install refuses to
    // run an installer that is not ours - silently installing the old .NET product over
    // a native build is exactly the kind of surprise this gate exists to prevent.
    services.application_version = [] {
        const auto text = domain::version();
        return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
    };
    services.update_check = [&logger, &update_executor, post_to_ui](
                                std::function<void(bool, QString, QString, QString)> report) {
        static_cast<void>(update_executor.post([&logger, post_to_ui, report = std::move(report)] {
            // The client is created here, on this worker: QNetworkAccessManager has
            // thread affinity and the UI thread must never block on a request.
            app::QtHttpClient http;
            const auto version = domain::version();
            core::support::UpdateService service(http, std::string(version));
            const auto result = service.check(domain::CancellationToken{});
            std::string detail;
            switch (result.kind) {
            case platform::UpdateCheckResultKind::update_available:
                detail = result.update.has_value() ? result.update->version : std::string();
                break;
            case platform::UpdateCheckResultKind::up_to_date:
                detail = "up to date";
                break;
            case platform::UpdateCheckResultKind::failed:
                detail = result.error;
                break;
            }
            static_cast<void>(logger.write(
                result.is_failed() ? platform::LogLevel::warn : platform::LogLevel::info,
                "update check", "current=" + std::string(version) + " result=" + detail));
            const QString version_text = result.update.has_value()
                ? QString::fromStdString(result.update->version)
                : QString();
            const QString notes = result.update.has_value() && result.update->release_notes.has_value()
                ? QString::fromStdString(*result.update->release_notes).left(600)
                : QString();
            const QString error = result.is_failed() ? QString::fromStdString(result.error) : QString();
            static_cast<void>(post_to_ui([report, available = result.is_available(), version_text,
                                                   notes, error] {
                report(available, version_text, notes, error);
            }));
        }));
    };
    services.update_install = [&logger, &update_executor, post_to_ui, &application, &presenter](
                                 std::function<void(int, QString, QString)> progress) {
        static_cast<void>(update_executor.post([&logger, post_to_ui, &application,
                                                   progress = std::move(progress)] {
            app::QtHttpClient http;
            const auto version = domain::version();
            core::support::UpdateService service(http, std::string(version));
            const auto result = service.check(domain::CancellationToken{});
            if (!result.is_available() || !result.update.has_value()) {
                const QString error = QString::fromStdString(
                    result.is_failed() ? result.error : std::string("no update available"));
                static_cast<void>(post_to_ui([progress, error] { progress(-1, QStringLiteral("download"), error); }));
                return;
            }
            const QString name = QString::fromStdString(
                result.update->installer_url.has_value() ? *result.update->installer_url : std::string());
            if (kNativeInstallerMarker.empty() || !name.contains(QString::fromLatin1(kNativeInstallerMarker))) {
                static_cast<void>(logger.write(platform::LogLevel::warn, "update install refused",
                    "the release carries no installer built for this application"));
                static_cast<void>(post_to_ui([progress] {
                    progress(-1, QStringLiteral("download"),
                        localized(ui::UiKey::k103));
                }));
                return;
            }
            const QString updates_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                + QStringLiteral("/updates");
            const std::filesystem::path installer = std::filesystem::path(updates_dir.toStdString())
                / ("VoiceTyper-" + result.update->version + "-Setup.exe");
            // The download reports every chunk; the UI needs whole percents only, and a
            // few thousand queued events would flood the Qt thread for nothing. The Linux
            // composition throttles the same way, and the flood is what killed the process
            // while the progress bar was updated from the worker (reproduced 2026-10-12).
            auto last_percent = std::make_shared<int>(-1);
            const auto status = service.download(*result.update, installer,
                [post_to_ui, progress, last_percent](std::uint64_t received, std::uint64_t total) {
                    const int percent = total > 0 ? static_cast<int>(received * 100U / total) : -1;
                    if (percent == *last_percent) {
                        return;
                    }
                    *last_percent = percent;
                    static_cast<void>(post_to_ui([progress, percent] {
                        progress(percent, QStringLiteral("download"), QString());
                    }));
                },
                domain::CancellationToken{});
            if (status.is_error()) {
                const QString error = QString::fromStdString(status.error().to_string());
                static_cast<void>(post_to_ui([progress, error] { progress(-1, QStringLiteral("download"), error); }));
                return;
            }
            const std::filesystem::path runner = std::filesystem::path(updates_dir.toStdString())
                / "run-update.cmd";
            const auto launched = platform::win32::launch_update(runner, installer,
                std::filesystem::path(QCoreApplication::applicationFilePath().toStdString()));
            if (launched.is_error()) {
                const QString error = QString::fromStdString(launched.error().to_string());
                static_cast<void>(post_to_ui([progress, error] { progress(-1, QStringLiteral("download"), error); }));
                return;
            }
            static_cast<void>(post_to_ui([progress] { progress(100, QStringLiteral("done"), QString()); }));
            // The launcher script waits for the installer and starts the app again; this
            // process must be gone by then.
            static_cast<void>(post_to_ui([&application] { QCoreApplication::quit(); }));
        }));
    };

    // The check on start. It used to be silent - the result went only to the log - so an
    // available update stayed invisible until the user thought of pressing «Проверить
    // обновления» (reported by Alexander, 16.10.2026). Now it is announced and the About
    // page is filled in, while the user is left to decide when to install.
    static_cast<void>(update_executor.post(
        [&logger, post_to_ui, startup_update, &announce_update, &notify_window_of_update] {
        app::QtHttpClient http;
        const auto version = domain::version();
        core::support::UpdateService service(http, std::string(version));
        const auto result = service.check(domain::CancellationToken{});
        // The compared version is part of the line: "why does it offer an update when
        // this build is newer" is otherwise unanswerable from the log.
        static_cast<void>(logger.write(result.is_failed() ? platform::LogLevel::warn : platform::LogLevel::info,
            "update check on start",
            "current=" + std::string(version) + " "
                + (result.is_available() && result.update.has_value()
                        ? "available=" + result.update->version
                        : result.is_failed() ? result.error : std::string("up to date"))));
        const bool available = result.is_available() && result.update.has_value();
        const QString version_text = available ? QString::fromStdString(result.update->version) : QString();
        const QString notes = available && result.update->release_notes.has_value()
            ? QString::fromStdString(*result.update->release_notes).left(600)
            : QString();
        const QString error = result.is_failed() ? QString::fromStdString(result.error) : QString();
        static_cast<void>(post_to_ui([available, version_text, notes, error, startup_update,
                                         &announce_update, &notify_window_of_update] {
            startup_update->answered = true;
            startup_update->available = available;
            startup_update->version = version_text;
            startup_update->notes = notes;
            startup_update->error = error;
            if (available && announce_update) {
                // At minimum a system notification: on a start minimized launch the window
                // is not on screen, and a user who never opens About would learn about the
                // update only by chance.
                announce_update(version_text);
            }
            if (notify_window_of_update) {
                notify_window_of_update();
            }
        }));
    }));

    // The microphone test runs its own capture: the dictation machine owns its session
    // and the native library allows one capture at a time, so the probe never touches
    // the machine. Capturing blocks, hence the capture worker and the UI-thread reply.
    // The live peak of the running probe, published for the settings window's meter.
    auto probe_level = std::make_shared<std::atomic<double>>(0.0);
    auto probe_cancel = std::make_shared<std::atomic<bool>>(false);
    services.microphone_probe_level = [probe_level] { return probe_level->load(); };
    services.microphone_probe_cancel = [probe_cancel] { probe_cancel->store(true); };
    services.microphone_probe = [&capture_options, &logger, &capture_executor, post_to_ui,
                                    probe_level, probe_cancel](
                                    std::function<void(ui::WindowServices::MicrophoneProbeOutcome, double, QString)> report) {
        probe_cancel->store(false);
        static_cast<void>(capture_executor.post([&capture_options, &logger, post_to_ui,
                                                    probe_level, probe_cancel,
                                                    report = std::move(report)] {
            platform::WindowsAudioCapture probe(capture_options);
            // The policy lives in app/microphone_probe.hpp: a successful start() is not a
            // promise of audio (the live build got buffers=0 on the first press), so the
            // probe waits for the first sample and restarts the session once if it stays
            // mute. On Linux the same rule runs over the libpulse backend.
            const auto outcome = run_microphone_probe(probe, *probe_cancel,
                [probe_level](double peak) { probe_level->store(peak); });
            const bool heard = outcome.verdict == MicrophoneProbeVerdict::heard;
            // The verdict and the numbers behind it go to the journal, so a silent meter can
            // be told from a session that never delivered anything.
            static_cast<void>(logger.write(heard ? platform::LogLevel::info : platform::LogLevel::warn,
                outcome.start_error.empty() ? "microphone test" : "microphone test failed",
                "peak=" + std::to_string(outcome.peak)
                    + (outcome.first_buffer_ms >= 0
                            ? " first_buffer_ms=" + std::to_string(outcome.first_buffer_ms)
                            : std::string(" first_buffer_ms=none"))
                    + " sessions=" + std::to_string(outcome.sessions)
                    + (outcome.start_error.empty() ? std::string()
                                                   : " error=" + outcome.start_error)
                    + " backend=" + outcome.backend));
            const QString detail = QString::fromStdString(
                outcome.start_error.empty() ? outcome.backend : outcome.start_error);
            const auto ui_outcome = outcome.verdict == MicrophoneProbeVerdict::heard
                ? ui::WindowServices::MicrophoneProbeOutcome::heard
                : outcome.verdict == MicrophoneProbeVerdict::silent
                    ? ui::WindowServices::MicrophoneProbeOutcome::silent
                    : ui::WindowServices::MicrophoneProbeOutcome::no_audio;
            static_cast<void>(post_to_ui([report, ui_outcome, peak = outcome.peak, detail] {
                report(ui_outcome, peak, detail);
            }));
        }));
    };
    services.capture_hotkey = [&](std::function<void(std::optional<std::string>, QString)> report) {
        auto hook = std::make_shared<platform::win32::HotkeyCaptureHook>();
        // A synthetic Ctrl+V during the capture would be eaten by the hook (the
        // .NET settings dialog suspends injection for exactly this reason).
        paste.set_suspended(true);
        static_cast<void>(capture_executor.post([&, hook, report = std::move(report)] {
            const auto run = [&](std::optional<std::string> gesture, QString error) {
                paste.set_suspended(false);
                static_cast<void>(post_to_ui(
                    [report = std::move(report), gesture = std::move(gesture), error = std::move(error)] {
                        report(gesture, error);
                    }));
            };
            const auto started = hook->start();
            if (started.is_error()) {
                static_cast<void>(logger.write(platform::LogLevel::warn, "hotkey capture unavailable",
                    started.error().to_string()));
                run(std::nullopt, QString::fromStdString(started.error().to_string()));
                return;
            }
            domain::CancellationSource source;
            const auto captured = hook->capture_next(source.token());
            static_cast<void>(hook->stop());
            if (captured.is_error()) {
                // Escape is a cancel, not a failure: the dialog must stay silent.
                const bool cancelled = captured.error().code() == domain::ErrorCode::cancelled;
                run(std::nullopt, cancelled ? QString() : QString::fromStdString(captured.error().to_string()));
                return;
            }
            const std::string text = captured.value().to_string();
            static_cast<void>(logger.write(platform::LogLevel::info, "hotkey captured", text));
            run(text, QString());
        }));
    };
    // The gamepad capture waits for a button press on its own worker, exactly like
    // the hotkey capture above. A machine whose XInput runtime could not be loaded
    // gets no service at all, so its buttons stay disabled with the localised
    // reason instead of failing on the first click.
    if (!platform::win32::xinput_runtime_name().empty()) {
        services.capture_gamepad = [&](std::function<void(std::optional<std::string>, QString)> report) {
            static_cast<void>(gamepad_capture_executor.post([&, report = std::move(report)] {
                domain::CancellationSource source;
                const auto captured = gamepad.capture_next(source.token());
                std::optional<std::string> binding;
                QString error;
                if (captured.is_error()) {
                    if (captured.error().code() == domain::ErrorCode::device_disconnected) {
                        // The raw reason names the technical cause; an unplugged
                        // controller is a user-facing state, so the window says so
                        // in the chosen language and the log keeps the detail.
                        static_cast<void>(logger.write(platform::LogLevel::warn, "gamepad capture",
                            captured.error().to_string()));
                        error = localized(ui::UiKey::k188);
                    } else if (captured.error().code() != domain::ErrorCode::cancelled) {
                        // A cancel is not a failure: the dialog must stay silent.
                        error = QString::fromStdString(captured.error().to_string());
                    }
                } else {
                    binding = captured.value().to_string();
                    static_cast<void>(logger.write(platform::LogLevel::info, "gamepad captured", *binding));
                }
                static_cast<void>(post_to_ui(
                    [report = std::move(report), binding = std::move(binding), error = std::move(error)] {
                        report(binding, error);
                    }));
            }));
        };
    }

    // Which models are on disk, and the ability to remove one. The .NET page offers
    // the same pair, and a downloaded model showing "Скачать" instead of "Удалить"
    // was reported from the running build.
    services.model_is_downloaded = [&](domain::TranscriptionEngine engine, int size_index) {
        if (size_index < 0 || size_index >= kModelRowsPerEngine) {
            return false;
        }
        const std::string name = model_file_name_for(engine, size_index);
        if (name.empty()) {
            return false;
        }
        std::error_code error;
        return std::filesystem::exists(
            models_directory(paths) / std::filesystem::path(name), error);
    };
    // Model downloads run like the update check: on a worker thread, with progress posted
    // back to the interface. Every row has its own cancellation source, so the user can stop a
    // transfer that is already running; the service then removes the partial file.
    auto transfer_sources =
        std::make_shared<std::vector<std::shared_ptr<domain::CancellationSource>>>(kModelRowSlots);
    services.model_download = [&logger, &update_executor, post_to_ui, &paths, transfer_sources](
                                 domain::TranscriptionEngine engine, int size_index,
                                 std::function<void(ui::WindowServices::ModelTransfer)> report) {
        if (size_index < 0 || size_index >= kModelRowsPerEngine) {
            return;
        }
        const std::string file_name = model_file_name_for(engine, size_index);
        if (file_name.empty()) {
            return;
        }
        const int slot = static_cast<int>(engine) * kModelRowsPerEngine + size_index;
        auto source = std::make_shared<domain::CancellationSource>();
        (*transfer_sources)[static_cast<std::size_t>(slot)] = source;
        static_cast<void>(update_executor.post([&logger, post_to_ui, &paths, engine, file_name,
                                                   source, report = std::move(report)] {
            app::QtHttpClient http;
            core::support::ModelDownloadService service(http, models_directory(paths));
            const auto download_engine = download_model_engine(engine);
            const std::string& name = file_name;
            const auto status = service.download(download_engine, name,
                [post_to_ui, report](const core::support::ModelDownloadProgress& progress) {
                    ui::WindowServices::ModelTransfer transfer;
                    transfer.percent = progress.total > 0
                        ? static_cast<int>(progress.fraction() * 100.0)
                        : -1;
                    transfer.bytes_per_second = progress.bytes_per_second;
                    transfer.remaining_seconds = progress.remaining_seconds().value_or(-1.0);
                    static_cast<void>(post_to_ui([report, transfer] { report(transfer); }));
                },
                source->token());
            if (status.is_error()) {
                const QString message = QString::fromStdString(status.error().to_string());
                const bool cancelled = status.error().code() == domain::ErrorCode::cancelled;
                static_cast<void>(logger.write(
                    cancelled ? platform::LogLevel::info : platform::LogLevel::warn,
                    cancelled ? "model download cancelled" : "model download failed",
                    name + ": " + status.error().to_string()));
                static_cast<void>(post_to_ui([report, message, cancelled] {
                    ui::WindowServices::ModelTransfer transfer;
                    transfer.error = message;
                    transfer.cancelled = cancelled;
                    report(transfer);
                }));
                return;
            }
            static_cast<void>(logger.write(platform::LogLevel::info, "model downloaded",
                std::string(engine_name(engine)) + " " + name));
            static_cast<void>(post_to_ui([report] {
                ui::WindowServices::ModelTransfer transfer;
                transfer.percent = 100;
                report(transfer);
            }));
        }));
    };
    services.model_download_cancel = [transfer_sources](domain::TranscriptionEngine engine, int size_index) {
        if (size_index < 0 || size_index >= kModelRowsPerEngine) {
            return;
        }
        const int slot = static_cast<int>(engine) * kModelRowsPerEngine + size_index;
        const auto& sources = *transfer_sources;
        if (static_cast<std::size_t>(slot) < sources.size() && sources[static_cast<std::size_t>(slot)]) {
            sources[static_cast<std::size_t>(slot)]->request_cancellation();
        }
    };
    services.model_delete = [&](domain::TranscriptionEngine engine, int size_index) {
        if (size_index < 0 || size_index >= kModelRowsPerEngine) {
            return false;
        }
        const std::string file_name = model_file_name_for(engine, size_index);
        if (file_name.empty()) {
            return false;
        }
        const std::filesystem::path file = models_directory(paths) / std::filesystem::path(file_name);
        const std::string name = file.filename().string();
        // Deleting a file is irreversible, so it is confirmed first.
        const auto answer = QMessageBox::question(nullptr, localized(ui::UiKey::k99),
            localized(ui::UiKey::k100).arg(QString::fromStdString(name)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return false;
        }
        std::error_code error;
        const bool removed = std::filesystem::remove(file, error);
        static_cast<void>(logger.write(removed ? platform::LogLevel::info : platform::LogLevel::error,
            "model deleted", name + (removed ? " (removed)" : " (could not be removed: " + error.message() + ")")));
        return removed;
    };

    // The optional local cleaner: three models in three repositories, picked by the user
    // (plan p_5d68d66503ca). The hooks are keyed by CleanupModel rather than a row index, because
    // `off` has no file and every size has its own address; a row offers "Скачать" until its file
    // is on disk.
    auto cleanup_sources = std::make_shared<std::vector<std::shared_ptr<domain::CancellationSource>>>(
        static_cast<std::size_t>(domain::CleanupModel::large) + 1);
    services.cleanup_model_is_downloaded = [&paths](domain::CleanupModel model) {
        const std::string name(core::support::cleanup_model_file_name(model));
        if (name.empty()) {
            return false;
        }
        std::error_code error;
        return std::filesystem::exists(models_directory(paths) / std::filesystem::path(name), error);
    };
    services.cleanup_model_download = [&logger, &update_executor, post_to_ui, &paths, cleanup_sources](
                                          domain::CleanupModel model,
                                          std::function<void(ui::WindowServices::ModelTransfer)> report) {
        const std::string name(core::support::cleanup_model_file_name(model));
        const std::string url(app::cleanup_model_url(model));
        if (name.empty() || url.empty()) {
            return;
        }
        auto source = std::make_shared<domain::CancellationSource>();
        (*cleanup_sources)[static_cast<std::size_t>(model)] = source;
        static_cast<void>(update_executor.post(
            [&logger, post_to_ui, &paths, name, url, source, report = std::move(report)] {
                app::QtHttpClient http;
                core::support::ModelDownloadService service(http, models_directory(paths));
                const auto status = service.download_url(url, name,
                    [post_to_ui, report](const core::support::ModelDownloadProgress& progress) {
                        ui::WindowServices::ModelTransfer transfer;
                        transfer.percent = progress.total > 0
                            ? static_cast<int>(progress.fraction() * 100.0)
                            : -1;
                        transfer.bytes_per_second = progress.bytes_per_second;
                        transfer.remaining_seconds = progress.remaining_seconds().value_or(-1.0);
                        static_cast<void>(post_to_ui([report, transfer] { report(transfer); }));
                    },
                    source->token());
                if (status.is_error()) {
                    const QString message = QString::fromStdString(status.error().to_string());
                    const bool cancelled = status.error().code() == domain::ErrorCode::cancelled;
                    static_cast<void>(logger.write(
                        cancelled ? platform::LogLevel::info : platform::LogLevel::warn,
                        cancelled ? "cleanup model download cancelled" : "cleanup model download failed",
                        name + ": " + status.error().to_string()));
                    static_cast<void>(post_to_ui([report, message, cancelled] {
                        ui::WindowServices::ModelTransfer transfer;
                        transfer.error = message;
                        transfer.cancelled = cancelled;
                        report(transfer);
                    }));
                    return;
                }
                static_cast<void>(logger.write(platform::LogLevel::info, "cleanup model downloaded", name));
                static_cast<void>(post_to_ui([report] {
                    ui::WindowServices::ModelTransfer transfer;
                    transfer.percent = 100;
                    report(transfer);
                }));
            }));
    };
    services.cleanup_model_download_cancel = [cleanup_sources](domain::CleanupModel model) {
        const auto slot = static_cast<std::size_t>(model);
        if (slot < cleanup_sources->size() && (*cleanup_sources)[slot]) {
            (*cleanup_sources)[slot]->request_cancellation();
        }
    };
    services.cleanup_model_delete = [&logger, &paths, &presenter,
                                        cleanup_runtime = cleanup_runtime.get()](
                                        domain::CleanupModel model) {
        const std::string name(core::support::cleanup_model_file_name(model));
        if (name.empty()) {
            return false;
        }
        const std::filesystem::path file = models_directory(paths) / std::filesystem::path(name);
        // Deleting a file is irreversible, so it is confirmed first, exactly like a recogniser.
        const auto answer = QMessageBox::question(nullptr, localized(ui::UiKey::k99),
            localized(ui::UiKey::k100).arg(QString::fromStdString(file.filename().string())),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return false;
        }
        // A loaded model keeps the GGUF mapped, and Windows refuses to delete a file that is in
        // use: the first live attempt logged "could not be removed: access denied" while the
        // interface stayed silent (Alexander, 14.10.2026). Let the weights go first, and drop the
        // choice when it pointed at this model, so no row claims to be active over a missing file.
        if (cleanup_runtime != nullptr) {
            cleanup_runtime->unload();
        }
        if (presenter.settings().cleanup_model == model) {
            presenter.update(app::SettingsChange::model, [](domain::AppSettings& settings) {
                settings.cleanup_model = domain::CleanupModel::off;
            });
        }
        std::error_code error;
        const bool removed = std::filesystem::remove(file, error);
        static_cast<void>(logger.write(removed ? platform::LogLevel::info : platform::LogLevel::error,
            "cleanup model deleted",
            name + (removed ? " (removed)" : " (could not be removed: " + error.message() + ")")));
        if (!removed) {
            QMessageBox::warning(nullptr, localized(ui::UiKey::k99),
                localized(ui::UiKey::k218)
                    .arg(QString::fromStdString(name),
                        QString::fromStdString(error.message())));
        }
        return removed;
    };

    // The microphone's own input level and its test: the two controls the .NET build
    // lacked while telling the user to "check the microphone level". The level is the
    // Windows endpoint value, so the slider and the Sound panel never disagree.
    auto microphone_level = std::make_shared<platform::MicrophoneLevelController>(
        platform::create_microphone_level());
    const platform::MicrophoneLevelState microphone_level_state = microphone_level->refresh();
    if (microphone_level_state.available) {
        services.microphone_level_get = [microphone_level] {
            return platform::microphone_level_percent(microphone_level->current().level);
        };
        services.microphone_level_set = [microphone_level, &logger](int percent) {
            const auto status = microphone_level->set_percent(percent);
            static_cast<void>(logger.write(
                status.is_ok() ? platform::LogLevel::info : platform::LogLevel::error,
                "microphone level set",
                "percent=" + std::to_string(percent)
                    + (status.is_ok() ? "" : " " + status.error().to_string())));
            return status.is_ok();
        };
        static_cast<void>(logger.write(platform::LogLevel::info, "microphone level",
            "level=" + std::to_string(platform::microphone_level_percent(microphone_level_state.level))
                + "% muted=" + (microphone_level_state.muted ? std::string("yes") : std::string("no"))));
    } else {
        static_cast<void>(logger.write(platform::LogLevel::warn, "microphone level",
            "the platform has no level control; the slider is disabled"));
    }

    // The button follows the machine, because a session can also end by itself (silence,
    // the hotkey, a failure) and then the UI kept showing "Остановить".
    services.recording_active = [&machine] {
        return machine.state() == domain::RecordingState::recording;
    };
    services.start_recording = [&machine] { static_cast<void>(machine.press_record()); };
    services.stop_recording = [&machine] { static_cast<void>(machine.release_record()); };
    // Settings changes reach the running services here, and only here. The engine
    // is re-selected rather than swapped in place, so a failed load leaves the UI
    // showing the reason instead of an engine that silently runs another model.
    services.settings_applied = [&](const domain::AppSettings& updated) {
        // The overlay follows the theme: white on white was unreadable.
        status_overlay.set_theme(updated.theme);
        status_overlay.set_language(updated.app_language);
        // Every later status string is built in the language the user just chose.
        ui::set_current_language(updated.app_language);
        if (apply_language_to_tray) {
            apply_language_to_tray(updated.app_language);
        }

        static_cast<void>(logger.write(
            platform::LogLevel::info,
            "settings applied",
            "engine=" + std::string(engine_name(updated.transcription_engine))));

        switch (updated.transcription_engine) {
        case domain::TranscriptionEngine::whisper:
            static_cast<void>(registry.set_model_path(
                domain::TranscriptionEngine::whisper,
                models_directory(paths) / core::support::whisper_model_file_name(updated.model_size)));
            break;
        case domain::TranscriptionEngine::parakeet:
            static_cast<void>(registry.set_model_path(
                domain::TranscriptionEngine::parakeet,
                models_directory(paths) / core::support::parakeet_model_file_name(updated.parakeet_model_size)));
            break;
        case domain::TranscriptionEngine::gigaam:
            static_cast<void>(registry.set_model_path(
                domain::TranscriptionEngine::gigaam,
                models_directory(paths) / core::support::gigaam_model_file_name(updated.gigaam_model_size)));
            break;
        }
        const auto selected_engine = updated.transcription_engine;
        const std::filesystem::path selected_model
            = registry.model_path(selected_engine).value_or(std::filesystem::path{});
        std::error_code model_probe;
        const bool model_present = !selected_model.empty() && std::filesystem::exists(selected_model, model_probe);
        // The switch itself is asynchronous, so the log has to say what was asked for
        // and whether the file it needs is there - otherwise "the engine does not
        // switch" is unanswerable from the log (Alexander checked exactly that).
        static_cast<void>(logger.write(model_present ? platform::LogLevel::info : platform::LogLevel::warn,
            "engine switch requested",
            std::string(engine_name(selected_engine))
                + " model=" + selected_model.string()
                + (model_present ? " (file present)" : " (MODEL FILE MISSING, the engine cannot load it)")));
        engine_host.select(selected_engine, selected_model);

        // Hotkeys are re-registered from the stored settings; a refused binding
        // reports its reason and the previous one is released, never kept twice.
        const auto report = hotkeys.apply_settings(updated);
        if (report.is_error()) {
            static_cast<void>(logger.write(
                platform::LogLevel::error, "hotkey re-registration failed", report.error().message()));
        } else {
            for (const auto& error : report.value().errors()) {
                static_cast<void>(logger.write(platform::LogLevel::warn, "hotkey", error));
            }
        }

        // The gamepad bindings live in the same settings change: an empty binding
        // stops the action, a malformed one is refused with a reason.
        const auto gamepad_report = gamepad.apply_settings(updated);
        if (gamepad_report.is_error()) {
            static_cast<void>(logger.write(platform::LogLevel::warn, "gamepad settings refused",
                gamepad_report.error().to_string()));
        }

        // Autostart is re-applied through the same hook the rest of the running
        // services use, which is where the .NET build called
        // StartupManager.SetRunAtStartup (SettingsViewModel.cs:1262, inside
        // Save). A changed startMinimized therefore rewrites the command line
        // of an entry that is already registered, rather than waiting for the
        // next launch to discover it.
        apply_autostart(updated, AutostartTrigger::user_change);
    };

    services.log_text = [&logger] {
        const auto lines = logger.tail(200);
        return lines.is_ok() ? QString::fromStdString(lines.value()) : localized(ui::UiKey::k96);
    };

    if (selftest) {
        // The Run key is reported, never touched: a diagnostic launch must not
        // add or remove an autostart entry on the user's machine, and the drift
        // between the setting and the registry is exactly what a support run
        // needs to see. Two entries can matter: this instance's own value (the
        // development name for a build tree) and the installed copy's canonical
        // one, which is what starts at logon - so both are reported.
        const auto autostart_entry = startup.read();
        const auto canonical_entry = canonical_startup.read();
        std::cout << "{\n";
        std::cout << "  \"settings\": \"" << settings_path(paths).string() << "\",\n";
        std::cout << "  \"engine_readiness\": \""
                  << asr::engine_readiness_name(engine_host.state().readiness) << "\",\n";
        std::cout << "  \"engine_engine\": \""
                  << (presenter.settings().transcription_engine == domain::TranscriptionEngine::whisper
                          ? "whisper"
                          : "parakeet")
                  << "\",\n";
        std::cout << "  \"devices\": " << devices.size() << ",\n";
        std::cout << "  \"hotkey_errors\": " << (registration.is_ok() ? registration.value().errors().size() : 1U)
                  << ",\n";
        std::cout << "  \"start_with_windows\": "
                  << (presenter.settings().start_with_windows ? "true" : "false") << ",\n";
        std::cout << "  \"start_minimized\": "
                  << ((start_minimized_switch || presenter.settings().start_minimized) ? "true" : "false")
                  << ",\n";
        std::cout << "  \"autostart_instance\": \""
                  << (installed_instance ? "installed" : "development") << "\",\n";
        std::cout << "  \"autostart_entry_name\": \""
                  << platform::win32::to_log_text(startup.value_name()) << "\",\n";
        std::cout << "  \"autostart_enabled\": "
                  << (autostart_entry.is_ok() && autostart_entry.value().present ? "true" : "false")
                  << ",\n";
        std::cout << "  \"autostart_state\": \""
                  << (autostart_entry.is_ok() ? autostart_state_text(startup)
                                              : autostart_entry.error().to_string())
                  << "\",\n";
        std::cout << "  \"installed_autostart_state\": \""
                  << (canonical_entry.is_ok() ? autostart_state_text(canonical_startup)
                                              : canonical_entry.error().to_string())
                  << "\"\n";
        std::cout << "}\n";
        std::cout << "voicetyper-selftest: OK\n";
        static_cast<void>(hotkeys.unregister_all());
        static_cast<void>(gamepad.stop());
        static_cast<void>(engine_host.shutdown(std::chrono::milliseconds(2000)));
        return 0;
    }

    // Reconcile the Run key on a real launch, before the window exists: the
    // entry must point at *this* executable, so it is rewritten whenever the
    // setting is on. Logged either way, so "autostart does not work" is a line
    // in the log rather than a mystery.
    apply_autostart(presenter.settings(), AutostartTrigger::launch);
    // «Запускать свёрнутым» is honoured on the stored setting *or* the switch
    // the autostart entry passes; the switch wins, because it is the explicit
    // request of this particular launch.
    const bool start_hidden = start_minimized_switch || presenter.settings().start_minimized;
    static_cast<void>(logger.write(
        platform::LogLevel::info,
        start_hidden ? "starting with the window hidden" : "starting with the window visible",
        std::string("source=") + (start_minimized_switch ? "--start-minimized" : "settings.start_minimized")));

    // A language change re-letters the window in place (ui::MainWindow::retranslate), so the
    // composition no longer rebuilds it and the window can own the services it was given.
    const auto app_icon = ui::TrayController::application_icon();
    auto window = std::make_shared<ui::MainWindow>(presenter, std::move(services), status_channel);
    window->setWindowIcon(app_icon);

    QSystemTrayIcon tray_icon;
    tray_icon.setIcon(app_icon);
    ui::TrayController tray(tray_icon);
    const auto wire_tray_window = [&tray, &window] {
        // One slot instead of show() + raise(): a click on the tray icon must put the
        // window in front of everything, not merely make it visible somewhere behind.
        QObject::connect(&tray, &ui::TrayController::show_requested, window.get(), &ui::MainWindow::bring_to_front);
        QObject::connect(&tray, &ui::TrayController::record_requested, window.get(),
            [weak = std::weak_ptr<ui::MainWindow>(window)] {
                if (const auto current = weak.lock()) {
                    if (auto* record = current->findChild<QPushButton*>(QStringLiteral("recordButton"))) {
                        record->click();
                    }
                }
            });
    };
    wire_tray_window();
    // The tray menu is built once, so it is told the language now and on every change.
    apply_language_to_tray = [&tray](domain::AppLanguage language) { tray.set_language(language); };
    // The tray is the only thing that can announce the update while the window is hidden
    // (a start minimized launch); the balloon is Qt's portable system notification.
    announce_update = [&tray_icon](const QString& version) {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) {
            return;
        }
        tray_icon.showMessage(localized(ui::UiKey::k224), localized(ui::UiKey::k67).arg(version),
            QSystemTrayIcon::Information, 10000);
    };
    // The unconfirmed paste is announced the same way: the transcript is safe, but the
    // user has to press the chord themselves, so the balloon names the chord.
    announce_clipboard_only = [&tray_icon] {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) {
            return;
        }
        tray_icon.showMessage(localized(ui::UiKey::k228),
            localized(ui::UiKey::k229).arg(QStringLiteral("Ctrl+V")), QSystemTrayIcon::Warning, 8000);
    };
    // A click on the balloon opens the settings window on «О программе», where «Скачать и
    // установить» is one press away (Alexander, 16.10.2026). The Windows tray backend emits
    // this for NIN_BALLOONUSERCLICK (qwindowssystemtrayicon.cpp), so it is the whole
    // mechanism here; the Linux composition has to send its own D-Bus notification.
    QObject::connect(&tray_icon, &QSystemTrayIcon::messageClicked, &application,
        [&logger, weak = std::weak_ptr<ui::MainWindow>(window)] {
            // Logged because "the notification opens nothing" is otherwise indistinguishable
            // from "the click never arrived": the Windows shell decides whether a balloon
            // click reaches NIN_BALLOONUSERCLICK at all.
            static_cast<void>(logger.write(platform::LogLevel::info, "update notification",
                "clicked; opening the About page"));
            if (auto current = weak.lock()) {
                current->open_about_page();
            }
        });
    // The check of the launch may answer before or after this point; either way the About
    // page is refreshed through the hook (and reads the report itself when it is opened).
    notify_window_of_update = [weak = std::weak_ptr<ui::MainWindow>(window)] {
        if (auto current = weak.lock()) {
            current->refresh_update_state();
        }
    };

    // A second launch asks this instance to come to the front. The window exists from here on;
    // a request that arrived while it was still being built is honoured right away.
    ui::set_activation_hook([&logger, weak = std::weak_ptr<ui::MainWindow>(window)] {
        if (auto raised = weak.lock()) {
            // Logged because a second launch is otherwise invisible: the window simply appears,
            // and during a check it must be possible to tell that from a fresh start.
            static_cast<void>(logger.write(platform::LogLevel::info, "single instance",
                "another launch asked to show; raising the window"));
            raised->bring_to_front();
        }
    });
    if (ui::take_pending_activation()) {
        window->bring_to_front();
    }
    tray.set_language(presenter.settings().app_language);
    QObject::connect(&tray, &ui::TrayController::quit_requested, &application, &QCoreApplication::quit);
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_icon.show();
    }
    // The tray tooltip is the only place this application states its state outside the
    // window, and a hover has to answer with what is true at that moment: it used to be
    // empty (reported by Alexander, 16.10.2026). A poll rather than two callbacks - the
    // engine's readiness changes without a signal of its own, and the recording handler
    // above only fires on a transition.
    QTimer tray_status_timer;
    auto tray_recording = std::make_shared<bool>(false);
    const auto refresh_tray_status = [&tray, &machine, &engine_host, tray_recording] {
        const auto state = machine.state();
        const bool recording_now = state != domain::RecordingState::idle;
        if (*tray_recording != recording_now) {
            *tray_recording = recording_now;
            tray.set_recording(recording_now);
        }
        if (state == domain::RecordingState::recording) {
            tray.set_status(ui::TrayStatus::recording);
            return;
        }
        if (state == domain::RecordingState::processing) {
            tray.set_status(ui::TrayStatus::processing);
            return;
        }
        tray.set_status(engine_host.state().readiness == asr::EngineReadiness::ready
                ? ui::TrayStatus::ready
                : ui::TrayStatus::engine_not_ready);
    };
    QObject::connect(&tray_status_timer, &QTimer::timeout, &tray, refresh_tray_status);
    tray_status_timer.setInterval(500);
    tray_status_timer.start();
    refresh_tray_status();
    // «Start minimized creates/assigns the window without showing it»
    // (compatibility-contracts.md §2). The window object still exists - it owns
    // the clipboard buffer and the tray - and the tray icon is still up, so the
    // app is reachable; only the window stays hidden, exactly like the .NET
    // build, which posted an already created ui::MainWindow without calling Show.
    if (!start_hidden) {
        window->show();
    }

    const auto exit_code = application.exec();

    // Shutdown, in the order the .NET app uses: hotkeys first so no new session
    // can start, then the gamepad poll, then the machine (which cancels and stops
    // capture), then the engine, which is only released once nothing is in flight.
    static_cast<void>(logger.write(platform::LogLevel::info, "shutting down"));
    static_cast<void>(hotkeys.unregister_all());
    static_cast<void>(gamepad.stop());
    machine.cancel();
    static_cast<void>(engine_host.shutdown(std::chrono::milliseconds(5000)));
    // Last: the overlay is the only thing still on screen, and destroying it
    // marks it terminal so a worker event queued behind the shutdown cannot
    // create a window while the process is exiting.
    static_cast<void>(status_overlay.destroy());
    return exit_code;
}

} // namespace voicetyper::app
