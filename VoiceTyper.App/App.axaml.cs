using System.Globalization;
using System.IO;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using VoiceTyper.App.Overlay;
using VoiceTyper.App.Services;
using VoiceTyper.App.ViewModels;
using VoiceTyper.Core.Abstractions;
using VoiceTyper.Core.Audio;
using VoiceTyper.Core.Localization;
using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;
using VoiceTyper.Core.Services.Transcription;
using AppTrayIcon = VoiceTyper.App.Tray.TrayIcon;

namespace VoiceTyper.App;

/// <summary>
/// Композиционный корень: создаёт сервисы, регистрирует хоткеи, иконку трея,
/// асинхронно подготавливает модель и связывает конечный автомат записи.
/// </summary>
public partial class App : Application
{
    private const string MutexName = "Global\\VoiceTyper_SingleInstance";

    private Mutex? _mutex;
    private AppTrayIcon? _tray;
    private HotkeyService? _hotkeys;
    private GamepadInputService? _gamepad;
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
    private ITranscriptionEngine? _transcription;
    private IEngineManager? _engineManager;
    private ISpeechSegmenter? _vadSegmenter;
    private TranscriptionEngine _loadedEngine;
    private ModelSize _loadedModelSize;
    private ParakeetModelSize _loadedParakeetSize;
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

    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            // Приложение живёт в трее: закрытие последнего окна не завершает процесс.
            desktop.ShutdownMode = ShutdownMode.OnExplicitShutdown;

            AppDomain.CurrentDomain.UnhandledException += (_, args) =>
                _logger.Error(Loc.T("Log_UnhandledAppDomain"), args.ExceptionObject as Exception);
            TaskScheduler.UnobservedTaskException += (_, args) =>
            {
                _logger.Error(Loc.T("Log_UnhandledTask"), args.Exception);
                args.SetObserved();
            };

