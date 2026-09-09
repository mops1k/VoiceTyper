using Avalonia;
using Avalonia.Controls;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using VoiceTyper.Core.Localization;

namespace VoiceTyper.App.Tray;

/// <summary>
/// Иконка в системном трее с контекстным меню на Avalonia
/// (Win32/Linux через нативный механизм Avalonia TrayIcon).
/// </summary>
public sealed class TrayIcon : IDisposable
{
    private readonly WindowIcon _icon;
    private readonly Avalonia.Controls.TrayIcon _trayIcon;
    private NativeMenuItem _openSettingsItem = null!;
    private NativeMenuItem _recordItem = null!;
    private NativeMenuItem _quitItem = null!;
    private bool _disposed;
    private bool _tooltipIsReady;

    public event Action? OpenSettingsRequested;
    public event Action? RecordRequested;
    public event Action? QuitRequested;

    public TrayIcon()
    {
        var assetDir = System.IO.Path.Combine(AppContext.BaseDirectory, "Assets");
        var iconPath = System.IO.Path.Combine(assetDir, "voiceTyper.ico");
        _icon = System.IO.File.Exists(iconPath)
            ? new WindowIcon(iconPath)
            : new WindowIcon(CreateFallbackIcon());

        var menu = new NativeMenu();
        _openSettingsItem = new NativeMenuItem(Loc.T("App_TrayOpenSettings"));
        _openSettingsItem.Click += (_, _) => OpenSettingsRequested?.Invoke();
        menu.Items.Add(_openSettingsItem);

        _recordItem = new NativeMenuItem(Loc.T("App_TrayRecord"));
        _recordItem.Click += (_, _) => RecordRequested?.Invoke();
        menu.Items.Add(_recordItem);

        menu.Items.Add(new NativeMenuItemSeparator());

        _quitItem = new NativeMenuItem(Loc.T("App_TrayQuit"));
        _quitItem.Click += (_, _) => QuitRequested?.Invoke();
        menu.Items.Add(_quitItem);

        _trayIcon = new Avalonia.Controls.TrayIcon
        {
            Icon = _icon,
            ToolTipText = Loc.T("App_TrayTooltip"),
            Menu = menu,
            IsVisible = true,
        };
        _trayIcon.Clicked += OnTrayClicked;

        AddToApplication();
    }

    private void AddToApplication()
    {
        var app = Avalonia.Application.Current;
        if (app is null)
        {
            return;
        }

        var icons = Avalonia.Controls.TrayIcon.GetIcons(app);
        if (icons is null || !icons.Contains(_trayIcon))
        {
            var target = icons ?? new Avalonia.Controls.TrayIcons();
            target.Add(_trayIcon);
            Avalonia.Controls.TrayIcon.SetIcons(app, target);
        }
    }

    private static WriteableBitmap CreateFallbackIcon()
    {
        var bmp = new WriteableBitmap(new PixelSize(1, 1), new Vector(96, 96), PixelFormat.Bgra8888, AlphaFormat.Premul);
        using var fb = bmp.Lock();
        return bmp;
    }

    private void OnTrayClicked(object? sender, EventArgs e) => OpenSettingsRequested?.Invoke();

    public void ApplyTheme(bool systemDark)
    {
        _ = systemDark;
    }

    public void ApplyLanguage()
    {
        _openSettingsItem.Header = Loc.T("App_TrayOpenSettings");
        _recordItem.Header = Loc.T("App_TrayRecord");
        _quitItem.Header = Loc.T("App_TrayQuit");
        SetTooltip(_tooltipIsReady ? Loc.T("App_TrayTooltipReady") : Loc.T("App_TrayTooltip"));
    }

    public void SetRecording(bool recording)
    {
        // Индикатор записи отображается оверлеем; иконка в трее — единая.
        _ = recording;
    }

    public void SetTooltip(string text)
    {
        _tooltipIsReady = text == Loc.T("App_TrayTooltipReady");
        _trayIcon.ToolTipText = text;
    }

    /// <summary>Всплывающее уведомление. Реализуется через локальное окно-тост (Avalonia).</summary>
    public void ShowBalloon(string title, string message)
    {
        Dispatcher.UIThread.Post(() => ToastWindow.Show(title, message));
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _trayIcon.Clicked -= OnTrayClicked;
        _trayIcon.Dispose();
    }
}
