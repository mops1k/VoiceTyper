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

#include "app/application_font.hpp"
#include "app/qt_http_client.hpp"
#include "app/ui_text.hpp"
#include "core/support/model_download_service.hpp"
#include "core/support/update_service.hpp"
#include "platform/api/capture_guard.hpp"
#include "platform/windows/win32_update_launcher.hpp"
#include "platform/api/microphone_level.hpp"
#include "app/main_window.hpp"
#include "app/status_overlay.hpp"
#include "app/tray_controller.hpp"
#include "app/settings_presenter.hpp"
#include "asr/engine_host.hpp"
#include "asr/native_engine_registry.hpp"
#include "asr/native_transcribers.hpp"
#include "domain/app_paths.hpp"
#include "domain/file_logger.hpp"
#include "domain/recording_state_machine.hpp"
#include "domain/version.hpp"
#include "domain/text_output.hpp"
#include "platform/windows/win32_clock.hpp"
#include "platform/windows/win32_clipboard.hpp"
#include "platform/windows/win32_executor.hpp"
#include "platform/windows/win32_file_system.hpp"
#include "platform/windows/win32_hotkeys.hpp"
#include "platform/windows/win32_paste.hpp"
#include "platform/windows/win32_startup.hpp"
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
constexpr int kModelRowSlots = kModelRowsPerEngine * 2;

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
constexpr std::string_view kNativeInstallerMarker = "win64";

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
    const bool start_minimized_switch = platform::win32::wants_start_minimized(argc, argv);
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
    platform::win32::Win32Startup startup;

    const auto autostart_state_text = [&startup] {
        const auto current = startup.read();
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
            const auto own_path = platform::win32::module_file_path();
            if (current.is_ok() && current.value().present && own_path.is_ok()) {
                const std::wstring target = platform::win32::command_line_executable(current.value().command_line);
                std::error_code exists_error;
                const bool target_exists = !target.empty()
                    && std::filesystem::exists(std::filesystem::path(target), exists_error)
                    && !exists_error;
                if (!platform::win32::launch_may_rewrite(
                        current.value().command_line, own_path.value(), target_exists)) {
                    static_cast<void>(logger.write(
                        platform::LogLevel::warn,
                        "autostart entry belongs to another installation and was left alone",
                        "registry=\"" + platform::win32::to_log_text(current.value().command_line)
                            + "\" this=\"" + platform::win32::to_log_text(own_path.value())
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
    platform::Win32Clipboard clipboard;
    platform::Win32PasteSimulator paste;
    platform::Win32Executor ui_executor;
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
            status_overlay.post_state(
                platform::OverlayState::error, detail.empty() ? state_name : detail);
        }
    });
    engine_status_timer.start();

    // --- recording state machine --------------------------------------------
    // VAD mode needs a segmenter or recording never auto-stops. This is the
    // energy heuristic, not Silero: the shipped silero model is not bound to a
    // native runtime yet, and a working auto-stop is better than none.
    domain::EnergySpeechSegmenter segmenter;
    domain::ThreadRecordingWorker worker;
    domain::RecordingStateMachineOptions machine_options;
    machine_options.mode = settings.recording_mode;
    machine_options.vad_silence_threshold_seconds =
        static_cast<double>(settings.silence_threshold_ms) / 1000.0;
    LoggedRecordingPort logged_capture(capture, logger,
        selected.has_value() ? *selected : std::string("(system default)"),
        [&capture] { return capture.backend_description(); }, &capture_guard);
    domain::RecordingStateMachine machine(
        logged_capture, engine_host, output, worker, machine_options, &segmenter);
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
            status_channel->post(localized(UiKey::k84) + QStringLiteral(": ")
                + QString::fromStdString(name));
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
        // The overlay keeps this on screen until the next dictation, so a
        // missing model or a lost device is visible instead of silent.
        status_overlay.post_state(platform::OverlayState::error,
            device_problem ? device_message.toStdString() : detail);
    });
    machine.set_text_ready_handler([&logger, status_channel](std::string text) {
        // Length only. The log must never contain recognised text (LOG-01).
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
        options.prompt = current.terms_dictionary;
        options.temperature = current.temperature;
        options.condition_on_previous_text = current.condition_on_previous_text;
        options.auto_paste = current.auto_paste_enabled;
        options.best_of = domain::kFinalBestOf;
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

    WindowServices services;
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
    services.record_hotkey_state = [&hotkeys, &presenter] {
        const auto& current = presenter.settings();
        return hotkeys.record_key_code() != 0
            ? localized(UiKey::k94) + QStringLiteral(": ")
                + QString::fromStdString(current.record_hotkey)
            : localized(UiKey::k95);
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
    services.update_check = [&logger, &update_executor, &ui_executor](
                                std::function<void(bool, QString, QString, QString)> report) {
        static_cast<void>(update_executor.post([&logger, &ui_executor, report = std::move(report)] {
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
            static_cast<void>(ui_executor.post([report, available = result.is_available(), version_text,
                                                   notes, error] {
                report(available, version_text, notes, error);
            }));
        }));
    };
    services.update_install = [&logger, &update_executor, &ui_executor, &application, &presenter](
                                 std::function<void(int, QString, QString)> progress) {
        static_cast<void>(update_executor.post([&logger, &ui_executor, &application,
                                                   progress = std::move(progress)] {
            app::QtHttpClient http;
            const auto version = domain::version();
            core::support::UpdateService service(http, std::string(version));
            const auto result = service.check(domain::CancellationToken{});
            if (!result.is_available() || !result.update.has_value()) {
                const QString error = QString::fromStdString(
                    result.is_failed() ? result.error : std::string("no update available"));
                static_cast<void>(ui_executor.post([progress, error] { progress(-1, QStringLiteral("download"), error); }));
                return;
            }
            const QString name = QString::fromStdString(
                result.update->installer_url.has_value() ? *result.update->installer_url : std::string());
            if (kNativeInstallerMarker.empty() || !name.contains(QString::fromLatin1(kNativeInstallerMarker))) {
                static_cast<void>(logger.write(platform::LogLevel::warn, "update install refused",
                    "the release carries no installer built for this application"));
                static_cast<void>(ui_executor.post([progress] {
                    progress(-1, QStringLiteral("download"),
                        localized(UiKey::k103));
                }));
                return;
            }
            const QString updates_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                + QStringLiteral("/updates");
            const std::filesystem::path installer = std::filesystem::path(updates_dir.toStdString())
                / ("VoiceTyper-" + result.update->version + "-Setup.exe");
            const auto status = service.download(*result.update, installer,
                [&ui_executor, progress](std::uint64_t received, std::uint64_t total) {
                    const int percent = total > 0 ? static_cast<int>(received * 100U / total) : -1;
                    static_cast<void>(ui_executor.post([progress, percent] {
                        progress(percent, QStringLiteral("download"), QString());
                    }));
                },
                domain::CancellationToken{});
            if (status.is_error()) {
                const QString error = QString::fromStdString(status.error().to_string());
                static_cast<void>(ui_executor.post([progress, error] { progress(-1, QStringLiteral("download"), error); }));
                return;
            }
            const std::filesystem::path runner = std::filesystem::path(updates_dir.toStdString())
                / "run-update.cmd";
            const auto launched = platform::win32::launch_update(runner, installer,
                std::filesystem::path(QCoreApplication::applicationFilePath().toStdString()));
            if (launched.is_error()) {
                const QString error = QString::fromStdString(launched.error().to_string());
                static_cast<void>(ui_executor.post([progress, error] { progress(-1, QStringLiteral("download"), error); }));
                return;
            }
            static_cast<void>(ui_executor.post([progress] { progress(100, QStringLiteral("done"), QString()); }));
            // The launcher script waits for the installer and starts the app again; this
            // process must be gone by then.
            static_cast<void>(ui_executor.post([&application] { QCoreApplication::quit(); }));
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
    services.microphone_probe = [&capture_options, &logger, &capture_executor, &ui_executor](
                                    std::function<void(bool, double, QString)> report) {
        static_cast<void>(capture_executor.post([&capture_options, &logger, &ui_executor,
                                                    report = std::move(report)] {
            platform::WindowsAudioCapture probe(capture_options);
            const auto started = probe.start(domain::CancellationToken{});
            if (started.is_error()) {
                const std::string detail = started.error().to_string();
                static_cast<void>(logger.write(platform::LogLevel::warn, "microphone test failed", detail));
                static_cast<void>(ui_executor.post([report, detail] {
                    report(false, 0.0, QString::fromStdString(detail));
                }));
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
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
        auto hook = std::make_shared<platform::win32::HotkeyCaptureHook>();
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

    // Which models are on disk, and the ability to remove one. The .NET page offers
    // the same pair, and a downloaded model showing "Скачать" instead of "Удалить"
    // was reported from the running build.
    services.model_is_downloaded = [&](bool whisper, int size_index) {
        std::error_code error;
        if (whisper) {
            const auto size = static_cast<domain::ModelSize>(size_index);
            return std::filesystem::exists(models_directory(paths) / std::filesystem::path(std::string(core::support::whisper_model_file_name(size))), error);
        }
        const auto size = static_cast<domain::ParakeetModelSize>(size_index);
        return std::filesystem::exists(models_directory(paths) / std::filesystem::path(std::string(core::support::parakeet_model_file_name(size))), error);
    };
    // Model downloads run like the update check: on a worker thread, with progress posted
    // back to the interface. Every row has its own cancellation source, so the user can stop a
    // transfer that is already running; the service then removes the partial file.
    auto transfer_sources =
        std::make_shared<std::vector<std::shared_ptr<domain::CancellationSource>>>(kModelRowSlots);
    services.model_download = [&logger, &update_executor, &ui_executor, &paths, transfer_sources](
                                 bool whisper, int size_index,
                                 std::function<void(app::WindowServices::ModelTransfer)> report) {
        if (size_index < 0 || size_index >= kModelRowsPerEngine) {
            return;
        }
        const int slot = (whisper ? 0 : kModelRowsPerEngine) + size_index;
        auto source = std::make_shared<domain::CancellationSource>();
        (*transfer_sources)[static_cast<std::size_t>(slot)] = source;
        static_cast<void>(update_executor.post([&logger, &ui_executor, &paths, whisper, size_index,
                                                   source, report = std::move(report)] {
            app::QtHttpClient http;
            core::support::ModelDownloadService service(http, models_directory(paths));
            const auto engine = whisper ? core::support::ModelEngine::whisper
                                        : core::support::ModelEngine::parakeet;
            const std::string name = whisper
                ? std::string(core::support::whisper_model_file_name(
                      static_cast<domain::ModelSize>(size_index)))
                : std::string(core::support::parakeet_model_file_name(
                      static_cast<domain::ParakeetModelSize>(size_index)));
            const auto status = service.download(engine, name,
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
                std::string(whisper ? "whisper " : "parakeet ") + name));
            static_cast<void>(ui_executor.post([report] {
                app::WindowServices::ModelTransfer transfer;
                transfer.percent = 100;
                report(transfer);
            }));
        }));
    };
    services.model_download_cancel = [transfer_sources](bool whisper, int size_index) {
        if (size_index < 0 || size_index >= kModelRowsPerEngine) {
            return;
        }
        const int slot = (whisper ? 0 : kModelRowsPerEngine) + size_index;
        const auto& sources = *transfer_sources;
        if (static_cast<std::size_t>(slot) < sources.size() && sources[static_cast<std::size_t>(slot)]) {
            sources[static_cast<std::size_t>(slot)]->request_cancellation();
        }
    };
    services.model_delete = [&](bool whisper, int size_index) {
        const auto size = static_cast<domain::ModelSize>(size_index);
        const std::filesystem::path file = whisper
            ? models_directory(paths) / std::filesystem::path(std::string(core::support::whisper_model_file_name(size)))
            : models_directory(paths) / std::filesystem::path(std::string(core::support::parakeet_model_file_name(static_cast<domain::ParakeetModelSize>(size_index))));
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
            "engine=" + std::string(
                updated.transcription_engine == domain::TranscriptionEngine::whisper ? "whisper" : "parakeet")));

        if (updated.transcription_engine == domain::TranscriptionEngine::whisper) {
            static_cast<void>(registry.set_model_path(
                domain::TranscriptionEngine::whisper,
                models_directory(paths) / core::support::whisper_model_file_name(updated.model_size)));
        } else {
            static_cast<void>(registry.set_model_path(
                domain::TranscriptionEngine::parakeet,
                models_directory(paths) / core::support::parakeet_model_file_name(updated.parakeet_model_size)));
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
            std::string(selected_engine == domain::TranscriptionEngine::whisper ? "whisper" : "parakeet")
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
        static_cast<void>(hotkeys.unregister_all());
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
    static_cast<void>(hotkeys.unregister_all());
    machine.cancel();
    static_cast<void>(engine_host.shutdown(std::chrono::milliseconds(5000)));
    // Last: the overlay is the only thing still on screen, and destroying it
    // marks it terminal so a worker event queued behind the shutdown cannot
    // create a window while the process is exiting.
    static_cast<void>(status_overlay.destroy());
    return exit_code;
}

} // namespace voicetyper::app
