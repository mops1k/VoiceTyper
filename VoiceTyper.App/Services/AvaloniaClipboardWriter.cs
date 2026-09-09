using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Threading;
using VoiceTyper.Core.Abstractions;

namespace VoiceTyper.App.Services;

/// <summary>Запись текста в буфер обмена через Avalonia (главное окно). Требует UI-поток.</summary>
public sealed class AvaloniaClipboardWriter : IClipboardWriter
{
    private const int MaxRetries = 5;
    private static readonly TimeSpan RetryDelay = TimeSpan.FromMilliseconds(120);

    public async Task SetTextAsync(string text, CancellationToken ct = default)
    {
        for (var attempt = 1; attempt <= MaxRetries; attempt++)
        {
            try
            {
                await Dispatcher.UIThread.InvokeAsync(async () =>
                {
                    if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime
                        { MainWindow: { } window })
                    {
                        await window.Clipboard!.SetTextAsync(text);
                    }
                });
                return;
            }
            catch (Exception) when (attempt < MaxRetries)
            {
                await Task.Delay(RetryDelay, ct);
            }
        }
    }
}
