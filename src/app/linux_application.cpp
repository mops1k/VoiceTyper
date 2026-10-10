// Linux composition root: the single place where the real backends meet the
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

#include "app/application_font.hpp"
#include "app/qt_http_client.hpp"
#include "app/ui_text.hpp"
#include "core/support/appimage_update.hpp"
#include "core/support/model_download_service.hpp"
#include "core/support/update_service.hpp"
#include "platform/api/capture_guard.hpp"
#include "platform/linux/linux_audio_capture.hpp"
#include "platform/linux/linux_clipboard.hpp"
#include "platform/linux/linux_executor.hpp"
#include "platform/linux/linux_hotkeys.hpp"
#if defined(VOICETYPER_HAS_KGLOBALACCEL)
#include "platform/linux/linux_kglobalaccel.hpp"
#endif
#include "platform/linux/linux_gamepad.hpp"
#include "platform/linux/linux_microphone.hpp"
#include "platform/linux/linux_microphone_level.hpp"
#include "platform/linux/linux_paste.hpp"
#include "platform/linux/linux_paths.hpp"
#include "platform/linux/linux_startup.hpp"
#include "platform/api/microphone_level.hpp"
#include "app/main_window.hpp"
#include "app/status_overlay.hpp"
#include "app/tray_controller.hpp"
#include "app/settings_presenter.hpp"
#include "asr/engine_host.hpp"
#include "asr/gigaam_transcriber.hpp"
#include "asr/native_engine_registry.hpp"
#include "asr/native_transcribers.hpp"
#include "asr/silero_segmenter.hpp"
#include "domain/app_paths.hpp"
#include "domain/file_logger.hpp"
#include "domain/recording_state_machine.hpp"
#include "domain/silence_trimming_port.hpp"
#include "domain/terms_dictionary.hpp"
#include "domain/terms_dictionary_port.hpp"
#include "domain/version.hpp"
#include "domain/text_output.hpp"
#include "platform/portable/portable_runtime.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QProcess>
#include <QProcessEnvironment>
#include <QUrl>
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
QString localized(UiKey key)
{
    return ui_text(key, current_language());
}

