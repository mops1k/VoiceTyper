using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace VoiceTyper.App.Overlay;

/// <summary>
/// Маленькое безрамочное окно-индикатор по центру внизу экрана (поверх всех окон),
/// показывающее текущий статус приложения: «Захват», «Распознавание».
/// Прозрачно для кликов, не появляется в Alt-Tab, не перехватывает фокус.
/// </summary>
public partial class StatusOverlayWindow : Window
{
    private const int GwlExStyle = -20;
    private const int WsExTransparent = 0x00000020;
    private const int WsExNoActivate = 0x08000000;

    [DllImport("user32.dll")]
    private static extern int GetWindowLong(IntPtr hWnd, int nIndex);

    [DllImport("user32.dll")]
    private static extern int SetWindowLong(IntPtr hWnd, int nIndex, int dwNewLong);

    private DoubleAnimation? _pulse;

    public StatusOverlayWindow()
    {
        InitializeComponent();
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        var hwnd = new WindowInteropHelper(this).Handle;
        var ex = GetWindowLong(hwnd, GwlExStyle);
        SetWindowLong(hwnd, GwlExStyle, ex | WsExTransparent | WsExNoActivate);
    }

    /// <summary>Показать статус. <paramref name="accentHex"/> — цвет индикатора (например "#4C8BF5").</summary>
    public void ShowStatus(string text, string accentHex)
    {
        StatusText.Text = text;
        Dot.Fill = new SolidColorBrush((System.Windows.Media.Color)System.Windows.Media.ColorConverter.ConvertFromString(accentHex));

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
        var area = SystemParameters.WorkArea;
        Left = area.Left + (area.Width - ActualWidth) / 2;
        Top = area.Bottom - ActualHeight - 26;
    }

    private void StartPulse()
    {
        if (_pulse is not null)
        {
            return;
        }

        _pulse = new DoubleAnimation(1.0, 0.35, TimeSpan.FromMilliseconds(700))
        {
            AutoReverse = true,
            RepeatBehavior = RepeatBehavior.Forever,
        };
        Dot.BeginAnimation(OpacityProperty, _pulse);
    }

    private void StopPulse()
    {
        if (_pulse is null)
        {
            return;
        }

        Dot.BeginAnimation(OpacityProperty, null);
        Dot.Opacity = 1.0;
        _pulse = null;
    }
}
