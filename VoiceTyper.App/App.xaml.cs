using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Windows;
using Application = System.Windows.Application;
using MessageBox = System.Windows.MessageBox;
using VoiceTyper.App.Services;
using VoiceTyper.App.Tray;
using VoiceTyper.App.Overlay;
using VoiceTyper.App.ViewModels;
using VoiceTyper.Core.Abstractions;
using VoiceTyper.Core.Audio;
using VoiceTyper.Core.Localization;
using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;

namespace VoiceTyper.App;

/// <summary>
/// Композиционный корень: создаёт сервисы, регистрирует хоткеи, иконку трея,
/// асинхронно подготавливает модель и связывает конечный автомат записи.
/// </summary>
public partial class App : Application
{
    private const string MutexName = "Global\\VoiceTyper_SingleInstance";

    private Mutex? _mutex;
    private TrayIcon? _tray;
    private HotkeyService? _hotkeys;
    private ISettingsService? _settingsService;
    private IModelManager? _modelManager;
    private IMicrophoneService? _microphoneService;
    private GithubUpdateService? _updateService;
    private SettingsViewModel? _settingsViewModel;
    private MainWindow? _mainWindow;
    private StatusOverlayWindow? _statusOverlay;
    private AppSettings _currentSettings = new();
    private readonly IAppLogger _logger;
    private bool _isQuitting;

    private IRecordingStateMachine? _stateMachine;
    private ITranscriptionService? _transcription;
    private ModelSize _loadedModelSize;
    private string? _loadedMicrophoneId;
    private string? _vadPath;
    private RecordingMode _lastRecordingMode;
    private int _lastSilenceThresholdMs;
    private AppTheme _lastAppliedTheme = AppTheme.System;
    private AppLanguage _lastAppliedLanguage = AppLanguage.Ru;
    private CancellationTokenSource? _downloadCts;
    private bool _engineInitializing;
    private readonly SemaphoreSlim _engineInitLock = new(1, 1);

    public App()
    {
        _logger = new FileLogger();
    }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // Каждый запуск начинаем с чистого лога.
        _logger.Clear();

        _settingsService = new SettingsService();
        _currentSettings = _settingsService.Load();

        // Язык применяем сразу после загрузки настроек, но до первого лога/UI,
        // чтобы логи и сообщения были на нужном языке. При первом запуске — по ОС.
        var effectiveLanguage = ResolveEffectiveLanguage();
        Loc.Instance.Apply(effectiveLanguage);
        _lastAppliedLanguage = effectiveLanguage;
        if (effectiveLanguage != _currentSettings.AppLanguage)
        {
            _currentSettings.AppLanguage = effectiveLanguage;
            _settingsService.Save(_currentSettings);
        }

        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
            _logger.Error(Loc.T("Log_UnhandledAppDomain"), args.ExceptionObject as Exception);
        TaskScheduler.UnobservedTaskException += (_, args) =>
        {
            _logger.Error(Loc.T("Log_UnhandledTask"), args.Exception);
            args.SetObserved();
        };