/// The marker an installer must carry to be allowed to update THIS build: the native
/// package is published as VoiceTyper-<version>-win64-Setup.exe (the frozen asset regex
/// `^VoiceTyper-\d[^/]*?-Setup\.exe$` still matches it, and the older .NET asset
/// `VoiceTyper-1.1.3-Setup.exe` does not), so the installer that replaced the .NET
/// build can never be installed over the native one by mistake.
/// Kept for the log wording only: Linux never runs a downloaded installer, so
/// the marker is not a gate here (the Windows build still enforces it).
constexpr std::string_view kNativeInstallerMarker = "linux";

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
    const auto module = platform::linuxos::executable_file_path();
    if (!module.empty()) {
        return module.parent_path();
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

    domain::Result<domain::SampleBuffer> stop() override
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

    domain::Result<domain::SampleBuffer> drain() override { return inner_.drain(); }

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
    const bool start_minimized_switch = platform::linuxos::wants_start_minimized(argc, argv);
    QApplication application(argc, argv);
    // The bundled typeface, before any widget is built.
    voicetyper::app::install_application_font();
    // The product icon, embedded as a Qt resource: the window, the taskbar and the
    // tray all show the same image.
    if (const QIcon product_icon(QStringLiteral(":/assets/voiceTyper.png")); !product_icon.isNull()) {
        application.setWindowIcon(product_icon);
    }
    if (!selftest && !claim_single_instance()) {
        // A live instance already owns the microphone and the global hotkeys; starting a
        // second recorder would fight it. Ask the first one to show itself and leave. The
        // owner logs the request, so nothing is logged here (the log is not open yet either).
        static_cast<void>(notify_running_instance());
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
        domain::linux_app_path_roots(environment_variable, executable_directory()));

    platform::PortableClock clock;
    platform::PortableFileSystem file_system;
    // A real log file from the first line of the run: a silent log is how a
    // failed dictation looks like a working app.
    domain::FileLogger logger(log_directory(paths));

    // On a first run the XDG layout does not exist yet, and the probe below
    // writes into the log directory: the four directories are created once here,
    // so "settings cannot be saved" can never be caused by a missing directory.
    for (const auto& directory : {settings_path(paths).parent_path(), models_directory(paths),
             log_directory(paths), paths.updates_directory()}) {
        std::error_code directory_error;
        std::filesystem::create_directories(directory, directory_error);
        if (directory_error) {
            static_cast<void>(logger.write(platform::LogLevel::warn,
                "cannot create application directory",
                directory.string() + ": " + directory_error.message()));
        }
    }

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
    platform::linuxos::LinuxStartup startup;

    const auto autostart_state_text = [&startup] {
        const auto current = startup.read();
        if (current.is_error()) {
            return "entry=unreadable (" + current.error().to_string() + ")";
        }
        if (!current.value().present) {
            // std::string, not const char*: the other two branches return
            // std::string, and a lambda has one deduced return type.
            return std::string("entry=absent");
        }
        return "entry=registered command=\"" + current.value().command_line + "\"";
    };

    // `reconcile_on_launch` separates "the user asked for this" from "the app is
    // starting". Only the user's explicit change may rewrite an entry that
    // belongs to another installation: measured 2026-10-01, a single launch of
    // this C++ build repointed the installed .NET app's Run value at the build
    // tree, so Windows would have started the development build at the next
    // logon. While the .NET build is still the reference release that is data
    // corruption, and it is what this flag exists to prevent.
    const auto apply_autostart = [&startup, &logger, &autostart_state_text](
                                    const domain::AppSettings& applied, bool reconcile_on_launch) {
        const std::string requested = std::string("start_with_windows=")
            + (applied.start_with_windows ? "true" : "false") + " start_minimized="
            + (applied.start_minimized ? "true" : "false");

        if (reconcile_on_launch && applied.start_with_windows) {
            const auto current = startup.read();
            const auto own_path = platform::linuxos::executable_file_path();
            if (current.is_ok() && current.value().present && !own_path.empty()) {
                const std::string target =
                    platform::linuxos::command_line_executable(current.value().command_line);
                std::error_code exists_error;
                const bool target_exists = !target.empty()
                    && std::filesystem::exists(std::filesystem::path(target), exists_error)
                    && !exists_error;
                if (!platform::linuxos::launch_may_rewrite(
                        current.value().command_line, own_path.string(), target_exists)) {
                    static_cast<void>(logger.write(
                        platform::LogLevel::warn,
                        "autostart entry belongs to another installation and was left alone",
                        "entry=\"" + current.value().command_line
                            + "\" this=\"" + own_path.string()
                            + "\" (" + localized(UiKey::k14).toStdString()
                            + " to re-register it for this build)"));
                    return;
                }
            }
        }

        const auto status = startup.set_enabled(applied.start_with_windows, applied.start_minimized);
        if (status.is_error()) {
            static_cast<void>(logger.write(
                platform::LogLevel::error,
                "autostart entry was not applied",
                requested + " -> " + status.error().to_string()));
            return;
        }
        static_cast<void>(logger.write(
            platform::LogLevel::info,
            "autostart entry applied",
            requested + " -> " + autostart_state_text()));
    };

    // --- text output ---------------------------------------------------------
    platform::linuxos::LinuxClipboard clipboard;
    platform::linuxos::LinuxPasteSimulator paste;
    platform::LinuxExecutor ui_executor;
    // Work that touches widgets must run on the Qt thread: a widget read or written
    // from a worker is a data race and, in Qt's own words, a hard error. This
    // executor is a separate thread, so UI results are handed over through
    // QMetaObject::invokeMethod with a queued connection - the project's established
    // way (see StatusChannel::post) - and QApplication lives on that thread.
    const auto post_to_ui = [&application](std::function<void()> task) {
        QMetaObject::invokeMethod(
            &application, [task = std::move(task)] { task(); }, Qt::QueuedConnection);
    };
    domain::RetryingClipboard retrying(clipboard, clock);
    domain::TextOutputService output(retrying, paste, clock, ui_executor);
    // Why the text did or did not reach the document. "Pasted" and "only on the
    // clipboard because the focused window refused it" look identical in the UI,
    // and the target machine reported "it did not paste into the input" without
    // this line there was nothing to check it against.
    output.set_report_sink([&logger](const domain::OutputReport& report) {
        const std::string_view outcome = platform::text_output_outcome_name(report.outcome);
        const std::string detail = report.paste.has_value()
            ? (std::string(report.paste->reason) + " (injection_supported="
                  + (report.paste->injection_supported ? "true" : "false") + ")")
            : std::string();
        platform::LogLevel level = platform::LogLevel::info;
        if (report.outcome == platform::TextOutputOutcome::clipboard_failed) {
            level = platform::LogLevel::error;
        } else if (report.outcome == platform::TextOutputOutcome::clipboard_only) {
            level = platform::LogLevel::warn;
        }
        static_cast<void>(logger.write(level, "text output " + std::string(outcome), detail));
    });

    // --- capture -------------------------------------------------------------
    platform::linuxos::LinuxMicrophoneService microphone;
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

    platform::linuxos::LinuxAudioCapture::Options capture_options;
    if (selected.has_value()) {
        capture_options.device_id = *selected;
    }
    platform::linuxos::LinuxAudioCapture capture(capture_options);

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
    QtStatusOverlay status_overlay;
    // The language the shell speaks before the first change arrives.
    set_current_language(presenter.settings().app_language);
    /// Set once the tray exists: the menu is created later in this function, so the
    /// language change reaches it through this hook (the same pattern used for the
    /// window before the language was applied in place).
    std::function<void(domain::AppLanguage)> apply_language_to_tray;
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
    domain::EnergySpeechSegmenter energy_segmenter;
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
    domain::SpeechSegmenter& segmenter = silero_segmenter != nullptr
        ? static_cast<domain::SpeechSegmenter&>(*silero_segmenter)
        : static_cast<domain::SpeechSegmenter&>(energy_segmenter);
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
    domain::SilenceTrimmingPort silence_port(engine_host, segmenter);
    domain::TermsDictionaryPort dictionary_port(
        silence_port, [&presenter] { return presenter.settings().terms_dictionary; });
    domain::RecordingStateMachine machine(
        logged_capture, dictionary_port, output, worker, machine_options, &segmenter);
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
    auto status_channel = std::make_shared<StatusChannel>(nullptr);
    machine.set_state_changed_handler(
        [&logger, status_channel, &status_overlay, describe_state](domain::RecordingState state) {
            const std::string name = describe_state(state);
            static_cast<void>(logger.write(
                platform::LogLevel::info, std::string("recording state=") + name));
            // The state name is localized: the strip used to read "запись: idle", mixing a
            // Russian prefix with the enum's English name (Alexander, 08.10.2026).
            const QString localized_name = localized(state == domain::RecordingState::recording
                    ? UiKey::k180
                    : state == domain::RecordingState::processing ? UiKey::k7 : UiKey::k179);
            status_channel->post(localized(UiKey::k84) + QStringLiteral(": ") + localized_name);
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
        const QString device_message = localized(UiKey::k97);
        status_channel->post(device_problem
                ? device_message
                : localized(UiKey::k85) + QStringLiteral(": ")
                    + QString::fromStdString(std::string(error_code_name(error.code()))));
        // The overlay never shows an error: the status line above and the log carry
        // it. A pill that stayed on screen would sit on top of the window the user
        // is typing into, so the overlay is hidden instead (reported 2026-10-11).
        status_overlay.post_state(platform::OverlayState::idle);
    });
    machine.set_text_ready_handler([&logger, status_channel, &silence_port](std::string text) {
        // Length only. The log must never contain recognised text (LOG-01).
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
                + " compressed_pause=" + std::to_string(trimming.compressed_pause_samples)));
        static_cast<void>(logger.write(
            platform::LogLevel::info,
            "text delivered",
            "characters=" + std::to_string(text.size())));
        static_cast<void>(text.size());
        status_channel->post(localized(UiKey::k86));
    });

    machine.set_options_provider([&presenter] {
        const auto& current = presenter.settings();
        domain::SessionOptions options;
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
    // --- global hotkeys ------------------------------------------------------
    // evdev is the primary backend: it is the only one that always delivers the
    // release edge push-to-talk needs. A session without /dev/input access falls
    // back to org.kde.kglobalaccel, which delivers the release edge only when the
    // bus exposes globalShortcutReleased; an older kglobalaccel is press-only and
    // the mode degrades to toggle instead of pretending push-to-talk works
    // (VT-PLT-1307/1308).
    const auto hotkey_sink = [&machine, &logger](platform::HotkeyAction action) {
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
    };

    platform::linuxos::LinuxHotkeyService evdev_hotkeys;
    static_cast<void>(evdev_hotkeys.set_event_sink(hotkey_sink));
    auto registration = evdev_hotkeys.apply_settings(presenter.settings());
    platform::HotkeyService* hotkeys = &evdev_hotkeys;
    auto hotkey_capability = evdev_hotkeys.capability();

#if defined(VOICETYPER_HAS_KGLOBALACCEL)
    std::unique_ptr<platform::linuxos::LinuxKGlobalAccelHotkeys> kglobal_hotkeys;
    if (hotkey_capability == platform::linuxos::HotkeyCapability::none) {
        kglobal_hotkeys = std::make_unique<platform::linuxos::LinuxKGlobalAccelHotkeys>();
        static_cast<void>(kglobal_hotkeys->set_event_sink(hotkey_sink));
        registration = kglobal_hotkeys->apply_settings(presenter.settings());
        hotkey_capability = kglobal_hotkeys->capability();
        if (hotkey_capability != platform::linuxos::HotkeyCapability::none) {
            hotkeys = kglobal_hotkeys.get();
        }
    }
#endif

    // A press-only fallback cannot serve push-to-talk: the machine would start a
    // session and never see the release. The live settings provider below hands
    // it toggle mode instead, and the window says so (VT-PLT-1308).
    const bool hotkey_press_only =
        hotkey_capability == platform::linuxos::HotkeyCapability::kglobal_accel_press_only;
    static_cast<void>(logger.write(platform::LogLevel::info, "hotkey capability",
        std::string(platform::linuxos::hotkey_capability_name(hotkey_capability))));
    if (hotkey_press_only) {
        static_cast<void>(logger.write(platform::LogLevel::warn, "hotkeys",
            "push-to-talk is not available through kglobalaccel; using toggle"));
    }

    machine.set_live_settings_provider([&presenter, hotkey_press_only] {
        const auto& current = presenter.settings();
        return domain::RecordingStateMachine::LiveSettings{
            hotkey_press_only ? domain::RecordingMode::toggle : current.recording_mode,
            static_cast<double>(current.silence_threshold_ms) / 1000.0,
        };
    });
    static_cast<void>(logger.write(
        platform::LogLevel::info,
        "composition ready",
        "engine=" + std::string(asr::engine_readiness_name(engine_host.state().readiness))
            + " devices=" + std::to_string(devices.size())));

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
    // The same bindings the Windows build stores ("XInput|A"); on Linux the
    // XInput names are mapped onto evdev button codes (VT-PLT-1310). No
    // controller attached is not an error: the port stays idle and the settings
    // page shows empty readouts.
    platform::linuxos::LinuxGamepadService gamepad;
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
        static_cast<void>(logger.write(platform::LogLevel::info, "gamepad",
            gamepad.diagnostics().empty() ? std::string("no gamepad bindings") : gamepad.diagnostics()));
    }

    WindowServices services;
    // The footer dot is green only when the engine can really work: a missing model, a load
    // in progress or a failed start all keep it red (Alexander, 08.10.2026).
    services.engine_ready = [&engine_host] {
        return engine_host.state().readiness == asr::EngineReadiness::ready;
    };
    services.engine_status = [&engine_host] {
        const auto snapshot = engine_host.state();
        switch (snapshot.readiness) {
        case asr::EngineReadiness::ready:
            return localized(UiKey::k87);
        case asr::EngineReadiness::loading:
            return localized(UiKey::k88);
        case asr::EngineReadiness::warming:
            return localized(UiKey::k89);
        case asr::EngineReadiness::model_missing:
            return localized(UiKey::k90);
        case asr::EngineReadiness::unavailable:
            return localized(UiKey::k91) + QStringLiteral(": ")
                .arg(QString::fromStdString(std::string(
                    platform::engine_availability_reason_name(snapshot.reason))));
        case asr::EngineReadiness::failed:
            return localized(UiKey::k92) + QStringLiteral(": ")
                + QString::fromStdString(snapshot.last_error);
        default:
            return localized(UiKey::k93);
        }
    };
    services.record_hotkey_state = [&hotkeys, &presenter, hotkey_press_only] {
        const auto& current = presenter.settings();
        QString state = hotkeys->record_key_code() != 0
            ? localized(UiKey::k94) + QStringLiteral(": ")
                + QString::fromStdString(current.record_hotkey)
            : localized(UiKey::k95);
        if (hotkey_press_only) {
            // The fallback has no release edge, so the mode was forced to toggle;
            // say it instead of letting the hotkey look broken.
            state += QStringLiteral(" · ") + localized(UiKey::k182);
        }
        return state;
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
    platform::LinuxExecutor capture_executor;
    // A gamepad capture waits for a button press too, and it must not queue
    // behind a hotkey capture that the user left waiting.
    platform::LinuxExecutor gamepad_capture_executor;
    /// Updates run off the UI thread: the release query and a 60 MB download both block.
    platform::LinuxExecutor update_executor;

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
    services.update_check = [&logger, &update_executor, &post_to_ui](
                                std::function<void(bool, QString, QString, QString)> report) {
        static_cast<void>(update_executor.post([&logger, &post_to_ui, report = std::move(report)] {
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
    services.update_install = [&logger, &update_executor, &post_to_ui, &application](
                                 std::function<void(int, QString, QString)> progress) {
        static_cast<void>(update_executor.post([&logger, &post_to_ui, &application,
                                                   progress = std::move(progress)] {
            app::QtHttpClient http;
            const auto version = domain::version();

            // Linux updates by replacing the AppImage this process was started from:
            // the runtime exports APPIMAGE, and renaming a fresh file over the running
            // one is atomic - the process keeps the old image mapped, so nothing it
            // still needs disappears. A build that was not started from an AppImage
            // (source tree, distribution package) has no target, and keeps the old
            // behaviour: open the release page and say why (VT-SYS-014).
            const auto target = core::support::resolve_appimage_update_target(
                qEnvironmentVariable("APPIMAGE").toStdString());

            if (!target.has_value()) {
                core::support::UpdateService service(http, std::string(version));
                const auto result = service.check(domain::CancellationToken{});
                const QString release_url = result.update.has_value()
                        && result.update->installer_url.has_value()
                    ? QString::fromStdString(*result.update->installer_url)
                    : QStringLiteral("https://github.com/mops1k/VoiceTyper/releases/latest");
                static_cast<void>(logger.write(platform::LogLevel::info, "update install",
                    "not an AppImage run (APPIMAGE is empty); opening "
                        + release_url.toStdString()));
                static_cast<void>(post_to_ui([progress, release_url] {
                    static_cast<void>(QDesktopServices::openUrl(QUrl(release_url)));
                    progress(100, QStringLiteral("done"), QString());
                }));
                return;
            }

            core::support::UpdateService service(http, std::string(version),
                std::string(core::support::kUpdateLatestReleaseUrl),
                core::support::UpdateAssetKind::appimage);
            const auto result = service.check(domain::CancellationToken{});
            if (!result.is_available() || !result.update.has_value()) {
                const QString error = QString::fromStdString(
                    result.is_failed() ? result.error : std::string("no update available"));
                static_cast<void>(logger.write(platform::LogLevel::warn, "update install",
                    error.toStdString()));
                static_cast<void>(post_to_ui(
                    [progress, error] { progress(-1, QStringLiteral("download"), error); }));
                return;
            }

            const platform::UpdateInfo info = *result.update;
            // The download reports every chunk; the UI needs whole percents only, and
            // a few thousand queued events would flood the Qt thread for nothing.
            auto last_percent = std::make_shared<int>(-1);
            const auto downloaded = service.download(info, target->download,
                [&post_to_ui, progress, last_percent](
                    std::uint64_t received, std::uint64_t total) {
                    const int percent
                        = total > 0 ? static_cast<int>((received * 100) / total) : 0;
                    if (percent == *last_percent) {
                        return;
                    }
                    *last_percent = percent;
                    post_to_ui([progress, percent] {
                        progress(percent, QStringLiteral("download"), QString());
                    });
                },
                domain::CancellationToken{});
            if (downloaded.is_error()) {
                const QString error = QString::fromStdString(downloaded.error().to_string());
                static_cast<void>(logger.write(platform::LogLevel::error,
                    "update download failed", error.toStdString()));
                static_cast<void>(post_to_ui(
                    [progress, error] { progress(-1, QStringLiteral("download"), error); }));
                return;
            }

            const auto replaced
                = core::support::replace_appimage(target->download, target->image);
            if (replaced.is_error()) {
                const QString error = QString::fromStdString(replaced.error().to_string());
                static_cast<void>(logger.write(platform::LogLevel::error,
                    "update install failed", error.toStdString()));
                static_cast<void>(post_to_ui(
                    [progress, error] { progress(-1, QStringLiteral("install"), error); }));
                return;
            }

            const QString image_path = QString::fromStdString(target->image.string());
            static_cast<void>(logger.write(platform::LogLevel::info, "update install",
                "replaced " + target->image.string() + " with " + info.version + ", restarting"));
            static_cast<void>(post_to_ui([progress, image_path, &application] {
                progress(100, QStringLiteral("done"), QString());
                // The user has to end up in the new version: start the replaced image
                // and leave, since this process still runs the old one.
                //
                // The environment is cleaned first. This process was started by the
                // AppImage runtime, which exported APPDIR and the paths of the mount
                // that is going away; a restarted image inherits them, AppRun keeps a
                // stale APPDIR (it only sets one that is unset) and the new process
                // then cannot find its own Qt plugins - observed live as
                // "Could not find the Qt platform plugin \"wayland\"" followed by a
                // qFatal abort, which looked like a failed self-update.
                QProcess restart;
                QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
                for (const char* name : {"APPDIR", "APPIMAGE", "OWD", "ARGV0", "LD_LIBRARY_PATH",
                         "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"}) {
                    environment.remove(QLatin1String(name));
                }
                restart.setProcessEnvironment(environment);
                restart.setProgram(image_path);
                static_cast<void>(restart.startDetached());
                application.quit();
            }));
        }));
    };

    // The quiet check on start, exactly like the .NET build.
    static_cast<void>(update_executor.post([&logger] {
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
    }));

    // The microphone test runs its own capture: the dictation machine owns its session
    // and the native library allows one capture at a time, so the probe never touches
    // the machine. Capturing blocks, hence the capture worker and the UI-thread reply.
    // The live peak of the running probe, published for the settings window's meter.
    auto probe_level = std::make_shared<std::atomic<double>>(0.0);
    auto probe_cancel = std::make_shared<std::atomic<bool>>(false);
    services.microphone_probe_level = [probe_level] { return probe_level->load(); };
    services.microphone_probe_cancel = [probe_cancel] { probe_cancel->store(true); };
    services.microphone_probe = [&capture_options, &logger, &capture_executor, &ui_executor,
                                    probe_level, probe_cancel](
                                    std::function<void(bool, double, QString)> report) {
        probe_cancel->store(false);
        static_cast<void>(capture_executor.post([&capture_options, &logger, &ui_executor,
                                                    probe_level, probe_cancel,
                                                    report = std::move(report)] {
            platform::linuxos::LinuxAudioCapture probe(capture_options);
            const auto started = probe.start(domain::CancellationToken{});
            if (started.is_error()) {
                const std::string detail = started.error().to_string();
                static_cast<void>(logger.write(platform::LogLevel::warn, "microphone test failed", detail));
                static_cast<void>(ui_executor.post([report, detail] {
                    report(false, 0.0, QString::fromStdString(detail));
                }));
                return;
            }
            // The level is published every 50 ms, which is what lets the meter move with
            // the voice. The probe runs for up to half a minute instead of the old 2.5 s
            // sleep so the indicator stays alive while the user speaks, and the button
            // stops it early.
            // Eight seconds: long enough to watch the meter follow the voice and to press
            // "Остановить", short enough that the block never hangs without a verdict.
            const auto probe_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            int ticks = 0;
            while (!probe_cancel->load() && std::chrono::steady_clock::now() < probe_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                probe_level->store(probe.live_peak());
                // One line per half second: the numbers behind the meter, so a silent
                // indicator can be told from a silent microphone from the log alone.
                if (++ticks % 10 == 0) {
                    static_cast<void>(logger.write(platform::LogLevel::info, "microphone level",
                        "peak=" + std::to_string(probe_level->load())));
                }
            }
            probe_level->store(0.0);
            const auto stopped = probe.stop();
            double peak = 0.0;
            if (stopped.is_ok()) {
                for (const float sample : stopped.value().samples()) {
                    peak = std::max(peak, std::abs(static_cast<double>(sample)));
                }
            }
            const std::string backend = probe.backend_description();
            // 0.5 % of full scale is the floor the .NET build never checked: below it
            // the microphone is silent, muted or not connected.
            const bool heard = peak > 0.005;
            static_cast<void>(logger.write(heard ? platform::LogLevel::info : platform::LogLevel::warn,
                "microphone test", "peak=" + std::to_string(peak) + " backend=" + backend));
            const QString detail = QString::fromStdString(backend);
            static_cast<void>(ui_executor.post([report, heard, peak, detail] {
                report(heard, peak, detail);
            }));
        }));
    };
    services.capture_hotkey = [&](std::function<void(std::optional<std::string>, QString)> report) {
        auto hook = std::make_shared<platform::linuxos::HotkeyCaptureHook>();
        // A synthetic Ctrl+V during the capture would be eaten by the hook (the
        // .NET settings dialog suspends injection for exactly this reason).
        paste.set_suspended(true);
        static_cast<void>(capture_executor.post([&, hook, report = std::move(report)] {
            const auto run = [&](std::optional<std::string> gesture, QString error) {
                paste.set_suspended(false);
                static_cast<void>(ui_executor.post(
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

    services.capture_gamepad = [&](std::function<void(std::optional<std::string>, QString)> report) {
        static_cast<void>(gamepad_capture_executor.post([&, report = std::move(report)] {
            domain::CancellationSource source;
            const auto captured = gamepad.capture_next(source.token());
            std::optional<std::string> binding;
            QString error;
            if (captured.is_error()) {
                // A cancel is not a failure: the dialog must stay silent.
                if (captured.error().code() != domain::ErrorCode::cancelled) {
                    error = QString::fromStdString(captured.error().to_string());
                }
            } else {
                binding = captured.value().to_string();
                static_cast<void>(logger.write(platform::LogLevel::info, "gamepad captured", *binding));
            }
            static_cast<void>(ui_executor.post(
                [report = std::move(report), binding = std::move(binding), error = std::move(error)] {
                    report(binding, error);
                }));
        }));
    };

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
    services.model_download = [&logger, &update_executor, &ui_executor, &paths, transfer_sources](
                                 domain::TranscriptionEngine engine, int size_index,
                                 std::function<void(app::WindowServices::ModelTransfer)> report) {
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
        static_cast<void>(update_executor.post([&logger, &ui_executor, &paths, engine, file_name,
                                                   source, report = std::move(report)] {
            app::QtHttpClient http;
            core::support::ModelDownloadService service(http, models_directory(paths));
            const auto download_engine = download_model_engine(engine);
            const std::string& name = file_name;
            const auto status = service.download(download_engine, name,
                [&ui_executor, report](const core::support::ModelDownloadProgress& progress) {
                    app::WindowServices::ModelTransfer transfer;
                    transfer.percent = progress.total > 0
                        ? static_cast<int>(progress.fraction() * 100.0)
                        : -1;
                    transfer.bytes_per_second = progress.bytes_per_second;
                    transfer.remaining_seconds = progress.remaining_seconds().value_or(-1.0);
                    static_cast<void>(ui_executor.post([report, transfer] { report(transfer); }));
                },
                source->token());
            if (status.is_error()) {
                const QString message = QString::fromStdString(status.error().to_string());
                const bool cancelled = status.error().code() == domain::ErrorCode::cancelled;
                static_cast<void>(logger.write(
                    cancelled ? platform::LogLevel::info : platform::LogLevel::warn,
                    cancelled ? "model download cancelled" : "model download failed",
                    name + ": " + status.error().to_string()));
                static_cast<void>(ui_executor.post([report, message, cancelled] {
                    app::WindowServices::ModelTransfer transfer;
                    transfer.error = message;
                    transfer.cancelled = cancelled;
                    report(transfer);
                }));
                return;
            }
            static_cast<void>(logger.write(platform::LogLevel::info, "model downloaded",
                std::string(engine_name(engine)) + " " + name));
            static_cast<void>(ui_executor.post([report] {
                app::WindowServices::ModelTransfer transfer;
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
        const auto answer = QMessageBox::question(nullptr, localized(UiKey::k99),
            localized(UiKey::k100).arg(QString::fromStdString(name)),
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

    // The microphone's own input level and its test: the two controls the .NET build
    // lacked while telling the user to "check the microphone level". The level is the
    // Windows endpoint value, so the slider and the Sound panel never disagree.
    auto microphone_level = std::make_shared<platform::MicrophoneLevelController>(
        platform::linuxos::create_linux_microphone_level());
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
        set_current_language(updated.app_language);
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
        const auto report = hotkeys->apply_settings(updated);
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
        apply_autostart(updated, /*reconcile_on_launch=*/false);
    };

    services.log_text = [&logger] {
        const auto lines = logger.tail(200);
        return lines.is_ok() ? QString::fromStdString(lines.value()) : localized(UiKey::k96);
    };

    if (selftest) {
        // The Run key is reported, never touched: a diagnostic launch must not
        // add or remove an autostart entry on the user's machine, and the drift
        // between the setting and the registry is exactly what a support run
        // needs to see.
        const auto autostart_entry = startup.read();
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
        std::cout << "  \"autostart_enabled\": "
                  << (autostart_entry.is_ok() && autostart_entry.value().present ? "true" : "false")
                  << ",\n";
        std::cout << "  \"autostart_state\": \""
                  << (autostart_entry.is_ok() ? autostart_state_text() : autostart_entry.error().to_string())
                  << "\"\n";
        std::cout << "}\n";
        std::cout << "voicetyper-selftest: OK\n";
        static_cast<void>(hotkeys->unregister_all());
        static_cast<void>(engine_host.shutdown(std::chrono::milliseconds(2000)));
        return 0;
    }

    // Reconcile the Run key on a real launch, before the window exists: the
    // entry must point at *this* executable, so it is rewritten whenever the
    // setting is on. Logged either way, so "autostart does not work" is a line
    // in the log rather than a mystery.
    apply_autostart(presenter.settings(), /*reconcile_on_launch=*/true);
    // «Запускать свёрнутым» is honoured on the stored setting *or* the switch
    // the autostart entry passes; the switch wins, because it is the explicit
    // request of this particular launch.
    const bool start_hidden = start_minimized_switch || presenter.settings().start_minimized;
    static_cast<void>(logger.write(
        platform::LogLevel::info,
        start_hidden ? "starting with the window hidden" : "starting with the window visible",
        std::string("source=") + (start_minimized_switch ? "--start-minimized" : "settings.start_minimized")));

    // A language change re-letters the window in place (MainWindow::retranslate), so the
    // composition no longer rebuilds it and the window can own the services it was given.
    const auto app_icon = TrayController::application_icon();
    auto window = std::make_shared<MainWindow>(presenter, std::move(services), status_channel);
    window->setWindowIcon(app_icon);

    QSystemTrayIcon tray_icon;
    tray_icon.setIcon(app_icon);
    TrayController tray(tray_icon);
    const auto wire_tray_window = [&tray, &window] {
        // One slot instead of show() + raise(): a click on the tray icon must put the
        // window in front of everything, not merely make it visible somewhere behind.
        QObject::connect(&tray, &TrayController::show_requested, window.get(), &MainWindow::bring_to_front);
        QObject::connect(&tray, &TrayController::record_requested, window.get(),
            [weak = std::weak_ptr<MainWindow>(window)] {
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

    // A second launch asks this instance to come to the front. The window exists from here on;
    // a request that arrived while it was still being built is honoured right away.
    set_activation_hook([&logger, weak = std::weak_ptr<MainWindow>(window)] {
        if (auto raised = weak.lock()) {
            // Logged because a second launch is otherwise invisible: the window simply appears,
            // and during a check it must be possible to tell that from a fresh start.
            static_cast<void>(logger.write(platform::LogLevel::info, "single instance",
                "another launch asked to show; raising the window"));
            raised->bring_to_front();
        }
    });
    if (take_pending_activation()) {
        window->bring_to_front();
    }
    tray.set_language(presenter.settings().app_language);
    QObject::connect(&tray, &TrayController::quit_requested, &application, &QCoreApplication::quit);
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_icon.show();
    }
    // «Start minimized creates/assigns the window without showing it»
    // (compatibility-contracts.md §2). The window object still exists - it owns
    // the clipboard buffer and the tray - and the tray icon is still up, so the
    // app is reachable; only the window stays hidden, exactly like the .NET
    // build, which posted an already created MainWindow without calling Show.
    if (!start_hidden) {
        window->show();
    }

    const auto exit_code = application.exec();

    // Shutdown, in the order the .NET app uses: hotkeys first so no new session
    // can start, then the machine (which cancels and stops capture), then the
    // engine, which is only released once nothing is in flight.
    static_cast<void>(logger.write(platform::LogLevel::info, "shutting down"));
    static_cast<void>(hotkeys->unregister_all());
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
