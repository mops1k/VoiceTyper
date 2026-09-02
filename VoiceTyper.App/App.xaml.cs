using System.ComponentModel;
using System.Windows;
using Application = System.Windows.Application;
using MessageBox = System.Windows.MessageBox;
using VoiceTyper.App.Services;
using VoiceTyper.App.Tray;
using VoiceTyper.App.Overlay;
using VoiceTyper.App.ViewModels;
using VoiceTyper.Core.Abstractions;
using VoiceTyper.Core.Audio;
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

        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
            _logger.Error("Необработанное исключение (AppDomain).", args.ExceptionObject as Exception);
        TaskScheduler.UnobservedTaskException += (_, args) =>
        {
            _logger.Error("Необработанное исключение задачи (Task).", args.Exception);
            args.SetObserved();
        };

        DispatcherUnhandledException += (_, args) =>
        {
            _logger.Error("Необработанное исключение (UI-поток).", args.Exception);
            MessageBox.Show($"Необработанная ошибка: {args.Exception.Message}", "VoiceTyper", MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        };

        _logger.Info($"=== VoiceTyper запуск (PID {Environment.ProcessId}) ===");
        _logger.Info($"Версия: {Environment.Version}, ОС: {Environment.OSVersion}, процессор: {Environment.ProcessorCount} ядер");
        _logger.Info($"Логи: {_logger.LogDirectory}");
        _mutex = new Mutex(initiallyOwned: true, MutexName, out var createdNew);
        if (!createdNew)
        {
            _logger.Warn("Обнаружен второй экземпляр — завершение.");
            MessageBox.Show("VoiceTyper уже запущен.", "VoiceTyper", MessageBoxButton.OK, MessageBoxImage.Information);
            Shutdown();
            return;
        }

        _settingsService = new SettingsService();
        _modelManager = new ModelManager();
        _microphoneService = new MicrophoneService();
        _currentSettings = _settingsService.Load();
        ThemeManager.Apply(_currentSettings.Theme);
        _lastAppliedTheme = _currentSettings.Theme;
        _logger.Info($"Настройки: {_settingsService.SettingsFilePath}");
        _logger.Info($"Настройки: режим={_currentSettings.RecordingMode}, язык={_currentSettings.Language}, " +
                     $"модель={_currentSettings.ModelSize}, автовставка={_currentSettings.AutoPasteEnabled}, " +
                     $"микрофон={_currentSettings.MicrophoneDeviceId ?? "(по умолчанию)"}");
        _logger.Info($"Хоткеи: запись='{_currentSettings.RecordHotkey}', отмена='{_currentSettings.CancelHotkey}'");

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
            _logger.Warn("Регистрация хоткея: " + error);
            _tray.ShowBalloon("VoiceTyper", error);
        }

        var microphones = _microphoneService.GetMicrophones();
        _logger.Info(microphones.Count == 0
            ? "Микрофоны: активные устройства не найдены."
            : "Микрофоны: " + string.Join(" | ", microphones.Select(m => m.Name)));

        _settingsViewModel = new SettingsViewModel(_settingsService, _hotkeys, _microphoneService, _modelManager);
        _settingsViewModel.SettingsApplied += OnSettingsApplied;
        _settingsViewModel.DownloadCancelRequested += CancelModelDownload;
        _settingsViewModel.SetStatus("Готов");
        ThemeManager.ThemeApplied += () => _tray?.ApplyTheme(ThemeManager.IsSystemDark);

        _ = InitializeEngineAsync();
        _ = TestMicrophoneAsync();

        if (!_currentSettings.StartMinimized)
        {
            ShowSettingsWindow();
        }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _logger.Info("=== VoiceTyper завершение ===");
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
                    _logger.Info("Инициализация движка: подготовка модели...");
                    _downloadCts = new CancellationTokenSource();
                    try
                    {
                        var modelPath = await _modelManager!.EnsureModelAsync(_currentSettings.ModelSize, ModelDownloadProgress("Модель Whisper"), _downloadCts.Token);
                        _vadPath = await _modelManager.EnsureVadModelAsync(ModelDownloadProgress("VAD"), _downloadCts.Token);
                        _loadedModelSize = _currentSettings.ModelSize;

                        var oldTranscription = _transcription;
                        _transcription = new WhisperTranscriptionService(modelPath);
                        await (oldTranscription?.DisposeAsync() ?? ValueTask.CompletedTask);

                        _logger.Info($"Модель Whisper: {modelPath}");
                        _logger.Info($"Модель VAD: {_vadPath}");
                    }
                    catch (OperationCanceledException)
                    {
                        _logger.Warn("Загрузка модели отменена пользователем.");
                        _settingsViewModel?.SetStatus("Скачивание отменено");
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
                _logger.Info($"Микрофон (устройство): {_currentSettings.MicrophoneDeviceId ?? "(по умолчанию)"}");

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
                    () => new SileroSpeechSegmenter(_vadPath!));

                machine.StateChanged += OnStateChanged;
                machine.TextReady += OnTextReady;
                machine.Failed += OnEngineFailed;

                _stateMachine = machine;

                _logger.Info("Движок инициализирован успешно.");
                _tray?.SetTooltip("VoiceTyper — готов");
                _settingsViewModel?.SetStatus("Модель загружена");
                _settingsViewModel?.ClearModelDownload();
            }
            catch (Exception ex)
            {
                _logger.Error("Не удалось подготовить движок/модель.", ex);
                _tray?.ShowBalloon("VoiceTyper", $"Ошибка подготовки модели: {ex.Message}");
                _settingsViewModel?.SetStatus("Ошибка загрузки модели");
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
                _logger.Info($"Тест микрофона: УСПЕХ. Бэкенд={backend}, считано данных ≈ {ms:F0} мс.");
            }
            else
            {
                _logger.Warn("Тест микрофона: запись не дала данных (проверьте уровень микрофона).");
            }
        }
        catch (Exception ex)
        {
            _logger.Error("Тест микрофона: НЕ УДАЛОСЬ захватить выбранный микрофон.", ex);
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
        _settingsViewModel?.SetStatus("Сохранено");
        _logger.Info("Применены настройки: " +
                     $"режим={_currentSettings.RecordingMode}, язык={_currentSettings.Language}, " +
                     $"модель={_currentSettings.ModelSize}, микрофон={_currentSettings.MicrophoneDeviceId ?? "(по умолчанию)"}");

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
            _settingsViewModel?.SetStatus(modelChanged ? "Перезагрузка модели..." : behaviorChanged ? "Применение режима..." : "Применение микрофона...");
            _ = InitializeEngineAsync();
        }
        else
        {
            _settingsViewModel?.SetStatus("Сохранено");
        }
    }

    private void OnRecordPressed()
    {
        if (_stateMachine is null || _engineInitializing || !_modelManager!.IsModelDownloaded(_currentSettings.ModelSize))
        {
            _tray?.ShowBalloon("VoiceTyper", "Модель скачивается или ещё не загружена — запись отключена.");
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
        if (_stateMachine is null || _engineInitializing || !_modelManager!.IsModelDownloaded(_currentSettings.ModelSize))
        {
            _tray?.ShowBalloon("VoiceTyper", "Модель скачивается или ещё не загружена — запись отключена.");
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
    {        _logger.Info("Состояние записи -> " + state);
        Dispatcher.BeginInvoke(() =>
        {
            _tray?.SetRecording(state == RecordingState.Recording);
            var status = state switch
            {
                RecordingState.Recording => "Запись...",
                RecordingState.Processing => "Распознавание...",
                _ => "Готов",
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
                _statusOverlay.ShowStatus("Захват", "#4C8BF5");
                break;
            case RecordingState.Processing:
                _statusOverlay.ShowStatus("Распознавание...", "#F5A623");
                break;
            default:
                _statusOverlay.HideStatus();
                break;
        }
    }

    private void OnTextReady(string text)
    {
        _logger.Info($"Распознанный текст ({text.Length} симв.): {text}");
        Dispatcher.BeginInvoke(() =>
        {
            _settingsViewModel?.SetLastText(text);
        });
    }

    private string? _lastMicError;

    private void OnEngineFailed(string message)
    {
        _logger.Error("Ошибка записи/распознавания: " + message);
        Dispatcher.BeginInvoke(() =>
        {
            _settingsViewModel?.SetStatus("Ошибка");
            // Показываем уведомление один раз, чтобы не спамить при каждом нажатии.
            if (_lastMicError != message)
            {
                _lastMicError = message;
                _tray?.ShowBalloon("VoiceTyper — микрофон", message);
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
}
