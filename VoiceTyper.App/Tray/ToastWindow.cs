using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace VoiceTyper.App.Tray;

/// <summary>
/// Лёгкое всплывающее уведомление (замена системного balloon, недоступного
/// через Avalonia TrayIcon). Показывается внизу справа и скрывается само.
/// </summary>
public static class ToastWindow
{
    private const double ShowSeconds = 3.5;

    public static void Show(string title, string message)
    {
        var panel = new StackPanel
        {
            Margin = new Thickness(16, 12),
            Spacing = 4,
            MaxWidth = 340,
        };
        panel.Children.Add(new TextBlock
        {
            Text = title,
            FontWeight = FontWeight.SemiBold,
            FontSize = 13,
            TextWrapping = TextWrapping.Wrap,
        });
        if (!string.IsNullOrWhiteSpace(message) && !string.Equals(title, message))
        {
            panel.Children.Add(new TextBlock
            {
                Text = message,
                FontSize = 12,
                Foreground = Brushes.Gray,
                TextWrapping = TextWrapping.Wrap,
            });
        }

        var window = new Window
        {
            Content = new Border
            {
                Background = new SolidColorBrush(Color.FromRgb(0x2D, 0x2D, 0x2D)),
                BorderBrush = new SolidColorBrush(Color.FromRgb(0x55, 0x55, 0x55)),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(10),
                Child = panel,
            },
            SystemDecorations = SystemDecorations.None,
            ShowInTaskbar = false,
            ShowActivated = false,
            Topmost = true,
            SizeToContent = SizeToContent.WidthAndHeight,
            WindowStartupLocation = WindowStartupLocation.Manual,
        };

        window.Opened += async (_, _) =>
        {
            var screen = window.Screens.ScreenFromVisual(window);
            if (screen is not null)
            {
                var wa = screen.WorkingArea;
                var scale = window.RenderScaling;
                window.Position = new PixelPoint(
                    wa.X + (int)(wa.Width - window.ClientSize.Width * scale - 16 * scale),
                    wa.Y + (int)(wa.Height - window.ClientSize.Height * scale - 16 * scale));
            }

            await Task.Delay(TimeSpan.FromSeconds(ShowSeconds));
            window.Close();
        };

        window.Show();
    }
}