        DispatcherUnhandledException += (_, args) =>
        {
            _logger.Error(Loc.T("Log_UnhandledUi"), args.Exception);
            MessageBox.Show(Loc.Format("Log_UnhandledErrorMsg", args.Exception.Message), Loc.T("App_MessageBoxTitle"),
                MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        };

        _logger.Info(Loc.Format("Log_Startup", Environment.ProcessId));
        _logger.Info(Loc.Format("Log_System", Environment.Version, Environment.OSVersion, Environment.ProcessorCount));
        _logger.Info(Loc.Format("Log_LogDirectory", _logger.LogDirectory));
        _mutex = new Mutex(initiallyOwned: true, MutexName, out var createdNew);
        if (!createdNew)
        {
            _logger.Warn(Loc.T("Log_SecondInstance"));
            MessageBox.Show(Loc.T("App_AlreadyRunning"), Loc.T("App_MessageBoxTitle"), MessageBoxButton.OK,
                MessageBoxImage.Information);
            Shutdown();
            return;
        }

        _modelManager = new ModelManager();
        _modelManager.CleanupLegacyModels();
        _microphoneService = new MicrophoneService();
        _updateService = new GithubUpdateService();
        ThemeManager.Apply(_currentSettings.Theme);
        _lastAppliedTheme = _currentSettings.Theme;
        _logger.Info(Loc.Format("Log_SettingsPath", _settingsService.SettingsFilePath));
        _logger.Info(Loc.Format("Log_SettingsSummary", _currentSettings.RecordingMode, _currentSettings.Language,
            _currentSettings.ModelSize, _currentSettings.AutoPasteEnabled,
            _currentSettings.MicrophoneDeviceId ?? Loc.T("Log_DefaultMic")));
        _logger.Info(Loc.Format("Log_Hotkeys", _currentSettings.RecordHotkey, _currentSettings.CancelHotkey));

        _tray = new TrayIcon();
        _tray.ApplyTheme(ThemeManager.IsSystemDark);
        _tray.OpenSettingsRequested += ShowSettingsWindow;
        _tray.RecordRequested += OnTrayRecord;
        _tray.QuitRequested += Quit;
        _hotkeys = new HotkeyService();
        _hotkeys.RecordPressed += OnRecordPressed;
        _hotkeys.CancelPressed += OnCancelPressed;
        foreach (var error in _hotkeys.ApplySettings(_currentSettings))
        {
            _logger.Warn(Loc.Format("Log_HotkeyRegistration", error));
            _tray.ShowBalloon(Loc.T("App_MessageBoxTitle"), error);
        }

        var microphones = _microphoneService.GetMicrophones();
        _logger.Info(microphones.Count == 0
            ? Loc.T("Log_MicNone")
            : Loc.Format("Log_MicList", string.Join(" | ", microphones.Select(m => m.Name))));

        _settingsViewModel =
            new SettingsViewModel(_settingsService, _hotkeys, _microphoneService, _modelManager, _updateService);
        _settingsViewModel.SettingsApplied += OnSettingsApplied;
        _settingsViewModel.DownloadCancelRequested += CancelModelDownload;
        _settingsViewModel.UpdateAvailable += v =>
            _tray?.ShowBalloon(Loc.T("App_MessageBoxTitle"), Loc.Format("Update_AvailableBalloon", v));
        _settingsViewModel.UpdateInstallStarted += OnUpdateInstallStarted;
        _settingsViewModel.SetStatus(Loc.T("Status_Ready"));
        ThemeManager.ThemeApplied += () => _tray?.ApplyTheme(ThemeManager.IsSystemDark);

        _ = _settingsViewModel.CheckForUpdatesAsync(auto: true);
        _ = InitializeEngineAsync();
        _ = TestMicrophoneAsync();

        if (!_currentSettings.StartMinimized)
        {
            ShowSettingsWindow();
        }
    }

    /// <summary>
    /// Определяет эффективный язык интерфейса: при первом запуске (нет файла настроек) —
    /// по языку операционной системы, иначе — из сохранённых настроек.
    /// </summary>
    private AppLanguage ResolveEffectiveLanguage()
    {
        if (!File.Exists(_settingsService!.SettingsFilePath))
        {
            var ui = CultureInfo.InstalledUICulture ?? CultureInfo.CurrentUICulture;
            return ui.Name.StartsWith("en", StringComparison.OrdinalIgnoreCase) ? AppLanguage.En : AppLanguage.Ru;
        }

        return _currentSettings.AppLanguage;
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _logger.Info(Loc.T("Log_Quit"));
        _hotkeys?.UnregisterAll();
        _tray?.Dispose();
        _statusOverlay?.Close();
        _ = _stateMachine?.DisposeAsync() ?? ValueTask.CompletedTask;
        _ = _transcription?.DisposeAsync() ?? ValueTask.CompletedTask;
        _mutex?.Dispose();
        base.OnExit(e);
    }

