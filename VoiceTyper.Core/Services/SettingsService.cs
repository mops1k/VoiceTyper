using System.Text.Json;
using System.Text.Json.Serialization;
using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Services;

/// <summary>Сервис загрузки и сохранения настроек в JSON-файл.</summary>
public interface ISettingsService
{
    /// <summary>Полный путь к файлу настроек.</summary>
    string SettingsFilePath { get; }

    /// <summary>Загружает настройки; при отсутствии/повреждении файла возвращает значения по умолчанию.</summary>
    AppSettings Load();

    /// <summary>Сохраняет настройки атомарно (tmp-файл + переименование).</summary>
    void Save(AppSettings settings);
}

public sealed class SettingsService : ISettingsService
{
    public static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) },
    };

    private readonly string _directory;

    public SettingsService(string? baseDirectory = null)
    {
        _directory = baseDirectory
                     ?? Path.Combine(
                         Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                         "VoiceTyper");
    }

    public string SettingsFilePath => Path.Combine(_directory, "settings.json");

    public AppSettings Load()
    {
        try
        {
            if (!File.Exists(SettingsFilePath))
            {
                return new AppSettings();
            }

            var json = File.ReadAllText(SettingsFilePath);
            return JsonSerializer.Deserialize<AppSettings>(json, JsonOptions) ?? new AppSettings();
        }
        catch (JsonException)
        {
            // Повреждённый файл — не роняем приложение, отдаём дефолты.
            return new AppSettings();
        }
        catch (IOException)
        {
            return new AppSettings();
        }
        catch (UnauthorizedAccessException)
        {
            return new AppSettings();
        }
    }

    public void Save(AppSettings settings)
    {
        Directory.CreateDirectory(_directory);
        var json = JsonSerializer.Serialize(settings, JsonOptions);
        var tmpPath = SettingsFilePath + ".tmp";
        File.WriteAllText(tmpPath, json);
        File.Move(tmpPath, SettingsFilePath, overwrite: true);
    }
}
