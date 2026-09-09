using VoiceTyper.Core.Models;

namespace VoiceTyper.App.Services;

/// <summary>Глобальные горячие клавиши (Record/Cancel) и отпускание клавиши.</summary>
public interface IHotkeyService
{
    /// <summary>Нажата клавиша записи (возникающий фронт).</summary>
    event Action? RecordPressed;

    /// <summary>Нажата клавиша отмены (возникающий фронт).</summary>
    event Action? CancelPressed;

    /// <summary>Виртуальный код клавиши записи (0 — не зарегистрирована/геймпад).</summary>
    int RecordKeyVk { get; }

    /// <summary>Перерегистрирует хоткеи по настройкам. Возвращает ошибки регистрации.</summary>
    IReadOnlyList<string> ApplySettings(AppSettings settings);

    /// <summary>Снимает все глобальные хоткеи.</summary>
    void UnregisterAll();
}
