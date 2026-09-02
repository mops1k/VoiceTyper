using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using System.Reflection;
using System.Windows;
using System.Windows.Threading;
using VoiceTyper.App.Models;
using VoiceTyper.App.Services;
using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;

namespace VoiceTyper.App.ViewModels;

/// <summary>ViewModel страницы настроек. Сохраняет настройки, применяет хоткеи на лету.</summary>
public sealed partial class SettingsViewModel : ObservableObject
{
    private readonly ISettingsService _settingsService;
    private readonly HotkeyService _hotkeyService;
    private readonly IMicrophoneService _microphoneService;
    private readonly IModelManager _modelManager;
    private string _captureTarget = string.Empty;
    private bool _isInitializing;
    private DispatcherTimer? _autoSaveTimer;

    /// <summary>Возникает после успешного сохранения и применения настроек.</summary>
    public event Action? SettingsApplied;

    /// <summary>Запрос отмены загрузки модели.</summary>
    public event Action? DownloadCancelRequested;

    /// <summary>Открыт ли модальный диалог подтверждения (защита от сворачивания окна в трей).</summary>
    public bool IsModalDialogOpen { get; private set; }

    [ObservableProperty]
    private RecordingMode _recordingMode;

    [ObservableProperty]
    private string _recordHotkey = string.Empty;

    [ObservableProperty]
    private string _cancelHotkey = string.Empty;

    [ObservableProperty]
    private bool _isCapturing;

    [ObservableProperty]
    private string _recordHotkeyHint = string.Empty;

    [ObservableProperty]
    private string _cancelHotkeyHint = string.Empty;

    [ObservableProperty]
    private RecognitionLanguage _language;

    [ObservableProperty]
    private ModelSize _modelSize;

    [ObservableProperty]
    private bool _autoPasteEnabled = true;

    [ObservableProperty]
    private string _termsDictionary = string.Empty;

    [ObservableProperty]
    private int _silenceThresholdMs = 1200;

    [ObservableProperty]
    private bool _startWithWindows;

    [ObservableProperty]
    private bool _startMinimized = true;

    [ObservableProperty]
    private string _statusText = "Загрузка...";

    [ObservableProperty]
    private string _lastText = string.Empty;

    [ObservableProperty]
    private string _errorMessage = string.Empty;

    [ObservableProperty]
    private MicrophoneDevice? _selectedMicrophone;

    [ObservableProperty]
    private IReadOnlyList<MicrophoneDevice> _microphones = Array.Empty<MicrophoneDevice>();

    [ObservableProperty]
    private string _microphoneStatus = string.Empty;

    [ObservableProperty]
    private string _selectedNavItem = "Основные";

    [ObservableProperty]
    private AppTheme _theme = AppTheme.System;

    [ObservableProperty]
    private bool _hideOnFocusLoss;

    [ObservableProperty]
    private bool _noiseReductionEnabled;

    [ObservableProperty]
    private double _temperature;

    [ObservableProperty]
    private bool _conditionOnPreviousText;

    [ObservableProperty]
    private bool _isDownloading;

    [ObservableProperty]
    private double _downloadProgress;

    [ObservableProperty]
    private string _downloadInfo = string.Empty;

    /// <summary>Текущая версия приложения (чистый semver, без git-суффикса "+sha").</summary>
    public string AppVersion { get; } = ResolveAppVersion();

    public IReadOnlyList<AppTheme> Themes { get; } = Enum.GetValues<AppTheme>();

    public IReadOnlyList<double> Temperatures { get; } = new[] { 0.0, 0.2, 0.4, 0.6, 0.8 };

    public IReadOnlyList<ModelListItem> ModelItems { get; private set; } = Array.Empty<ModelListItem>();

    public IReadOnlyList<string> NavItems { get; } =
        new[] { "Основные", "Внешний вид", "Модели", "Горячие клавиши", "Микрофон", "Запуск", "О программе" };

    private void BuildModelItems()
    {
        ModelItems = new[]
        {
            new ModelListItem(ModelSize.Tiny, "Tiny", "Минимальная модель. Быстро распознаёт, подходит для простых задач.", "Очень быстро", "Низкое", "≈ 75 МБ"),
            new ModelListItem(ModelSize.Base, "Base", "Базовая модель. Хороший баланс скорости и качества на CPU.", "Быстро", "Среднее", "≈ 142 МБ"),
            new ModelListItem(ModelSize.Small, "Small", "Рекомендуемая модель. Точное распознавание, умеренная нагрузка.", "Средне", "Высокое", "≈ 466 МБ"),
            new ModelListItem(ModelSize.Medium, "Medium", "Качественное распознавание, заметно медленнее на CPU.", "Медленно", "Очень высокое", "≈ 1,5 ГБ"),
            new ModelListItem(ModelSize.Large, "Large (turbo)", "Максимальное качество. Для мощных процессоров.", "Очень медленно", "Максимальное", "≈ 1,6 ГБ"),
        };
        RefreshModelItems();
    }

