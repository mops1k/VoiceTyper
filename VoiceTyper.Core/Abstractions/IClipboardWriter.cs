namespace VoiceTyper.Core.Abstractions;

/// <summary>Запись текста в системный буфер обмена. Реализация живёт в слое приложения (WPF Clipboard + STA).</summary>
public interface IClipboardWriter
{
    Task SetTextAsync(string text, CancellationToken ct = default);
}

/// <summary>Симуляция клавиатуры (Ctrl+V) в активное окно. Реализация — WindowsInput (SendInput).</summary>
public interface IPasteSimulator
{
    void Paste();
}
