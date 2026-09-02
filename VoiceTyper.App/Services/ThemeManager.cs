using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using Microsoft.Win32;
using VoiceTyper.Core.Models;

namespace VoiceTyper.App.Services;

/// <summary>
/// Управление темой приложения. Меняет словарь цветов (тёмный/светлый),
/// синхронизирует заголовок окна Windows (immersive dark/light) и умеет
/// следовать за системной темой (авто) с живым обновлением.
/// </summary>
public static class ThemeManager
{
    private const string PersonalizeKey =
        @"HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize";

    private const int WmSettingChange = 0x001A;
    private const int DwmwaUseImmersiveDarkMode = 20;

    private static AppTheme _currentTheme = AppTheme.System;
    private static bool _isDark = true;

    public static bool IsDark => _isDark;

    /// <summary>Тема системы тёмная (по реестру Windows).</summary>
    public static bool IsSystemDark => !SystemUsesLightTheme();

    /// <summary>Возникает после применения темы (в т.ч. при смене системной темы).</summary>
    public static event Action? ThemeApplied;

    /// <summary>Применить тему: подменяет палитру и перекрашивает заголовки окон.</summary>
    public static void Apply(AppTheme theme)
    {
        _currentTheme = theme;
        _isDark = ResolveIsDark(theme);

        var app = System.Windows.Application.Current;
        if (app is null)
        {
            return;
        }

        var dicts = app.Resources.MergedDictionaries;
        for (var i = dicts.Count - 1; i >= 0; i--)
        {
            var source = dicts[i].Source?.OriginalString ?? string.Empty;
            if (source.EndsWith("Dark.xaml") || source.EndsWith("Light.xaml"))
            {
                dicts.RemoveAt(i);
            }
        }

        var uri = _isDark ? "Themes/Dark.xaml" : "Themes/Light.xaml";
        dicts.Add(new ResourceDictionary { Source = new Uri(uri, UriKind.Relative) });

        ApplyToAllWindows();
        ThemeApplied?.Invoke();
    }

    public static bool ResolveIsDark(AppTheme theme) => theme switch
    {
        AppTheme.Light => false,
        AppTheme.Dark => true,
        _ => SystemUsesLightTheme() is false,
    };

    /// <summary>Перекрасить заголовок окна по текущей теме.</summary>
    public static void ApplyTitleBar(Window window)
    {
        if (window is null)
        {
            return;
        }

        var hwnd = new WindowInteropHelper(window).Handle;
        if (hwnd == IntPtr.Zero)
        {
            return;
        }

        var dark = _isDark ? 1 : 0;
        _ = DwmSetWindowAttribute(hwnd, DwmwaUseImmersiveDarkMode, ref dark, sizeof(int));
    }

    /// <summary>Подписаться на смену системной темы (для режима «Авто»).</summary>
    public static void AttachLiveUpdates(Window window)
    {
        if (window is null)
        {
            return;
        }

        var hwnd = new WindowInteropHelper(window).Handle;
        var source = HwndSource.FromHwnd(hwnd);
        source?.RemoveHook(WndProc);
        source?.AddHook(WndProc);
    }

    private static void ApplyToAllWindows()
    {
        foreach (Window window in System.Windows.Application.Current.Windows)
        {
            ApplyTitleBar(window);
        }
    }

    private static IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WmSettingChange)
        {
            // Системная тема изменилась — обновляем, только если выбран режим «Авто».
            if (_currentTheme == AppTheme.System)
            {
                Apply(AppTheme.System);
                handled = true;
            }
        }

        return IntPtr.Zero;
    }

    private static bool SystemUsesLightTheme()
    {
        try
        {
            var value = Registry.GetValue(PersonalizeKey, "AppsUseLightTheme", 1);
            return Convert.ToInt32(value) == 1;
        }
        catch
        {
            return false;
        }
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
}