    partial void OnModelSizeChanged(ModelSize value)
    {
        if (ModelItems.Count > 0)
        {
            RefreshModelItems();
        }

        ScheduleAutoSave();
    }

    /// <summary>Синхронизирует состояние моделей (скачана / выбрана) с диском и настройкой.</summary>
    private void RefreshModelItems()
    {
        foreach (var item in ModelItems)
        {
            item.IsDownloaded = _modelManager.IsModelDownloaded(item.Size);
            item.IsSelected = item.Size == ModelSize;
        }
    }

    [RelayCommand]
    private void SelectModel(ModelListItem item)
    {
        if (item.Size == ModelSize)
        {
            item.IsSelected = true;
            return;
        }

        ModelSize = item.Size;
        RefreshModelItems();
    }

    [RelayCommand]
    private async Task DownloadModel(ModelListItem item)
    {
        if (item.IsDownloaded)
        {
            return;
        }

        try
        {
            var progress = new Progress<ModelDownloadProgress>(p => UpdateModelUi(item.Name, p));
            await _modelManager.EnsureModelAsync(item.Size, progress);
            await System.Windows.Application.Current.Dispatcher.InvokeAsync(() => item.IsDownloaded = true);
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            await System.Windows.Application.Current.Dispatcher.InvokeAsync(() =>
                ErrorMessage = $"Не удалось скачать модель «{item.Name}»: {ex.Message}");
        }
        finally
        {
            await System.Windows.Application.Current.Dispatcher.InvokeAsync(() =>
            {
                IsDownloading = false;
                DownloadInfo = string.Empty;
                DownloadProgress = 0;
                RefreshModelItems();
            });
        }
    }

    [RelayCommand]
    private void DeleteModel(ModelListItem item)
    {
        if (!item.CanDelete)
        {
            return;
        }

        if (_modelManager.DeleteModel(item.Size))
        {
            item.IsDownloaded = false;
            RefreshModelItems();
            ErrorMessage = $"Модель «{item.Name}» удалена с диска.";
        }
    }

    /// <summary>Извлекает версию приложения из сборки: InformationalVersion (обрезая "+sha") или AssemblyVersion.</summary>
    private static string ResolveAppVersion()
    {
        var asm = typeof(SettingsViewModel).Assembly;
        var info = asm.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
        if (!string.IsNullOrEmpty(info))
        {
            return info.Split('+')[0];
        }
        return asm.GetName().Version?.ToString(3) ?? "1.0.0";
    }

    public IReadOnlyList<RecordingMode> RecordingModes { get; } = Enum.GetValues<RecordingMode>();
    public IReadOnlyList<RecognitionLanguage> Languages { get; } = Enum.GetValues<RecognitionLanguage>();
    public IReadOnlyList<ModelSize> ModelSizes { get; } = Enum.GetValues<ModelSize>();

    public SettingsViewModel(ISettingsService settingsService, HotkeyService hotkeyService, IMicrophoneService microphoneService, IModelManager modelManager)
    {
        _settingsService = settingsService;
        _hotkeyService = hotkeyService;
        _microphoneService = microphoneService;
        _modelManager = modelManager;
        _isInitializing = true;
        try
        {
            LoadFromSettings();
            BuildModelItems();
            RefreshMicrophones();
        }
        finally
        {
            _isInitializing = false;
        }
    }

    /// <summary>Отложенное автосохранение (дебаунс), чтобы текст не сохранялся на каждую клавишу.</summary>
    private void ScheduleAutoSave()
    {
        if (_isInitializing)
        {
            return;
        }

        if (_autoSaveTimer is null)
        {
            _autoSaveTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(700) };
            _autoSaveTimer.Tick += OnAutoSaveTick;
        }

