using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Threading;

namespace VoiceTyper.App.Overlay;

/// <summary>
/// Маленькое безрамочное окно-индикатор по центру внизу экрана (поверх всех окон),
/// показывающее текущий статус приложения: «Захват», «Распознавание».
/// Не получает фокус и не появляется в Alt-Tab.
/// </summary>
public partial class StatusOverlayWindow : Window
{
    private DispatcherTimer? _pulseTimer;
    private bool _pulsePhase;

    public StatusOverlayWindow()
    {
        InitializeComponent();
        ShowActivated = false;
    }

    /// <summary>Показать статус. <paramref name="accentHex"/> — цвет индикатора (например "#4C8BF5").</summary>
    public void ShowStatus(string text, string accentHex)
    {
        StatusText.Text = text;
        Dot.Fill = new SolidColorBrush(Color.Parse(accentHex));

        if (!IsVisible)
        {
            Show();
        }

        Reposition();
        StartPulse();
    }

    /// <summary>Скрыть индикатор и остановить анимацию.</summary>
    public void HideStatus()
    {
        StopPulse();
        if (IsVisible)
        {
            Hide();
        }
    }

    private void Reposition()
    {
        UpdateLayout();
        var screen = Screens.ScreenFromVisual(this) ?? Screens.Primary;
        if (screen is null)
        {
            return;
        }

        var wa = screen.WorkingArea;
        var scale = RenderScaling;
        var widthPx = ClientSize.Width * scale;
        var heightPx = ClientSize.Height * scale;
        Position = new PixelPoint(
            wa.X + (int)((wa.Width - widthPx) / 2),
            wa.Y + (int)(wa.Height - heightPx - 26 * scale));
    }

    private void StartPulse()
    {
        if (_pulseTimer is not null)
        {
            return;
        }

        _pulsePhase = true;
        Dot.Opacity = 1.0;
        _pulseTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(350) };
        _pulseTimer.Tick += OnPulseTick;
        _pulseTimer.Start();
    }

    private void OnPulseTick(object? sender, EventArgs e)
    {
        _pulsePhase = !_pulsePhase;
        Dot.Opacity = _pulsePhase ? 1.0 : 0.35;
    }

    private void StopPulse()
    {
        if (_pulseTimer is null)
        {
            return;
        }

        _pulseTimer.Stop();
        _pulseTimer.Tick -= OnPulseTick;
        _pulseTimer = null;
        Dot.Opacity = 1.0;
    }

    protected override void OnClosed(EventArgs e)
    {
        StopPulse();
        base.OnClosed(e);
    }
}
