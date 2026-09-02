using VoiceTyper.Core.Abstractions;

namespace VoiceTyper.Core.Services;

/// <summary>Вывод распознанного текста: буфер обмена + опциональная автовставка.</summary>
public interface ITextOutputService
{
    /// <summary>
    /// Кладёт текст в буфер обмена и, если <paramref name="autoPaste"/> = <c>true</c>,
    /// имитирует Ctrl+V. Возвращает <c>false</c>, если текст пустой.
    /// </summary>
    Task<bool> OutputAsync(string text, bool autoPaste, CancellationToken ct = default);
}

public sealed class TextOutputService : ITextOutputService
{
    /// <summary>Задержка между записью в буфер и вставкой (буфер должен «устояться»).</summary>
    public static readonly TimeSpan PasteDelay = TimeSpan.FromMilliseconds(80);

    private readonly IClipboardWriter _clipboard;
    private readonly IPasteSimulator _paste;

    public TextOutputService(IClipboardWriter clipboard, IPasteSimulator paste)
    {
        _clipboard = clipboard;
        _paste = paste;
    }

    public async Task<bool> OutputAsync(string text, bool autoPaste, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return false;
        }

        await _clipboard.SetTextAsync(text, ct);

        if (autoPaste)
        {
            await Task.Delay(PasteDelay, ct);
            _paste.Paste();
        }

        return true;
    }
}
