using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Layout;
using Avalonia.Media;

namespace VoiceTyper.App.Services;

/// <summary>Реализация диалогов на Avalonia (модальные окна поверх главного).</summary>
public sealed class AvaloniaDialogService : IDialogService
{
    private Window? ResolveOwner()
    {
        if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime lifetime)
        {
            foreach (var window in lifetime.Windows)
            {
                if (window.IsActive)
                {
                    return window;
                }
            }

            return lifetime.MainWindow;
        }

        return null;
    }

    public async Task<bool> ConfirmAsync(string message, string title)
    {
        var result = await ShowDialogAsync(message, title, showCancel: true);
        return result == true;
    }

    public Task InfoAsync(string message, string title) => ShowDialogAsync(message, title, showCancel: false);

    public Task ErrorAsync(string message, string title) => ShowDialogAsync(message, title, showCancel: false);

    private async Task<bool?> ShowDialogAsync(string message, string title, bool showCancel)
    {
        var yes = CreateButton(Loc("Common_Yes"), true);
        var no = showCancel ? CreateButton(Loc("Common_No"), false) : null;

        var panel = new StackPanel
        {
            Margin = new Thickness(20),
            Spacing = 16,
            MinWidth = 360,
            MaxWidth = 480,
        };
        panel.Children.Add(new TextBlock
        {
            Text = message,
            TextWrapping = TextWrapping.Wrap,
            FontSize = 14,
            MaxWidth = 440,
        });

        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Spacing = 8,
        };
        if (no is not null)
        {
            buttons.Children.Add(no);
        }

        buttons.Children.Add(yes);
        panel.Children.Add(buttons);

        var dialog = new Window
        {
            Title = title,
            Content = panel,
            SizeToContent = SizeToContent.WidthAndHeight,
            CanResize = false,
            ShowInTaskbar = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
        };

        var owner = ResolveOwner();
        var tcs = new TaskCompletionSource<bool?>();
        yes.Click += (_, _) =>
        {
            tcs.TrySetResult(true);
            dialog.Close();
        };
        if (no is not null)
        {
            no.Click += (_, _) =>
            {
                tcs.TrySetResult(false);
                dialog.Close();
            };
        }

        dialog.Closed += (_, _) => tcs.TrySetResult(null);

        if (owner is not null)
        {
            _ = dialog.ShowDialog(owner);
        }
        else
        {
            dialog.Show();
        }

        return await tcs.Task;
    }

    private Button CreateButton(string text, bool isDefault)
    {
        var button = new Button
        {
            Content = text,
            MinWidth = 90,
            Padding = new Thickness(14, 6),
        };
        if (isDefault)
        {
            button.Classes.Add("primary");
        }

        return button;
    }

    private static string Loc(string key)
    {
        try
        {
            return VoiceTyper.Core.Localization.Loc.T(key);
        }
        catch
        {
            return key;
        }
    }
}
