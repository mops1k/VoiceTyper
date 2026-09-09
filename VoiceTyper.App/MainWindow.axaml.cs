using System;
using System.ComponentModel;
using System.IO;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;
using VoiceTyper.App.Services;
using VoiceTyper.App.ViewModels;
using VoiceTyper.Core.Localization;
using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;

namespace VoiceTyper.App;

/// <summary>Главное окно (страница настроек).</summary>
public partial class MainWindow : Window
{
    private const int HideAfterMs = 300;

    public MainWindow()
    {
        InitializeComponent();
        Loc.Instance.PropertyChanged += OnLocChanged;
        Closed += (_, _) => Loc.Instance.PropertyChanged -= OnLocChanged;
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
        (DataContext as SettingsViewModel)?.CaptureRecordHotkeyCommand.Execute(null);
    }

    private void CaptureCancelHotkey_Click(object? sender, TappedEventArgs e)
    {
        Focus();
        (DataContext as SettingsViewModel)?.CaptureCancelHotkeyCommand.Execute(null);
    }

    private void Window_PreviewKeyDown(object? sender, KeyEventArgs e)
    {
        var vm = DataContext as SettingsViewModel;
        if (vm?.IsCapturing != true)
        {
            return;
        }

        var key = e.Key;

        if (key == Key.Escape)
        {
            e.Handled = true;
            vm.CancelCapture();
            return;
        }

        if (IsModifierKey(key))
        {
            // Сам модификатор ещё не является комбинацией — ждём «главную» клавишу.
            return;
        }

        e.Handled = true;

        // Windows резервирует клавишу Win и не всегда отдаёт её в e.KeyModifiers,
        // поэтому модификаторы читаем по физическому состоянию клавиш.
        var mods = ReadPhysicalModifiers();
        var isFunctionKey = key is >= Key.F1 and <= Key.F24;

        if (mods == HotkeyModifiers.None && !isFunctionKey)
        {
            vm.NotifyHotkeyNeedsModifier();
            return;
        }

        var keyName = NormalizeKeyName(key);
        var combo = HotkeyParser.Format(new HotkeyGesture(mods, keyName));
        _ = vm.SubmitCapturedHotkey(combo);
    }

    /// <summary>Имя клавиши в формате HotkeyParser (например D1, Space, F12).</summary>
    private static string NormalizeKeyName(Key key)
    {
        var name = key.ToString()!;
        return name;
    }

    /// <summary>Модификаторы по физическому состоянию клавиш (учитывает Win).</summary>
    private HotkeyModifiers ReadPhysicalModifiers()
    {
        var mods = HotkeyModifiers.None;
        if (IsDown(0x11) || IsDown(0xA2) || IsDown(0xA3)) // Ctrl
        {
            mods |= HotkeyModifiers.Control;
        }

        if (IsDown(0x12) || IsDown(0xA4) || IsDown(0xA5)) // Alt
        {
            mods |= HotkeyModifiers.Alt;
        }

        if (IsDown(0x10) || IsDown(0xA0) || IsDown(0xA1)) // Shift
        {
            mods |= HotkeyModifiers.Shift;
        }

        if (IsDown(0x5B) || IsDown(0x5C)) // LWin / RWin
        {
            mods |= HotkeyModifiers.Win;
        }

        return mods;
    }

    private static bool IsDown(int vk) => (GetAsyncKeyState(vk) & 0x8000) != 0;

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);

    private void Window_Deactivated(object? sender, EventArgs e)
    {
        // Не сбрасываем захват: пользователь нажимает Win (открывается «Пуск»),
        // окно деактивируется — но захват должен продолжаться, чтобы считать Win+Space.
        var vm = DataContext as SettingsViewModel;
        if (vm?.IsCapturing != true)
        {
            vm?.CancelGamepadCapture();
        }
    }

    private static bool IsModifierKey(Key key) =>
        key is Key.LeftCtrl or Key.RightCtrl
            or Key.LeftAlt or Key.RightAlt
            or Key.LeftShift or Key.RightShift
            or Key.LWin or Key.RWin;

    private void LogSection_Loaded(object? sender, RoutedEventArgs e) =>
        (DataContext as SettingsViewModel)?.StartLogTimer();

    private void LogSection_Unloaded(object? sender, RoutedEventArgs e) =>
        (DataContext as SettingsViewModel)?.StopLogTimer();
}
