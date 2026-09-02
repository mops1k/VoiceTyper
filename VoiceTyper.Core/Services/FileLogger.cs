using VoiceTyper.Core.Abstractions;

namespace VoiceTyper.Core.Services;

/// <summary>
/// Пишет читаемые логи в <c>%LOCALAPPDATA%\VoiceTyper\logs\voiceTyper.log</c>.
/// Формат строки: <c>yyyy-MM-dd HH:mm:ss.fff [LEVEL] сообщение</c>, для ошибок — дополнительно стек-трейс.
/// При достижении ~1 МБ файл ротируется (voiceTyper.N.log, хранится до 5 архивов).
/// </summary>
public sealed class FileLogger : IAppLogger
{
    private const long MaxSizeBytes = 1_000_000;
    private const int MaxArchives = 5;

    private static readonly object Sync = new();

    private readonly string _directory;

    public FileLogger(string? directory = null)
    {
        _directory = directory
                     ?? Path.Combine(
                         Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                         "VoiceTyper",
                         "logs");
    }

    public string LogDirectory => _directory;

    public string LogFilePath => Path.Combine(_directory, "voiceTyper.log");

    public void Clear()
    {
        try
        {
            lock (Sync)
            {
                if (File.Exists(LogFilePath))
                {
                    File.WriteAllText(LogFilePath, string.Empty);
                }
            }
        }
        catch
        {
            // Логирование никогда не должно ронять приложение.
        }
    }

    public void Info(string message) => Write(LogLevel.Info, message, null);

    public void Warn(string message) => Write(LogLevel.Warn, message, null);

    public void Error(string message, Exception? exception = null) => Write(LogLevel.Error, message, exception);

    private void Write(LogLevel level, string message, Exception? exception)
    {
        try
        {
            lock (Sync)
            {
                Directory.CreateDirectory(_directory);
                RotateIfNeeded();

                var line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} [{level}] {message}";
                if (exception is not null)
                {
                    line += Environment.NewLine + exception;
                }

                File.AppendAllText(LogFilePath, line + Environment.NewLine);
            }
        }
        catch
        {
            // Логирование никогда не должно ронять приложение.
        }
    }

    private void RotateIfNeeded()
    {
        var file = new FileInfo(LogFilePath);
        if (!file.Exists || file.Length < MaxSizeBytes)
        {
            return;
        }

        for (var i = MaxArchives - 1; i >= 1; i--)
        {
            var src = Path.Combine(_directory, $"voiceTyper.{i}.log");
            var dst = Path.Combine(_directory, $"voiceTyper.{i + 1}.log");
            if (File.Exists(src))
            {
                File.Move(src, dst, overwrite: true);
            }
        }

        File.Move(LogFilePath, Path.Combine(_directory, "voiceTyper.1.log"), overwrite: true);
    }
}
