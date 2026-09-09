using VoiceTyper.Core.Models;

namespace VoiceTyper.App.Services;

/// <summary>Геймпад-сервис (захват кнопки и применение привязок). UI-поток не требуется.</summary>
public interface IGamepadInputService
{
    /// <summary>Применяет привязки кнопок записи/отмены из настроек.</summary>
    void ApplySettings(AppSettings settings);

    /// <summary>Начинает захват: ожидает нажатия кнопки геймпада. Возвращает привязку.</summary>
    Task<GamepadBinding?> StartCapture();

    /// <summary>Отменяет активный захват кнопки.</summary>
    void CancelCapture();
}