    private async Task InitializeEngineAsync()
    {
        await _engineInitLock.WaitAsync();
        try
        {
            _engineInitializing = true;
            var oldMachine = _stateMachine;

            try
            {
                // Модель (и VAD) грузим только при смене размера модели или первом запуске.
                // При смене РЕЖИМА/ПОРОГА пересоздаём только конечный автомат — модель остаётся в памяти.
                if (_transcription is null || _loadedModelSize != _currentSettings.ModelSize)
                {
                    _logger.Info(Loc.T("Log_EngineInitModel"));
                    _downloadCts = new CancellationTokenSource();
                    try
                    {
                        var modelPath = await _modelManager!.EnsureModelAsync(_currentSettings.ModelSize,
                            ModelDownloadProgress(Loc.T("Models_WhisperLabel")), _downloadCts.Token);
                        _vadPath = await _modelManager.EnsureVadModelAsync(
                            ModelDownloadProgress(Loc.T("Models_VadLabel")), _downloadCts.Token);
                        _loadedModelSize = _currentSettings.ModelSize;

                        var oldTranscription = _transcription;
                        _transcription = new WhisperTranscriptionService(modelPath);
                        _transcription.Warmup();
                        await (oldTranscription?.DisposeAsync() ?? ValueTask.CompletedTask);

                        _logger.Info(Loc.Format("Log_ModelWhisper", modelPath));
                        _logger.Info(Loc.Format("Log_ModelVad", _vadPath));
                    }
                    catch (OperationCanceledException)
                    {
                        _logger.Warn(Loc.T("Log_DownloadCancelled"));
                        _settingsViewModel?.SetStatus(Loc.T("Status_DownloadCancelled"));
                        _settingsViewModel?.ClearModelDownload();
                        return;
                    }
                    finally
                    {
                        _downloadCts?.Dispose();
                        _downloadCts = null;
                    }
                }

                _loadedMicrophoneId = _currentSettings.MicrophoneDeviceId;
                _lastRecordingMode = _currentSettings.RecordingMode;
                _lastSilenceThresholdMs = _currentSettings.SilenceThresholdMs;
                _logger.Info(
                    Loc.Format("Log_MicDevice", _currentSettings.MicrophoneDeviceId ?? Loc.T("Log_DefaultMic")));

                if (oldMachine is not null)
                {
                    oldMachine.StateChanged -= OnStateChanged;
                    oldMachine.TextReady -= OnTextReady;
                    oldMachine.Failed -= OnEngineFailed;
                }

                var recorder = new AudioRecorder(_currentSettings.MicrophoneDeviceId)
                {
                    NoiseReductionEnabled = _currentSettings.NoiseReductionEnabled,
                };
                var machine = new RecordingStateMachine(
                    recorder,
                    _transcription,
                    new TextOutputService(new WpfClipboardWriter(Dispatcher), new InputSimulatorPaster()),
                    _currentSettings.RecordingMode,
                    TimeSpan.FromMilliseconds(_currentSettings.SilenceThresholdMs),
                    GetTranscriptionOptions,
                    () => new SileroSpeechSegmenter(_vadPath!),
                    _logger);

                machine.StateChanged += OnStateChanged;
                machine.TextReady += OnTextReady;
                machine.Failed += OnEngineFailed;

                _stateMachine = machine;

                _logger.Info(Loc.T("Log_EngineReady"));
                _tray?.SetTooltip(Loc.T("App_TrayTooltipReady"));
                _settingsViewModel?.SetStatus(Loc.T("Status_ModelLoaded"));
                _settingsViewModel?.ClearModelDownload();
            }
            catch (Exception ex)
            {
                _logger.Error(Loc.T("Log_EngineFail"), ex);
                _tray?.ShowBalloon(Loc.T("App_MessageBoxTitle"), Loc.Format("Log_ModelErrorBalloon", ex.Message));
                _settingsViewModel?.SetStatus(Loc.T("Status_EngineError"));
            }
            finally
            {
                await (oldMachine?.DisposeAsync() ?? ValueTask.CompletedTask);
            }
        }
        finally
        {
            _engineInitializing = false;
            _engineInitLock.Release();
        }
    }

    private TranscriptionOptions GetTranscriptionOptions() => new(
        _currentSettings.Language,
        _currentSettings.TermsDictionary,
        _currentSettings.AutoPasteEnabled,
        (float)_currentSettings.Temperature,
        _currentSettings.ConditionOnPreviousText);

