using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using VoiceTyper.App.Services;
using VoiceTyper.App.ViewModels;
using VoiceTyper.Core.Localization;
using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;
using Key = System.Windows.Input.Key;
using KeyEventArgs = System.Windows.Input.KeyEventArgs;
using ModifierKeys = System.Windows.Input.ModifierKeys;

namespace VoiceTyper.App;

/// <summary>Главное окно (страница настроек).</summary>
public partial class MainWindow : Window
{
    private const int HideAfterMs = 300;
    private DispatcherTimer? _hideTimer;

    public MainWindow(SettingsViewModel viewModel)
    {
        InitializeComponent();
        DataContext = viewModel;
        Loc.Instance.PropertyChanged += OnLocChanged;
        Closed += (_, _) => Loc.Instance.PropertyChanged -= OnLocChanged;
    }

    /// <summary>
    /// После смены культуры (языка интерфейса) WPF может не перерисовать текст
    /// до первого взаимодействия с окном. Принудительно обновляем разметку, чтобы
    /// меню и подписи отобразились сразу, а не после клика по пункту.
    /// </summary>
    private void OnLocChanged(object? sender, PropertyChangedEventArgs e)
    {
        Dispatcher.BeginInvoke(new Action(() =>
        {
            InvalidateVisual();
            UpdateLayout();
        }), DispatcherPriority.Loaded);
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        ThemeManager.ApplyTitleBar(this);
        ThemeManager.AttachLiveUpdates(this);
        ApplyThemeIcon();
        ThemeManager.ThemeApplied += ApplyThemeIcon;
    }

    /// <summary>
    /// Иконка окна (в панели задач). Берём самый крупный кадр .ico (256×256), чтобы Windows
    /// отображал иконку крупно и чётко, а не уменьшенной до мелкого кадра.
    /// </summary>
    private void ApplyThemeIcon()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "Assets", "voiceTyper.ico");
        if (!File.Exists(path))
        {
            return;
        }

        using var fs = File.OpenRead(path);
        var decoder = new IconBitmapDecoder(fs, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
        var largest = decoder.Frames.OrderByDescending(f => f.PixelWidth).FirstOrDefault();
        if (largest is not null)
        {
            Icon = largest;
        }
    }

    private void MinimizeButton_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void MaximizeButton_Click(object sender, RoutedEventArgs e) =>
        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();

    private void CaptureRecordHotkey_Click(object sender, MouseButtonEventArgs e) =>
        (DataContext as SettingsViewModel)?.CaptureRecordHotkeyCommand.Execute(null);

    private void CaptureCancelHotkey_Click(object sender, MouseButtonEventArgs e) =>
        (DataContext as SettingsViewModel)?.CaptureCancelHotkeyCommand.Execute(null);

    private void Window_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        var vm = DataContext as SettingsViewModel;
        if (vm?.IsCapturing != true)
        {
            return;
        }

        var key = e.Key == Key.System ? e.SystemKey : e.Key;

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

        var mods = ToModifiers(Keyboard.Modifiers);
        var isFunctionKey = key is >= Key.F1 and <= Key.F24;

        if (mods == HotkeyModifiers.None && !isFunctionKey)
        {
            vm.NotifyHotkeyNeedsModifier();
            return;
        }

        var combo = HotkeyParser.Format(new HotkeyGesture(mods, key.ToString()));
        vm.SubmitCapturedHotkey(combo);
    }

    private void Window_Deactivated(object? sender, EventArgs e)
    {
        // Окно потеряло фокус — сбрасываем режим захвата.
        (DataContext as SettingsViewModel)?.CancelCapture();
        ScheduleHideToTray();
    }

    private void ScheduleHideToTray()
    {
        _hideTimer?.Stop();
        _hideTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(HideAfterMs) };
        _hideTimer.Tick += (_, _) =>
        {
            _hideTimer?.Stop();
            if (ShouldHideToTray())
            {
                Hide();
            }
        };
        _hideTimer.Start();
    }

    private bool ShouldHideToTray()
    {
        var vm = DataContext as SettingsViewModel;
        if (vm?.HideOnFocusLoss != true)
        {
            return false;
        }

        if (vm.IsModalDialogOpen)
        {
            return false;
        }

        if (IsActive)
        {
            return false;
        }

        // Не прячем, если открыто другое активное окно приложения.
        if (System.Windows.Application.Current.Windows.OfType<Window>().Any(w => w != this && w.IsVisible && w.IsActive))
        {
            return false;
        }

        return true;
    }

    private static bool IsModifierKey(Key key) =>
        key is Key.LeftCtrl or Key.RightCtrl
            or Key.LeftAlt or Key.RightAlt
            or Key.LeftShift or Key.RightShift
            or Key.LWin or Key.RWin;

    private static HotkeyModifiers ToModifiers(ModifierKeys modifiers)
    {
        var result = HotkeyModifiers.None;
        if (modifiers.HasFlag(ModifierKeys.Control))
        {
            result |= HotkeyModifiers.Control;
        }

        if (modifiers.HasFlag(ModifierKeys.Alt))
        {
            result |= HotkeyModifiers.Alt;
        }

        if (modifiers.HasFlag(ModifierKeys.Shift))
        {
            result |= HotkeyModifiers.Shift;
        }

        if (modifiers.HasFlag(ModifierKeys.Windows))
        {
            result |= HotkeyModifiers.Win;
        }

        return result;
    }
}
