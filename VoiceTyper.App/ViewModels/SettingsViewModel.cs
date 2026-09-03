using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using System.Reflection;
using System.Windows;
using System.Windows.Threading;
using VoiceTyper.App.Models;
using VoiceTyper.App.Services;
using VoiceTyper.Core.Localization;
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
    private readonly IUpdateService _updateService;
    private string _captureTarget = string.Empty;
    private bool _isInitializing;
    private DispatcherTimer? _autoSaveTimer;
    private CancellationTokenSource? _updateDownloadCts;
    private UpdateInfo? _pendingUpdate;

    /// <summary>Возникает после успешного сохранения и применения настроек.</summary>
    public event Action? SettingsApplied;

    /// <summary>Запрос отмены загрузки модели.</summary>
    public event Action? DownloadCancelRequested;

    /// <summary>Найдена новая версия (для трей-balloon). Параметр — номер версии.</summary>
    public event Action<string>? UpdateAvailable;

    /// <summary>Начата установка обновления. Параметр — путь к скачанному установщику;
    /// приложение должно полностью закрыться, а установщик — запуститься.</summary>
    public event Action<string>? UpdateInstallStarted;

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
    private string _statusText = Loc.T("Status_Loading");

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
    private string _selectedNavItem = "Main";

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

    [ObservableProperty]
    private AppLanguage _appLanguage = AppLanguage.Ru;

    [ObservableProperty]
    private bool _isCheckingUpdate;

    [ObservableProperty]
    private bool _isUpdateAvailable;

    [ObservableProperty]
    private bool _isDownloadingUpdate;

    [ObservableProperty]
    private double _updateDownloadProgress;

    [ObservableProperty]
    private string _updateStatus = string.Empty;

    [ObservableProperty]
    private string _latestVersion = string.Empty;

    [ObservableProperty]
    private string _releaseNotes = string.Empty;

    [ObservableProperty]
    private bool _isUpToDateVersion;

    /// <summary>Текущая версия приложения (чистый semver, без git-суффикса "+sha").</summary>
    public string AppVersion { get; } = ResolveAppVersion();

    public IReadOnlyList<LocalizedOption<AppTheme>> Themes { get; private set; } = Array.Empty<LocalizedOption<AppTheme>>();

    public IReadOnlyList<double> Temperatures { get; } = new[] { 0.0, 0.2, 0.4, 0.6, 0.8 };

    public IReadOnlyList<ModelListItem> ModelItems { get; private set; } = Array.Empty<ModelListItem>();

    public IReadOnlyList<LocalizedNavItem> NavItems { get; private set; } = Array.Empty<LocalizedNavItem>();

    public IReadOnlyList<LocalizedOption<RecordingMode>> RecordingModes { get; private set; } = Array.Empty<LocalizedOption<RecordingMode>>();

    public IReadOnlyList<LocalizedOption<RecognitionLanguage>> Languages { get; private set; } = Array.Empty<LocalizedOption<RecognitionLanguage>>();

    public IReadOnlyList<LocalizedOption<AppLanguage>> UiLanguages { get; private set; } = Array.Empty<LocalizedOption<AppLanguage>>();

    public IReadOnlyList<ModelSize> ModelSizes { get; } = Enum.GetValues<ModelSize>();

    private static IReadOnlyList<LocalizedNavItem> BuildNavItems() => new[]
    {
        new LocalizedNavItem("Main", "\uE80F"),
        new LocalizedNavItem("Appearance", "\uE90F"),
        new LocalizedNavItem("Models", "\uE7B8"),
        new LocalizedNavItem("Hotkeys", "\uE765"),
        new LocalizedNavItem("Microphone", "\uE720"),
        new LocalizedNavItem("Startup", "\uE768"),
        new LocalizedNavItem("About", "\uE946"),
    };

    private static IReadOnlyList<LocalizedOption<RecordingMode>> BuildRecordingModes() => new[]
    {
        new LocalizedOption<RecordingMode>(RecordingMode.PushToTalk, "Enum_RecordingMode_PushToTalk"),
        new LocalizedOption<RecordingMode>(RecordingMode.Toggle, "Enum_RecordingMode_Toggle"),
        new LocalizedOption<RecordingMode>(RecordingMode.Vad, "Enum_RecordingMode_Vad"),
    };

    private static IReadOnlyList<LocalizedOption<RecognitionLanguage>> BuildLanguages() => new[]
    {
        new LocalizedOption<RecognitionLanguage>(RecognitionLanguage.Auto, "Enum_RecognitionLanguage_Auto"),
        new LocalizedOption<RecognitionLanguage>(RecognitionLanguage.Ru, "Enum_RecognitionLanguage_Ru"),
        new LocalizedOption<RecognitionLanguage>(RecognitionLanguage.En, "Enum_RecognitionLanguage_En"),
    };

    private static IReadOnlyList<LocalizedOption<AppTheme>> BuildThemes() => new[]
    {
        new LocalizedOption<AppTheme>(AppTheme.System, "Enum_AppTheme_System"),
        new LocalizedOption<AppTheme>(AppTheme.Light, "Enum_AppTheme_Light"),
        new LocalizedOption<AppTheme>(AppTheme.Dark, "Enum_AppTheme_Dark"),
    };

    private static IReadOnlyList<LocalizedOption<AppLanguage>> BuildUiLanguages() => new[]
    {
        new LocalizedOption<AppLanguage>(AppLanguage.Ru, "Enum_AppLanguage_Ru"),
        new LocalizedOption<AppLanguage>(AppLanguage.En, "Enum_AppLanguage_En"),
    };

    private void BuildModelItems()
    {
        ModelItems = new[]
        {
            new ModelListItem(ModelSize.Tiny, "Tiny (q8)", "Models_Tiny_Description", "Models_SpeedVeryFast", "Models_QualityLow", sizeMb: 42),
            new ModelListItem(ModelSize.Base, "Base (q8)", "Models_Base_Description", "Models_SpeedFast", "Models_QualityMedium", sizeMb: 78),
            new ModelListItem(ModelSize.Small, "Small (q8)", "Models_Small_Description", "Models_SpeedMedium", "Models_QualityHigh", sizeMb: 252),
            new ModelListItem(ModelSize.Medium, "Medium (q8)", "Models_Medium_Description", "Models_SpeedSlow", "Models_QualityVeryHigh", sizeMb: 785),
            new ModelListItem(ModelSize.Large, "Large (turbo, q8)", "Models_Large_Description", "Models_SpeedVerySlow", "Models_QualityMax", sizeMb: 834),
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
                ErrorMessage = Loc.Format("Models_DownloadError", item.Name, ex.Message));
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
            ErrorMessage = Loc.Format("Models_DeleteSuccess", item.Name);
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

    public SettingsViewModel(ISettingsService settingsService, HotkeyService hotkeyService, IMicrophoneService microphoneService, IModelManager modelManager, IUpdateService updateService)
    {
        _settingsService = settingsService;
        _hotkeyService = hotkeyService;
        _microphoneService = microphoneService;
        _modelManager = modelManager;
        _updateService = updateService;
        _isInitializing = true;
        try
        {
            NavItems = BuildNavItems();
            RecordingModes = BuildRecordingModes();
            Languages = BuildLanguages();
            Themes = BuildThemes();
            UiLanguages = BuildUiLanguages();
            LoadFromSettings();
            BuildModelItems();
            RefreshMicrophones();
        }
        finally
        {
            _isInitializing = false;
        }

        Loc.Instance.PropertyChanged += OnLocLanguageChanged;
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

    /// <summary>
    /// Язык интерфейса сменился. Списки (<see cref="NavItems"/>, выпадающие списки,
    /// карточки моделей) обновляются сами — их локализуемые свойства подписаны на
    /// <see cref="Loc"/> и поднимают <c>PropertyChanged</c>. Здесь обновляем только
    /// разовые/вычисляемые строки, не зависящие от INPC-свойств элементов.
    /// </summary>
    private void OnLocLanguageChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        RecordHotkeyHint = RecordHotkey;
        CancelHotkeyHint = CancelHotkey;
        RefreshMicrophoneStatus();
        ErrorMessage = string.Empty;
    }

    [RelayCommand]
    private void RefreshMicrophones()
    {
        var previousId = SelectedMicrophone?.Id;
        var devices = _microphoneService.GetMicrophones();
        Microphones = devices;
        RefreshMicrophoneStatus();

        if (previousId is not null && devices.Any(m => m.Id == previousId))
        {
            SelectedMicrophone = devices.First(m => m.Id == previousId);
        }
        else
        {
            SelectedMicrophone = devices.FirstOrDefault();
        }
    }

    private void RefreshMicrophoneStatus() =>
        MicrophoneStatus = Microphones.Count == 0
            ? Loc.T("Mic_NoneFound")
            : Loc.Format("Mic_DevicesFound", Microphones.Count);

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
        var eta = p.Remaining is { } r ? Loc.Format("Models_RemainingEta", FormatSeconds(r)) : string.Empty;
        DownloadInfo = Loc.Format("Models_DownloadInfoFmt", name, downloaded, total, speed, eta);
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

    /// <summary>
    /// Проверяет наличие обновления. При <c>auto = true</c> ошибки не выводятся разрушительно
    /// (только статус), а найденное обновление дополнительно показывает трей-balloon.
    /// </summary>
    public async Task CheckForUpdatesAsync(bool auto = false)
    {
        if (IsCheckingUpdate)
        {
            return;
        }

        IsCheckingUpdate = true;
        UpdateStatus = Loc.T("Update_Checking");
        try
        {
            var result = await _updateService.CheckForUpdateAsync(AppVersion);

            if (result.IsUpToDate)
            {
                _pendingUpdate = null;
                IsUpdateAvailable = false;
                IsUpToDateVersion = true;
                UpdateStatus = Loc.T("Update_UpToDate");
            }
            else if (result.IsAvailable && result.Update is not null)
            {
                _pendingUpdate = result.Update;
                LatestVersion = result.Update.Version;
                ReleaseNotes = result.Update.ReleaseNotes ?? string.Empty;
                IsUpdateAvailable = true;
                IsUpToDateVersion = false;
                UpdateStatus = Loc.Format("Update_AvailableInfo", result.Update.Version);
                if (auto)
                {
                    UpdateAvailable?.Invoke(result.Update.Version);
                }
            }
            else
            {
                _pendingUpdate = null;
                IsUpdateAvailable = false;
                IsUpToDateVersion = false;
                UpdateStatus = auto
                    ? Loc.T("Update_CheckFailed")
                    : Loc.Format("Update_CheckFailedDetail", result.Error);
            }
        }
        catch (OperationCanceledException)
        {
            _pendingUpdate = null;
            IsUpdateAvailable = false;
            UpdateStatus = Loc.T("Update_CheckCancelled");
        }
        catch (Exception ex)
        {
            _pendingUpdate = null;
            IsUpdateAvailable = false;
            UpdateStatus = auto
                ? Loc.T("Update_CheckFailed")
                : Loc.Format("Update_CheckFailedDetail", ex.Message);
        }
        finally
        {
            IsCheckingUpdate = false;
        }
    }

    [RelayCommand]
    private Task CheckForUpdates() => CheckForUpdatesAsync(auto: false);

    [RelayCommand]
    private async Task InstallUpdate()
    {
        if (_pendingUpdate is null || !IsUpdateAvailable || IsDownloadingUpdate)
        {
            return;
        }

        IsModalDialogOpen = true;
        var confirm = System.Windows.MessageBox.Show(
            Loc.Format("Update_Confirm", LatestVersion),
            Loc.T("App_MessageBoxTitle"),
            MessageBoxButton.YesNo,
            MessageBoxImage.Question);
        IsModalDialogOpen = false;

        if (confirm != MessageBoxResult.Yes)
        {
            return;
        }

        _updateDownloadCts = new CancellationTokenSource();
        IsDownloadingUpdate = true;
        UpdateDownloadProgress = 0;
        UpdateStatus = Loc.T("Update_Downloading");
        try
        {
            var progress = new Progress<double>(p => UpdateDownloadProgress = p);
            var path = await _updateService.DownloadInstallerAsync(_pendingUpdate, progress, _updateDownloadCts.Token);

            UpdateStatus = Loc.T("Update_StartingInstall");
            UpdateInstallStarted?.Invoke(path);
        }
        catch (OperationCanceledException)
        {
            UpdateStatus = Loc.T("Update_InstallCancelled");
        }
        catch (Exception ex)
        {
            UpdateStatus = Loc.Format("Update_InstallFailed", ex.Message);
        }
        finally
        {
            _updateDownloadCts?.Dispose();
            _updateDownloadCts = null;
            IsDownloadingUpdate = false;
        }
    }

    [RelayCommand]
    private void CancelUpdateDownload() => _updateDownloadCts?.Cancel();

    private static string FormatSize(long bytes)
    {
        const double mb = 1024.0 * 1024.0;
        const double gb = mb * 1024.0;
        return bytes >= gb
            ? Loc.Format("Models_SizeGb", (bytes / gb).ToString("0.0"))
            : bytes >= mb
                ? Loc.Format("Models_SizeMb", (bytes / mb).ToString("0"))
                : Loc.Format("Models_SizeKb", (bytes / 1024.0).ToString("0"));
    }

    private static string FormatSeconds(TimeSpan t) =>
        t.TotalMinutes >= 1
            ? Loc.Format("Models_MinSecFmt", (int)t.TotalMinutes, t.Seconds)
            : Loc.Format("Models_SecFmt", Math.Max(1, (int)t.TotalSeconds));

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
            RecordHotkeyHint = Loc.T("Hotkeys_CaptureHint");
        }
        else
        {
            CancelHotkeyHint = Loc.T("Hotkeys_CaptureHint");
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
            Loc.Format("Hotkeys_ConfirmDialog", combo),
            Loc.T("App_MessageBoxTitle"),
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
        ErrorMessage = Loc.T("Hotkeys_NeedsModifier");

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
    partial void OnAppLanguageChanged(AppLanguage value) => ScheduleAutoSave();

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
        AppLanguage = s.AppLanguage;
    }

    [RelayCommand]
    private void Save()
    {
        ErrorMessage = string.Empty;

        if (!HotkeyParser.TryParse(RecordHotkey, out _))
        {
            ErrorMessage = Loc.T("Hotkeys_InvalidRecord");
            return;
        }

        if (!HotkeyParser.TryParse(CancelHotkey, out _))
        {
            ErrorMessage = Loc.T("Hotkeys_InvalidCancel");
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
            AppLanguage = AppLanguage,
        };

        var errors = _hotkeyService.ApplySettings(settings);
        _settingsService.Save(settings);
        StartupManager.SetRunAtStartup(settings.StartWithWindows);

        if (errors.Count > 0)
        {
            ErrorMessage = string.Join(" ", errors);
        }

        StatusText = Loc.T("Status_Saved");
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
        AppLanguage = defaults.AppLanguage;
        ErrorMessage = string.Empty;
    }
}