    /// <summary>Проверяет, что выбранный микрофон реально захватывается, и логирует результат.</summary>
    private async Task TestMicrophoneAsync()
    {
        try
        {
            await Task.Delay(500);
            using var recorder = new AudioRecorder(_currentSettings.MicrophoneDeviceId);
            recorder.Start();
            var backend = recorder.ActiveBackend;
            await Task.Delay(1500);
            var wav = recorder.Stop();
            if (wav is not null && wav.Length > 44)
            {
                var ms = (wav.Length - 44) * 1000.0 / (16000.0 * 2);
                _logger.Info(Loc.Format("Log_TestMicSuccess", backend, Math.Round(ms)));
            }
            else
            {
                _logger.Warn(Loc.T("Log_TestMicNoData"));
            }
        }
        catch (Exception ex)
        {
            _logger.Error(Loc.T("Log_TestMicError"), ex);
        }
    }

    private void ShowSettingsWindow()
    {
        if (_mainWindow is null)
        {
            _mainWindow = new MainWindow(_settingsViewModel!);
            _mainWindow.Closing += OnMainWindowClosing;
        }

        _mainWindow.Show();
        _mainWindow.Activate();
    }

    private void OnMainWindowClosing(object? sender, CancelEventArgs e)
    {
        if (!_isQuitting)
        {
            e.Cancel = true;
            _mainWindow!.Hide();
        }
    }

    private void OnSettingsApplied()
    {
        _currentSettings = _settingsService!.Load();

        // Смена языка интерфейса — применяем её и обновляем элементы, созданные один раз.
        if (_currentSettings.AppLanguage != _lastAppliedLanguage)
        {
            Loc.Instance.Apply(_currentSettings.AppLanguage);
            _lastAppliedLanguage = _currentSettings.AppLanguage;
            _tray?.ApplyLanguage();
        }

        _settingsViewModel?.SetStatus(Loc.T("Status_Saved"));
        _logger.Info(Loc.Format("Log_SettingsApplied", _currentSettings.RecordingMode, _currentSettings.Language,
            _currentSettings.ModelSize, _currentSettings.MicrophoneDeviceId ?? Loc.T("Log_DefaultMic")));

        var modelChanged = _currentSettings.ModelSize != _loadedModelSize;
        var micChanged = _currentSettings.MicrophoneDeviceId != _loadedMicrophoneId;
        var behaviorChanged = _currentSettings.RecordingMode != _lastRecordingMode
                              || _currentSettings.SilenceThresholdMs != _lastSilenceThresholdMs;

        if (_currentSettings.Theme != _lastAppliedTheme)
        {
            ThemeManager.Apply(_currentSettings.Theme);
            _lastAppliedTheme = _currentSettings.Theme;
        }

        if (modelChanged || micChanged || behaviorChanged)
        {
            _settingsViewModel?.SetStatus(modelChanged
                ? Loc.T("Status_ModelReloading")
                : behaviorChanged
                    ? Loc.T("Status_ApplyingMode")
                    : Loc.T("Status_ApplyingMic"));
            _ = InitializeEngineAsync();
        }
        else
        {
            _settingsViewModel?.SetStatus(Loc.T("Status_Saved"));
        }
    }

    private void OnRecordPressed()
    {
        if (_stateMachine is null || _engineInitializing ||
            !_modelManager!.IsModelDownloaded(_currentSettings.ModelSize))
        {
            _tray?.ShowBalloon(Loc.T("App_MessageBoxTitle"), Loc.T("Status_ModelNotReady"));
            return;
        }

        _stateMachine.PressRecord();
        if (_currentSettings.RecordingMode == RecordingMode.PushToTalk)
        {
            _ = DetectRecordReleaseAsync();
        }
    }

