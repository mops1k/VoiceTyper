using System.Drawing;
using System.Windows.Forms;
using System.IO;

namespace VoiceTyper.App.Tray;

/// <summary>Иконка в системном трее с контекстным меню.</summary>
public sealed class TrayIcon : IDisposable
{
    private readonly NotifyIcon _notifyIcon;
    private readonly Icon _darkIcon;
    private readonly Icon _lightIcon;
    private bool _disposed;

    public event Action? OpenSettingsRequested;
    public event Action? RecordRequested;
    public event Action? QuitRequested;

    public TrayIcon()
    {
        var assetDir = Path.Combine(AppContext.BaseDirectory, "Assets");
        var darkPath = Path.Combine(assetDir, "voiceTyper_dark.ico");
        var lightPath = Path.Combine(assetDir, "voiceTyper_light.ico");

        _darkIcon = File.Exists(darkPath) ? new Icon(darkPath) : SystemIcons.Application;
        _lightIcon = File.Exists(lightPath) ? new Icon(lightPath) : SystemIcons.Application;

        var menu = new ContextMenuStrip();
        menu.Items.Add("Открыть настройки", null, (_, _) => OpenSettingsRequested?.Invoke());
        menu.Items.Add("Запись", null, (_, _) => RecordRequested?.Invoke());
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Выход", null, (_, _) => QuitRequested?.Invoke());

        _notifyIcon = new NotifyIcon
        {
            Icon = _darkIcon,
            Text = "VoiceTyper",
            ContextMenuStrip = menu,
            Visible = true,
        };
        _notifyIcon.DoubleClick += (_, _) => OpenSettingsRequested?.Invoke();
    }

    /// <summary>
    /// Применяет вариант иконки по теме системы: тёмная система → светлая иконка,
    /// светлая система → тёмная (контраст с панелью задач).
    /// </summary>
    public void ApplyTheme(bool systemDark)
    {
        _notifyIcon.Icon = systemDark ? _lightIcon : _darkIcon;
    }

    public void SetRecording(bool recording)
    {
        // Индикатор записи отображается оверлеем; иконка в трее — по теме системы.
        _ = recording;
    }

    public void SetTooltip(string text)
    {
        _notifyIcon.Text = text;
    }

    public void ShowBalloon(string title, string message)
    {
        _notifyIcon.ShowBalloonTip(3000, title, message, ToolTipIcon.Info);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _notifyIcon.Visible = false;
        _notifyIcon.Dispose();
        _darkIcon.Dispose();
        _lightIcon.Dispose();
    }
}
