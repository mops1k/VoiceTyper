using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Styling;
using Microsoft.Win32;
using VoiceTyper.Core.Models;

namespace VoiceTyper.App.Services;

/// <summary>
/// Управление темой приложения. Переключает палитру цветов (тёмный/светлый),
/// вариант FluentTheme и умеет следовать за системной темой Windows.
/// </summary>
public static class ThemeManager
{
    private const string PersonalizeKey =
        @"HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize";

    private static AppTheme _currentTheme = AppTheme.System;
    private static bool _isDark = true;
    private static ResourceDictionary? _palette;

    public static bool IsDark => _isDark;

    /// <summary>Тема системы тёмная (по реестру Windows).</summary>
    public static bool IsSystemDark => !SystemUsesLightTheme();

    /// <summary>Возникает после применения темы.</summary>
    public static event Action? ThemeApplied;

    /// <summary>Применить тему: переключает FluentTheme-вариант и словарь палитры.</summary>
    public static void Apply(AppTheme theme)
    {
        _currentTheme = theme;
        _isDark = ResolveIsDark(theme);

        var app = Application.Current;
        if (app is not null)
        {
            app.RequestedThemeVariant = _isDark ? ThemeVariant.Dark : ThemeVariant.Light;
            SwapPalette(app);
        }

        ThemeApplied?.Invoke();
    }

    public static bool ResolveIsDark(AppTheme theme) => theme switch
    {
        AppTheme.Light => false,
        AppTheme.Dark => true,
        _ => SystemUsesLightTheme() is false,
    };

    private static void SwapPalette(Application app)
    {
        var merged = app.Resources.MergedDictionaries;
        if (_palette is not null && merged.Contains(_palette))
        {
            merged.Remove(_palette);
        }

        _palette = BuildPalette(_isDark);
        merged.Add(_palette);
    }

    /// <summary>Строит словарь палитры с теми же ключами, что использовал WPF-интерфейс.</summary>
    private static ResourceDictionary BuildPalette(bool dark)
    {
        var palette = new ResourceDictionary
        {
            ["Bg.Window"] = new SolidColorBrush(Color.Parse(dark ? "#FF1E1E1E" : "#FFEDEDED")),
            ["Bg.Control"] = new SolidColorBrush(Color.Parse(dark ? "#FF2D2D2D" : "#FFFFFFFF")),
            ["Bg.ControlHover"] = new SolidColorBrush(Color.Parse(dark ? "#FF3A3A3A" : "#FFE2E2E2")),
            ["Fg.Text"] = new SolidColorBrush(Color.Parse(dark ? "#FFE6E6E6" : "#FF1B1B1B")),
            ["Fg.Muted"] = new SolidColorBrush(Color.Parse(dark ? "#FF9A9A9A" : "#FF616161")),
            ["Border"] = new SolidColorBrush(Color.Parse(dark ? "#FF555555" : "#FFD6D6D6")),
            ["Accent"] = new SolidColorBrush(Color.Parse(dark ? "#FF4C8BF5" : "#FF3B82F6")),
        };
        return palette;
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
}