    private async Task DetectRecordReleaseAsync()
    {
        try
        {
            await HotkeyReleaseDetector.WaitForKeyRelease(_hotkeys!.RecordKey);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        _stateMachine?.ReleaseRecord();
    }

    private void OnCancelPressed() => _stateMachine?.Cancel();

    private void OnTrayRecord()
    {
        if (_stateMachine is null || _engineInitializing ||
            !_modelManager!.IsModelDownloaded(_currentSettings.ModelSize))
        {
            _tray?.ShowBalloon(Loc.T("App_MessageBoxTitle"), Loc.T("Status_ModelNotReady"));
            return;
        }

        if (_stateMachine.State == RecordingState.Idle)
        {
            _stateMachine.PressRecord();
            if (_currentSettings.RecordingMode == RecordingMode.PushToTalk)
            {
                _ = DetectRecordReleaseAsync();
            }
        }
        else
        {
            _stateMachine.Cancel();
        }
    }

    private IProgress<ModelDownloadProgress> ModelDownloadProgress(string name)
    {
        return new Progress<ModelDownloadProgress>(p =>
            Dispatcher.BeginInvoke(() => _settingsViewModel?.SetModelDownload(name, p)));
    }

    private void CancelModelDownload() => _downloadCts?.Cancel();

    private void OnStateChanged(RecordingState state)
    {
        _logger.Info(Loc.Format("Log_StateChange", state));
        Dispatcher.BeginInvoke(() =>
        {
            _tray?.SetRecording(state == RecordingState.Recording);
            var status = state switch
            {
                RecordingState.Recording => Loc.T("Status_Recording"),
                RecordingState.Processing => Loc.T("Status_Processing"),
                _ => Loc.T("Status_Ready"),
            };
            _settingsViewModel?.SetStatus(status);
            UpdateStatusOverlay(state);
        });
    }

    private void UpdateStatusOverlay(RecordingState state)
    {
        // Создаём лениво и только на UI-потоке (сюда попадаем из Dispatcher.BeginInvoke).
        _statusOverlay ??= new StatusOverlayWindow();

        switch (state)
        {
            case RecordingState.Recording:
                _statusOverlay.ShowStatus(Loc.T("Status_Capturing"), "#4C8BF5");
                break;
            case RecordingState.Processing:
                _statusOverlay.ShowStatus(Loc.T("Status_Processing"), "#F5A623");
                break;
            default:
                _statusOverlay.HideStatus();
                break;
        }
    }

    private void OnTextReady(string text)
    {
        _logger.Info(Loc.Format("Log_TextReady", text.Length, text));
        Dispatcher.BeginInvoke(() => { _settingsViewModel?.SetLastText(text); });
    }

    private string? _lastMicError;

    private void OnEngineFailed(string message)
    {
        _logger.Error(Loc.Format("Log_EngineFailed", message));
        Dispatcher.BeginInvoke(() =>
        {
            _settingsViewModel?.SetStatus(Loc.T("Status_Error"));
            // Показываем уведомление один раз, чтобы не спамить при каждом нажатии.
            if (_lastMicError != message)
            {
                _lastMicError = message;
                _tray?.ShowBalloon(Loc.T("App_BalloonMicError"), message);
            }
        });
    }

    private void Quit()
    {
        _isQuitting = true;
        _hotkeys?.UnregisterAll();
        _tray?.Dispose();
        _mainWindow?.Close();
        Shutdown();
    }

    /// <summary>
    /// Установка обновления: запускаем скрытый наблюдатель (он покажет мастер установки
    /// и перезапустит приложение после его закрытия), затем полностью закрываемся,
    /// чтобы освободить exe и mutex одиночного экземпляра. В трей не сворачиваемся.
    /// </summary>
    private void OnUpdateInstallStarted(string installerPath)
    {
        try
        {
            var appPath = Environment.ProcessPath;
            if (string.IsNullOrWhiteSpace(appPath))
            {
                throw new InvalidOperationException("Не удалось определить путь к приложению.");
            }

            _logger.Info(Loc.Format("Log_UpdateQuit", installerPath));
            UpdateLauncher.Run(installerPath, appPath);
            Quit();
        }
        catch (Exception ex)
        {
            _logger.Error(Loc.Format("Update_InstallFailed", ex.Message));
            if (_settingsViewModel is not null)
            {
                _settingsViewModel.UpdateStatus = Loc.Format("Update_InstallFailed", ex.Message);
            }
        }
    }
}
