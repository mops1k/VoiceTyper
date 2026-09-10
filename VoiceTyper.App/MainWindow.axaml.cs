using System;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;
using VoiceTyper.App.Services;
using VoiceTyper.App.ViewModels;
using VoiceTyper.Core.Localization;
using VoiceTyper.Core.Services;

namespace VoiceTyper.App;

/// <summary>Главное окно (страница настроек).</summary>
public partial class MainWindow : Window
{
    private const int HideAfterMs = 300;

    private readonly HotkeyCaptureHook _captureHook = new();

    public MainWindow()
    {
        InitializeComponent();
        Loc.Instance.PropertyChanged += OnLocChanged;
        Closed += (_, _) =>
        {
            Loc.Instance.PropertyChanged -= OnLocChanged;
            _captureHook.Dispose();
        };
        PropertyChanged += OnWindowPropertyChanged;
    }

    private void OnWindowPropertyChanged(object? sender, AvaloniaPropertyChangedEventArgs e)
    {
        if (e.Property == WindowStateProperty && e.NewValue is WindowState { } state && state == WindowState.Maximized)
        {
            WindowState = WindowState.Normal;
        }
    }

    /// <summary>Устанавливает ViewModel (окно создаётся до композиции сервисов).</summary>
    public void SetViewModel(SettingsViewModel viewModel) => DataContext = viewModel;

    /// <summary>
    /// После смены культуры интерфейса текст может не перерисоваться до первого
    /// взаимодействия с окном — принудительно обновляем разметку.
    /// </summary>
    private void OnLocChanged(object? sender, PropertyChangedEventArgs e)
    {
        Dispatcher.UIThread.Post(UpdateLayout);
    }

    protected override void OnOpened(EventArgs e)
    {
        base.OnOpened(e);
        ApplyThemeIcon();
        ThemeManager.ThemeApplied += ApplyThemeIcon;
    }

    /// <summary>Иконка окна (в панели задач).</summary>
    private void ApplyThemeIcon()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "Assets", "voiceTyper.ico");
        if (!File.Exists(path))
        {
            return;
        }

        Icon = new WindowIcon(path);
    }

    private void MinimizeButton_Click(object? sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void CloseButton_Click(object? sender, RoutedEventArgs e) => Hide();

    private void CaptureRecordHotkey_Click(object? sender, TappedEventArgs e)
    {
        Focus();
        var vm = DataContext as SettingsViewModel;
        vm?.CaptureRecordHotkeyCommand.Execute(null);
        if (vm?.IsCapturing == true)
        {
            _ = CaptureHotkeyAsync(vm);
        }
    }

    private void CaptureCancelHotkey_Click(object? sender, TappedEventArgs e)
    {
        Focus();
        var vm = DataContext as SettingsViewModel;
        vm?.CaptureCancelHotkeyCommand.Execute(null);
        if (vm?.IsCapturing == true)
        {
            _ = CaptureHotkeyAsync(vm);
        }
    }

    /// <summary>
    /// Захват комбинации глобальным хуком клавиатуры. Хук не зависит от активации
    /// окна, поэтому ловит и комбинации с Win (при нажатии Win окно теряет фокус
    /// и «Пуск» перехватывает последующие клавиши).
    /// </summary>
    private async Task CaptureHotkeyAsync(SettingsViewModel vm)
    {
        var gesture = await _captureHook.CaptureAsync(() =>
            Dispatcher.UIThread.Post(() =>
            {
                if (vm.IsCapturing)
                {
                    vm.NotifyHotkeyNeedsModifier();
                }
            }));

        await Dispatcher.UIThread.InvokeAsync(async () =>
        {
            if (!vm.IsCapturing)
            {
                return;
            }

            if (gesture is null)
            {
                vm.CancelCapture();
            }
            else
            {
                await vm.SubmitCapturedHotkey(HotkeyParser.Format(gesture));
            }
        });
    }

    private void Window_Deactivated(object? sender, EventArgs e)
    {
        // Захват клавиш идёт через глобальный хук и не зависит от активации окна.
        // Отмена при деактивации нужна только для захвата кнопки геймпада.
        var vm = DataContext as SettingsViewModel;
        if (vm?.IsCapturing != true)
        {
            vm?.CancelGamepadCapture();
        }
    }

    private void LogSection_Loaded(object? sender, RoutedEventArgs e) =>
        (DataContext as SettingsViewModel)?.StartLogTimer();

    private void LogSection_Unloaded(object? sender, RoutedEventArgs e) =>
        (DataContext as SettingsViewModel)?.StopLogTimer();
}
