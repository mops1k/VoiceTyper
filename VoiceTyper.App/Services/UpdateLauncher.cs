using System.Diagnostics;
using System.IO;

namespace VoiceTyper.App.Services;

/// <summary>
/// Запускает установщик обновления так, чтобы пережить полное закрытие приложения:
/// генерирует рядом с установщиком batch-скрипт и запускает его в скрытом cmd.
/// Скрипт ждёт завершения мастера Inno Setup (/AutoUpdate отключает его собственный
/// [Run]-перезапуск) и затем сам запускает VoiceTyper — после успешной установки это
/// будет новая версия, после отмены/ошибки — прежняя.
/// </summary>
public static class UpdateLauncher
{
    private const string RunnerName = "run-update.cmd";

    /// <summary>Создаёт скрипт-наблюдатель и запускает его скрыто. Не блокирует вызывающий поток.</summary>
    public static void Run(string installerPath, string appPath)
    {
        if (string.IsNullOrWhiteSpace(installerPath) || !File.Exists(installerPath))
        {
            throw new FileNotFoundException("Установщик не найден.", installerPath);
        }

        if (string.IsNullOrWhiteSpace(appPath))
        {
            throw new ArgumentException("Не указан путь к приложению.", nameof(appPath));
        }

        var runnerDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "VoiceTyper",
            "updates");
        Directory.CreateDirectory(runnerDir);
        var runnerPath = Path.Combine(runnerDir, RunnerName);

        File.WriteAllText(runnerPath,
            "@echo off\r\n" +
            $"start \"\" /wait \"{installerPath}\" /AutoUpdate\r\n" +
            $"start \"\" \"{appPath}\"\r\n");

        var psi = new ProcessStartInfo("cmd.exe")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            Arguments = "/d /c \"" + runnerPath + "\"",
        };
        Process.Start(psi);
    }
}
