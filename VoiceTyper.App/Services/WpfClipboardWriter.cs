using System.Windows.Threading;
using VoiceTyper.Core.Abstractions;

namespace VoiceTyper.App.Services;

/// <summary>Запись в буфер обмена через WPF Clipboard. Требует STA-поток (Dispatcher).</summary>
public sealed class WpfClipboardWriter : IClipboardWriter
{
    private const int MaxRetries = 5;
    private static readonly TimeSpan RetryDelay = TimeSpan.FromMilliseconds(120);

    private readonly Dispatcher _dispatcher;

    public WpfClipboardWriter(Dispatcher dispatcher)
    {
        _dispatcher = dispatcher;
    }

    public async Task SetTextAsync(string text, CancellationToken ct = default)
    {
        for (var attempt = 1; attempt <= MaxRetries; attempt++)
        {
            try
            {
                await _dispatcher.InvokeAsync(() => System.Windows.Clipboard.SetText(text));
                return;
            }
            catch (System.Runtime.InteropServices.COMException) when (attempt < MaxRetries)
            {
                // Буфер занят другим приложением — повторяем.
                await Task.Delay(RetryDelay, ct);
            }
        }
    }
}