            _logger.Clear();
            StartupCore(desktop);
        }

        base.OnFrameworkInitializationCompleted();
    }

    private void StartupCore(IClassicDesktopStyleApplicationLifetime desktop)
    {
        _settingsService = new SettingsService();
        _currentSettings = _settingsService.Load();

        // Язык применяем сразу после загрузки настроек, но до первого лога/UI.
        var effectiveLanguage = ResolveEffectiveLanguage();
        Loc.Instance.Apply(effectiveLanguage);
        _lastAppliedLanguage = effectiveLanguage;
        if (effectiveLanguage != _currentSettings.AppLanguage)
        {
            _currentSettings.AppLanguage = effectiveLanguage;
            _settingsService.Save(_currentSettings);
        }

        _logger.Info(Loc.Format("Log_Startup", Environment.ProcessId));
        _logger.Info(Loc.Format("Log_System", Environment.Version, Environment.OSVersion, Environment.ProcessorCount));
        _logger.Info(Loc.Format("Log_LogDirectory", _logger.LogDirectory));

        _mutex = new Mutex(initiallyOwned: true, MutexName, out var createdNew);
        if (!createdNew)
        {
            _logger.Warn(Loc.T("Log_SecondInstance"));
            NotifySecondInstance(desktop);
            return;
        }

        _modelManager = new ModelManager();
        _modelManager.CleanupLegacyModels();
        _engineManager = new EngineManager(_logger);
        _microphoneService = new MicrophoneService();
        _updateService = new GithubUpdateService();
        ThemeManager.Apply(_currentSettings.Theme);
        _lastAppliedTheme = _currentSettings.Theme;

        _logger.Info(Loc.Format("Log_SettingsPath", _settingsService.SettingsFilePath));
        _logger.Info(Loc.Format("Log_SettingsSummary", _currentSettings.RecordingMode, _currentSettings.Language,
            _currentSettings.ModelSize, _currentSettings.AutoPasteEnabled,
            _currentSettings.MicrophoneDeviceId ?? Loc.T("Log_DefaultMic")));
        _logger.Info(Loc.Format("Log_Hotkeys", _currentSettings.RecordHotkey, _currentSettings.CancelHotkey));

        _tray = new AppTrayIcon();
        _tray.ApplyTheme(ThemeManager.IsSystemDark);
        _tray.OpenSettingsRequested += ShowSettingsWindow;
        _tray.RecordRequested += OnTrayRecord;
        _tray.QuitRequested += Quit;

        _hotkeys = new HotkeyService();
        _hotkeys.RecordPressed += () => Dispatcher.UIThread.Post(OnRecordPressed);
        _hotkeys.CancelPressed += () => Dispatcher.UIThread.Post(OnCancelPressed);
        foreach (var error in _hotkeys.ApplySettings(_currentSettings))
        {
            _logger.Warn(Loc.Format("Log_HotkeyRegistration", error));
            _tray.ShowBalloon(Loc.T("App_MessageBoxTitle"), error);
        }

        _gamepad = new GamepadInputService();
        _gamepad.ApplySettings(_currentSettings);
        _gamepad.RecordPressed += () => Dispatcher.UIThread.Post(OnGamepadRecordPressed);
        _gamepad.CancelPressed += () => Dispatcher.UIThread.Post(OnCancelPressed);
        _gamepad.Start();

        var microphones = _microphoneService.GetMicrophones();
        _logger.Info(microphones.Count == 0
            ? Loc.T("Log_MicNone")
            : Loc.Format("Log_MicList", string.Join(" | ", microphones.Select(m => m.Name))));

        _settingsViewModel = new SettingsViewModel(
            _settingsService,
            _hotkeys,
            _gamepad,
            _microphoneService,
            _modelManager,
            _updateService,
            new AvaloniaDialogService());
        _settingsViewModel.SettingsApplied += OnSettingsApplied;
        _settingsViewModel.DownloadCancelRequested += CancelModelDownload;
        _settingsViewModel.UpdateAvailable += v =>
            _tray?.ShowBalloon(Loc.T("App_MessageBoxTitle"), Loc.Format("Update_AvailableBalloon", v));
        _settingsViewModel.UpdateInstallStarted += OnUpdateInstallStarted;
        _settingsViewModel.SetStatus(Loc.T("Status_Ready"));
        ThemeManager.ThemeApplied += () => _tray?.ApplyTheme(ThemeManager.IsSystemDark);

        _mainWindow = new MainWindow();
        _mainWindow.SetViewModel(_settingsViewModel);
        desktop.MainWindow = _mainWindow;

        // MainWindow создаём всегда: он нужен как TopLevel для буфера обмена.
        // При запуске в трей окно остаётся невидимым до команды из трея.
        if (!_currentSettings.StartMinimized)
        {
            _mainWindow.Show();
        }

        _ = _settingsViewModel.CheckForUpdatesAsync(auto: true);
        _ = InitializeEngineAsync();
        _ = TestMicrophoneAsync();
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

    /// <summary>Запущен второй экземпляр — показываем сообщение и выходим.</summary>
    private void NotifySecondInstance(IClassicDesktopStyleApplicationLifetime desktop)
    {
        Dispatcher.UIThread.Post(async () =>
        {
            try
            {
                var dialog = new AvaloniaDialogService();
                await dialog.InfoAsync(Loc.T("App_AlreadyRunning"), Loc.T("App_MessageBoxTitle"));
            }
            finally
            {
                _isQuitting = true;
                _mutex?.Dispose();
                desktop.Shutdown();
            }
        });
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
                // Модель (и VAD) грузим только при смене движка/размера модели или первом запуске.
                // При смене РЕЖИМА/ПОРОГА пересоздаём только конечный автомат — модель остаётся в памяти.
                var engine = _currentSettings.TranscriptionEngine;
                var engineReady = _transcription is not null && _loadedEngine == engine &&
                    (engine != TranscriptionEngine.Whisper
                        ? _loadedParakeetSize == _currentSettings.ParakeetModelSize
                        : _loadedModelSize == _currentSettings.ModelSize);

                if (!engineReady)
                {
                    _logger.Info(Loc.T("Log_EngineInitModel"));
                    _logger.Info(Loc.Format("Log_EngineSelected", engine));
                    _downloadCts = new CancellationTokenSource();
                    try
                    {
                        // Silero VAD нужен обоим движкам (режим VAD).
                        _vadPath = await _modelManager!.EnsureVadModelAsync(
                            ModelDownloadProgress(Loc.T("Models_VadLabel")), _downloadCts.Token);

                        string modelPath;
                        if (engine == TranscriptionEngine.Parakeet)
                        {
                            if (!_engineManager!.IsAvailable(TranscriptionEngine.Parakeet))
                            {
                                // Явная ошибка, без фолбэка на Whisper (решение №7).
                                throw new InvalidOperationException(Loc.T("Engine_ParakeetNativeMissing"));
                            }

                            modelPath = await _modelManager.EnsureParakeetModelAsync(_currentSettings.ParakeetModelSize,
                                ModelDownloadProgress(Loc.T("Models_Label")), _downloadCts.Token);
                            _loadedParakeetSize = _currentSettings.ParakeetModelSize;
                        }
                        else
                        {
                            modelPath = await _modelManager.EnsureModelAsync(_currentSettings.ModelSize,
                                ModelDownloadProgress(Loc.T("Models_WhisperLabel")), _downloadCts.Token);
                            _loadedModelSize = _currentSettings.ModelSize;
                        }

                        _loadedEngine = engine;

                        var oldTranscription = _transcription;
                        _transcription = _engineManager!.Create(engine, modelPath);
                        _transcription.Warmup();
                        await (oldTranscription?.DisposeAsync() ?? ValueTask.CompletedTask);

                        // Глубокий прогрев в фоне: один короткий инференс, чтобы первая
                        // реальная диктовка не включала инициализацию compute-пути.
                        var forWarmup = _transcription;
                        _ = Task.Run(async () =>
                        {
                            var sw = System.Diagnostics.Stopwatch.StartNew();
                            try
                            {
                                await forWarmup.WarmupAsync(CancellationToken.None);
                                _logger.Info(Loc.Format("Log_EngineWarmupDone", sw.ElapsedMilliseconds));
                            }
                            catch (OperationCanceledException)
                            {
                            }
                            catch (Exception ex)
                            {
                                _logger.Warn($"Прогрев: {ex.Message}");
                            }
                        });

                        _logger.Info(engine == TranscriptionEngine.Parakeet
                            ? Loc.Format("Log_ModelParakeet", modelPath)
                            : Loc.Format("Log_ModelWhisper", modelPath));
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

                if (_transcription is null)
                {
                    throw new InvalidOperationException("не инициализирован движок распознавания");
                }

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
                    new TextOutputService(new AvaloniaClipboardWriter(), new InputSimulatorPaster()),
                    _currentSettings.RecordingMode,
                    TimeSpan.FromMilliseconds(_currentSettings.SilenceThresholdMs),
                    GetTranscriptionOptions,
                    // Сегментер VAD кэшируется на всё время жизни приложения: модель
                    // Silero не грузится заново на каждую сессию записи.
                    () => GetOrCreateVadSegmenter(_vadPath!),
                    _logger);

                machine.StateChanged += OnStateChanged;
                machine.TextReady += OnTextReady;
                machine.Failed += OnEngineFailed;

                _stateMachine = machine;

                // Глубокий прогрев VAD в фоне (важно для режима VAD): один вызов детекции
                // на коротком куске тишины инициализирует WhisperaufenVAD-контекст.
                var vadPathSnapshot = _vadPath;
                if (vadPathSnapshot is not null)
                {
                    _ = Task.Run(() =>
                    {
                        var sw = System.Diagnostics.Stopwatch.StartNew();
                        try
                        {
                            var seg = GetOrCreateVadSegmenter(vadPathSnapshot);
                            seg.DetectSpeechNoReset(new float[16000]); // 1 с тишины
                            seg.ResetState();
                            _logger.Info(Loc.Format("Log_VadWarmupDone", sw.ElapsedMilliseconds));
                        }
                        catch (Exception ex)
                        {
                            _logger.Warn($"VAD-прогрев: {ex.Message}");
                        }
                    });
                }

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
        if (_isQuitting)
        {
            return;
        }

        if (_mainWindow is null || !_mainWindow.IsVisible)
        {
            _mainWindow = new MainWindow();
            _mainWindow.SetViewModel(_settingsViewModel!);
            if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            {
                desktop.MainWindow = _mainWindow;
            }
        }

        _mainWindow.Show();
        _mainWindow.Activate();
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

        _gamepad?.ApplySettings(_currentSettings);

        var engineChanged = _currentSettings.TranscriptionEngine != _loadedEngine;
        var activeChanged = _currentSettings.TranscriptionEngine == TranscriptionEngine.Whisper
            ? _currentSettings.ModelSize != _loadedModelSize
            : _currentSettings.ParakeetModelSize != _loadedParakeetSize;
        var modelChanged = engineChanged || activeChanged;
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

    private void OnRecordPressed() => StartRecord(_hotkeys!.RecordKeyVk);

    private void OnGamepadRecordPressed() => StartRecord(null);

    private bool IsActiveModelDownloaded() =>
        _currentSettings.TranscriptionEngine == TranscriptionEngine.Whisper
            ? _modelManager!.IsModelDownloaded(_currentSettings.ModelSize)
            : _modelManager!.IsParakeetModelDownloaded(_currentSettings.ParakeetModelSize);

    private void StartRecord(int? keyboardVk)
    {
        if (_stateMachine is null || _engineInitializing || !IsActiveModelDownloaded())
        {
            _tray?.ShowBalloon(Loc.T("App_MessageBoxTitle"), Loc.T("Status_ModelNotReady"));
            return;
        }

        _stateMachine.PressRecord();
        if (_currentSettings.RecordingMode == RecordingMode.PushToTalk)
        {
            _ = keyboardVk.HasValue
                ? DetectRecordReleaseAsync(keyboardVk.Value)
                : DetectGamepadRecordReleaseAsync();
        }
    }

    private async Task DetectRecordReleaseAsync(int vk)
    {
        try
        {
            await HotkeyService.WaitForKeyRelease(vk);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        _stateMachine?.ReleaseRecord();
    }

    private async Task DetectGamepadRecordReleaseAsync()
    {
        try
        {
            await _gamepad!.WaitForRecordReleaseAsync();
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
        if (_stateMachine is null || _engineInitializing || !IsActiveModelDownloaded())
        {
            _tray?.ShowBalloon(Loc.T("App_MessageBoxTitle"), Loc.T("Status_ModelNotReady"));
            return;
        }

        if (_stateMachine.State == RecordingState.Idle)
        {
            _stateMachine.PressRecord();
            if (_currentSettings.RecordingMode == RecordingMode.PushToTalk)
            {
                _ = DetectRecordReleaseAsync(_hotkeys!.RecordKeyVk);
            }
        }
        else
        {
            _stateMachine.Cancel();
        }
    }

    /// <summary>Кэшированный VAD-сегментер: модель Silero грузится один раз за работу.</summary>
    private ISpeechSegmenter GetOrCreateVadSegmenter(string vadPath)
    {
        lock (this)
        {
            if (_vadSegmenter is null)
            {
                _vadSegmenter = new SileroSpeechSegmenter(vadPath);
                _logger.Info(Loc.Format("Log_VadModelLoaded", vadPath));
            }

            return _vadSegmenter;
        }
    }

    private IProgress<ModelDownloadProgress> ModelDownloadProgress(string name)
    {
        return new Progress<ModelDownloadProgress>(p =>
            Dispatcher.UIThread.Post(() => _settingsViewModel?.SetModelDownload(name, p)));
    }

    private void CancelModelDownload() => _downloadCts?.Cancel();

    private void OnStateChanged(RecordingState state)
    {
        _logger.Info(Loc.Format("Log_StateChange", state));
        Dispatcher.UIThread.Post(() =>
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
        // Создаём лениво и только на UI-потоке.
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
        Dispatcher.UIThread.Post(() => _settingsViewModel?.SetLastText(text));
    }

    private string? _lastMicError;

    private void OnEngineFailed(string message)
    {
        _logger.Error(Loc.Format("Log_EngineFailed", message));
        Dispatcher.UIThread.Post(() =>
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
        _gamepad?.Dispose();
        _tray?.Dispose();
        _statusOverlay?.Close();
        _ = _stateMachine?.DisposeAsync() ?? ValueTask.CompletedTask;
        _ = _transcription?.DisposeAsync() ?? ValueTask.CompletedTask;
        _vadSegmenter?.Dispose();
        _mainWindow?.Close();
        _mutex?.Dispose();
        if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.Shutdown();
        }
        else
        {
            Environment.Exit(0);
        }
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
