namespace VoiceTyper.Core.Abstractions;

/// <summary>Уровень лога.</summary>
public enum LogLevel
{
    Info,
    Warn,
    Error,
}

/// <summary>Простой логгер с записью в файл.</summary>
public interface IAppLogger
{
    /// <summary>Каталог с логами.</summary>
    string LogDirectory { get; }

    /// <summary>Полный путь к файлу лога.</summary>
    string LogFilePath { get; }

    /// <summary>Очищает текущий файл лога (при старте приложения).</summary>
    void Clear();

    void Info(string message);
    void Warn(string message);
    void Error(string message, Exception? exception = null);
}
