namespace VoiceTyper.App.Services;

/// <summary>Пользовательские диалоги (подтверждения, сообщения) поверх UI-фреймворка.</summary>
public interface IDialogService
{
    /// <summary>Модальный вопрос Да/Нет. Возвращает true при подтверждении.</summary>
    Task<bool> ConfirmAsync(string message, string title);

    /// <summary>Информационное сообщение с одной кнопкой OK.</summary>
    Task InfoAsync(string message, string title);

    /// <summary>Сообщение об ошибке.</summary>
    Task ErrorAsync(string message, string title);
}