        _autoSaveTimer.Stop();
        _autoSaveTimer.Start();
    }

    private void OnAutoSaveTick(object? sender, EventArgs e)
    {
        _autoSaveTimer?.Stop();
        Save();
    }

    [RelayCommand]
    private void RefreshMicrophones()
    {
        var previousId = SelectedMicrophone?.Id;
        var devices = _microphoneService.GetMicrophones();
        Microphones = devices;
        MicrophoneStatus = devices.Count == 0
            ? "Активные микрофоны не найдены."
            : $"Найдено устройств: {devices.Count}";

        if (previousId is not null && devices.Any(m => m.Id == previousId))
        {
            SelectedMicrophone = devices.First(m => m.Id == previousId);
        }
        else
        {
            SelectedMicrophone = devices.FirstOrDefault();
        }
    }

    public void SetStatus(string text) => StatusText = text;

    public void SetLastText(string text) => LastText = text;

    /// <summary>Обновляет индикатор загрузки модели в статус-баре.</summary>
    public void SetModelDownload(string name, ModelDownloadProgress p) => UpdateModelUi(name, p);

    private void UpdateModelUi(string name, ModelDownloadProgress p)
    {
        IsDownloading = true;
        DownloadProgress = p.Fraction;
        var speed = FormatSize((long)p.BytesPerSecond);
        var downloaded = FormatSize(p.BytesDownloaded);
        var total = FormatSize(p.TotalBytes);
        var eta = p.Remaining is { } r ? $" · осталось ~{FormatSeconds(r)}" : string.Empty;
        DownloadInfo = $"{name}: {downloaded} из {total} · {speed}/с{eta}";
    }

    /// <summary>Скрывает индикатор загрузки.</summary>
    public void ClearModelDownload()
    {
        IsDownloading = false;
        DownloadProgress = 0;
        DownloadInfo = string.Empty;
    }

    [RelayCommand]
    private void CancelDownload() => DownloadCancelRequested?.Invoke();

    private static string FormatSize(long bytes)
    {
        const double mb = 1024.0 * 1024.0;
        const double gb = mb * 1024.0;
        return bytes >= gb ? $"{bytes / gb:F1} ГБ" : bytes >= mb ? $"{bytes / mb:F0} МБ" : $"{bytes / 1024.0:F0} КБ";
    }

    private static string FormatSeconds(TimeSpan t) =>
        t.TotalMinutes >= 1 ? $"{(int)t.TotalMinutes} мин {t.Seconds}с" : $"{Math.Max(1, (int)t.TotalSeconds)}с";

    [RelayCommand]
    private void CaptureRecordHotkey() => BeginCapture("record");

    [RelayCommand]
    private void CaptureCancelHotkey() => BeginCapture("cancel");

    private void BeginCapture(string target)
    {
        if (IsCapturing)
        {
            return;
        }

        _captureTarget = target;

        // Снимаем текущие глобальные хоткеи, чтобы зажатая комбинация не сработала во время захвата.
        _hotkeyService.UnregisterAll();
        IsCapturing = true;

        if (target == "record")
        {
            RecordHotkeyHint = "Нажмите сочетание...";
        }
        else
        {
            CancelHotkeyHint = "Нажмите сочетание...";
        }
    }

    /// <summary>Обработка нажатой комбинации из окна настроек (для захвата).</summary>
    public void SubmitCapturedHotkey(string combo)
    {
        if (!IsCapturing)
        {
            return;
        }

        var target = _captureTarget;
        EndCapture();

        IsModalDialogOpen = true;
        var result = System.Windows.MessageBox.Show(
            $"Новое сочетание клавиш: {combo}\n\nПрименить?",
            "VoiceTyper",
            MessageBoxButton.YesNo,
            MessageBoxImage.Question);
        IsModalDialogOpen = false;

        if (result == MessageBoxResult.Yes && HotkeyParser.TryParse(combo, out _))
        {
            if (target == "record")
            {
                RecordHotkey = combo;
            }
            else
            {
                CancelHotkey = combo;
            }
        }

        _captureTarget = string.Empty;
        RecordHotkeyHint = RecordHotkey;
        CancelHotkeyHint = CancelHotkey;
    }

    /// <summary>Отмена захвата (клавиша Escape или потеря фокуса окном).</summary>
    public void CancelCapture()
    {
        if (!IsCapturing)
        {
            return;
        }

        EndCapture();
        _captureTarget = string.Empty;
        RecordHotkeyHint = RecordHotkey;
        CancelHotkeyHint = CancelHotkey;
    }

    /// <summary>Подсказка, что для глобального хоткея нужен модификатор или F-клавиша.</summary>
    public void NotifyHotkeyNeedsModifier() =>
        ErrorMessage = "Для глобальной горячей клавиши нужен модификатор (Ctrl/Alt/Shift/Win) или F-клавиша.";

    private void EndCapture()
    {
        IsCapturing = false;

        // Возвращаем глобальные хоткеи (текущие, из файла).
        _ = _hotkeyService.ApplySettings(_settingsService.Load());
    }

    partial void OnRecordHotkeyChanged(string value)
    {
        if (!IsCapturing || _captureTarget != "record")
        {
            RecordHotkeyHint = value;
        }

        ScheduleAutoSave();
    }

    partial void OnCancelHotkeyChanged(string value)
    {
        if (!IsCapturing || _captureTarget != "cancel")
        {
            CancelHotkeyHint = value;
        }

        ScheduleAutoSave();
    }

    partial void OnRecordingModeChanged(RecordingMode value) => ScheduleAutoSave();
    partial void OnLanguageChanged(RecognitionLanguage value) => ScheduleAutoSave();
    partial void OnAutoPasteEnabledChanged(bool value) => ScheduleAutoSave();
    partial void OnTermsDictionaryChanged(string value) => ScheduleAutoSave();
    partial void OnSilenceThresholdMsChanged(int value) => ScheduleAutoSave();
    partial void OnStartWithWindowsChanged(bool value) => ScheduleAutoSave();
    partial void OnStartMinimizedChanged(bool value) => ScheduleAutoSave();
    partial void OnThemeChanged(AppTheme value) => ScheduleAutoSave();
    partial void OnHideOnFocusLossChanged(bool value) => ScheduleAutoSave();
    partial void OnSelectedMicrophoneChanged(MicrophoneDevice? value) => ScheduleAutoSave();
    partial void OnNoiseReductionEnabledChanged(bool value) => ScheduleAutoSave();
    partial void OnTemperatureChanged(double value) => ScheduleAutoSave();
    partial void OnConditionOnPreviousTextChanged(bool value) => ScheduleAutoSave();

    private void LoadFromSettings()
    {
        var s = _settingsService.Load();
        RecordingMode = s.RecordingMode;
        RecordHotkey = s.RecordHotkey;
        CancelHotkey = s.CancelHotkey;
        Language = s.Language;
        ModelSize = s.ModelSize;
        AutoPasteEnabled = s.AutoPasteEnabled;
        TermsDictionary = s.TermsDictionary;
        SilenceThresholdMs = s.SilenceThresholdMs;
        StartWithWindows = s.StartWithWindows;
        StartMinimized = s.StartMinimized;
        Theme = s.Theme;
        HideOnFocusLoss = s.HideOnFocusLoss;
        NoiseReductionEnabled = s.NoiseReductionEnabled;
        Temperature = s.Temperature;
        ConditionOnPreviousText = s.ConditionOnPreviousText;
    }

    [RelayCommand]
    private void Save()
    {
        ErrorMessage = string.Empty;

        if (!HotkeyParser.TryParse(RecordHotkey, out _))
        {
            ErrorMessage = "Неверный хоткей записи.";
            return;
        }

        if (!HotkeyParser.TryParse(CancelHotkey, out _))
        {
            ErrorMessage = "Неверный хоткей отмены.";
            return;
        }

        var settings = new AppSettings
        {
            RecordingMode = RecordingMode,
            RecordHotkey = HotkeyParser.Format(HotkeyParser.Parse(RecordHotkey)),
            CancelHotkey = HotkeyParser.Format(HotkeyParser.Parse(CancelHotkey)),
            Language = Language,
            ModelSize = ModelSize,
            AutoPasteEnabled = AutoPasteEnabled,
            TermsDictionary = TermsDictionary,
            SilenceThresholdMs = Math.Clamp(SilenceThresholdMs, 300, 10000),
            StartWithWindows = StartWithWindows,
            StartMinimized = StartMinimized,
            Theme = Theme,
            HideOnFocusLoss = HideOnFocusLoss,
            NoiseReductionEnabled = NoiseReductionEnabled,
            Temperature = Temperature,
            ConditionOnPreviousText = ConditionOnPreviousText,
            MicrophoneDeviceId = SelectedMicrophone?.Id,
        };

        var errors = _hotkeyService.ApplySettings(settings);
        _settingsService.Save(settings);
        StartupManager.SetRunAtStartup(settings.StartWithWindows);

        if (errors.Count > 0)
        {
            ErrorMessage = string.Join(" ", errors);
        }

        StatusText = "Сохранено";
        SettingsApplied?.Invoke();
    }

    [RelayCommand]
    private void ResetDefaults()
    {
        var defaults = new AppSettings();
        RecordingMode = defaults.RecordingMode;
        RecordHotkey = defaults.RecordHotkey;
        CancelHotkey = defaults.CancelHotkey;
        Language = defaults.Language;
        ModelSize = defaults.ModelSize;
        AutoPasteEnabled = defaults.AutoPasteEnabled;
        TermsDictionary = defaults.TermsDictionary;
        SilenceThresholdMs = defaults.SilenceThresholdMs;
        StartWithWindows = defaults.StartWithWindows;
        StartMinimized = defaults.StartMinimized;
        Theme = defaults.Theme;
        HideOnFocusLoss = defaults.HideOnFocusLoss;
        NoiseReductionEnabled = defaults.NoiseReductionEnabled;
        Temperature = defaults.Temperature;
        ConditionOnPreviousText = defaults.ConditionOnPreviousText;
        ErrorMessage = string.Empty;
    }
}

