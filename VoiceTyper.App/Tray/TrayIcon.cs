using System.Drawing;
using System.Windows.Forms;
using System.IO;
using VoiceTyper.Core.Localization;

namespace VoiceTyper.App.Tray;

/// <summary>Иконка в системном трее с контекстным меню.</summary>
public sealed class TrayIcon : IDisposable
{
    private readonly NotifyIcon _notifyIcon;
    private readonly Icon _icon;
    private readonly ToolStripMenuItem _openSettingsItem;
    private readonly ToolStripMenuItem _recordItem;
    private readonly ToolStripMenuItem _quitItem;
    private bool _disposed;
    private bool _tooltipIsReady;

    public event Action? OpenSettingsRequested;
    public event Action? RecordRequested;
    public event Action? QuitRequested;

    public TrayIcon()
    {
        var assetDir = Path.Combine(AppContext.BaseDirectory, "Assets");
        var iconPath = Path.Combine(assetDir, "voiceTyper.ico");

        _icon = File.Exists(iconPath) ? new Icon(iconPath) : SystemIcons.Application;

        var menu = new ContextMenuStrip();
        _openSettingsItem = new ToolStripMenuItem(Loc.T("App_TrayOpenSettings"));
        _openSettingsItem.Click += (_, _) => OpenSettingsRequested?.Invoke();
        menu.Items.Add(_openSettingsItem);

        _recordItem = new ToolStripMenuItem(Loc.T("App_TrayRecord"));
        _recordItem.Click += (_, _) => RecordRequested?.Invoke();
        menu.Items.Add(_recordItem);

        menu.Items.Add(new ToolStripSeparator());

        _quitItem = new ToolStripMenuItem(Loc.T("App_TrayQuit"));
        _quitItem.Click += (_, _) => QuitRequested?.Invoke();
        menu.Items.Add(_quitItem);

        _notifyIcon = new NotifyIcon
        {
            Icon = _icon,
            Text = Loc.T("App_TrayTooltip"),
            ContextMenuStrip = menu,
            Visible = true,
        };
        _notifyIcon.DoubleClick += (_, _) => OpenSettingsRequested?.Invoke();
    }

    /// <summary>Иконка единая (не зависит от темы системы).</summary>
    public void ApplyTheme(bool systemDark)
    {
        _ = systemDark;
        _notifyIcon.Icon = _icon;
    }

    /// <summary>Перезаписывает тексты пунктов меню и подсказку текущим языком.</summary>
    public void ApplyLanguage()
    {
        _openSettingsItem.Text = Loc.T("App_TrayOpenSettings");
        _recordItem.Text = Loc.T("App_TrayRecord");
        _quitItem.Text = Loc.T("App_TrayQuit");
        SetTooltip(_tooltipIsReady ? Loc.T("App_TrayTooltipReady") : Loc.T("App_TrayTooltip"));
    }

    public void SetRecording(bool recording)
    {
        // Индикатор записи отображается оверлеем; иконка в трее — по теме системы.
        _ = recording;
    }

    public void SetTooltip(string text)
    {
        _tooltipIsReady = text == Loc.T("App_TrayTooltipReady");
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
        _icon.Dispose();
    }
}
