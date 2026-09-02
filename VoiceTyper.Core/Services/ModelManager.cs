using System.Diagnostics;
using Whisper.net.Ggml;
using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Services;

/// <summary>Загрузка и кэширование ggml-моделей Whisper и Silero VAD.</summary>
public interface IModelManager
{
    string ModelsDirectory { get; }

    /// <summary>Полный путь к файлу модели указанного размера (на диске или нет).</summary>
    string GetModelPath(ModelSize size);

    /// <summary>Скачана ли модель на диск.</summary>
    bool IsModelDownloaded(ModelSize size);

    /// <summary>Удаляет файл модели с диска. Возвращает true, если файл был удалён.</summary>
    bool DeleteModel(ModelSize size);

    /// <summary>Возвращает путь к модели нужного размера, скачивая её при первом обращении.</summary>
    Task<string> EnsureModelAsync(ModelSize size, IProgress<ModelDownloadProgress>? progress = null, CancellationToken ct = default);

    /// <summary>Возвращает путь к модели Silero VAD, скачивая её при первом обращении.</summary>
    Task<string> EnsureVadModelAsync(IProgress<ModelDownloadProgress>? progress = null, CancellationToken ct = default);
}

/// <summary>
/// Хранит модели в <c>%LOCALAPPDATA%\VoiceTyper\models</c>.
/// Скачивание идёт через <see cref="WhisperGgmlDownloader"/> (HuggingFace), файл пишется
/// во временный файл и атомарно переименовывается по завершении.
/// </summary>
public sealed class ModelManager : IModelManager
{
    public const string DefaultModelsDirectory = "models";
    private const string VadModelFileName = "ggml-silero-v6.2.0.bin";

    private readonly string _modelsDirectory;
    private readonly WhisperGgmlDownloader _downloader;

    public ModelManager(string? modelsDirectory = null, WhisperGgmlDownloader? downloader = null)
    {
        _modelsDirectory = modelsDirectory
                           ?? Path.Combine(
                               Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                               "VoiceTyper",
                               DefaultModelsDirectory);
        _downloader = downloader ?? WhisperGgmlDownloader.Default;
    }

    public string ModelsDirectory => _modelsDirectory;

    public string GetModelPath(ModelSize size) => Path.Combine(_modelsDirectory, GetModelFileName(size));

    public bool IsModelDownloaded(ModelSize size) => File.Exists(GetModelPath(size));

    public bool DeleteModel(ModelSize size)
    {
        var path = GetModelPath(size);
        if (!File.Exists(path))
        {
            return false;
        }

        File.Delete(path);
        return true;
    }

    public async Task<string> EnsureModelAsync(ModelSize size, IProgress<ModelDownloadProgress>? progress = null, CancellationToken ct = default)
    {
        Directory.CreateDirectory(_modelsDirectory);
        var path = Path.Combine(_modelsDirectory, GetModelFileName(size));
        if (File.Exists(path))
        {
            return path;
        }

        var tmpPath = path + ".download";
        try
        {
            await using var stream = await _downloader.GetGgmlModelAsync(MapToGgmlType(size), cancellationToken: ct);
            await CopyWithProgressAsync(stream, tmpPath, GetModelApproxSize(size), progress, ct);
            File.Move(tmpPath, path, overwrite: true);
            return path;
        }
        catch
        {
            TryDelete(tmpPath);
            throw;
        }
    }

    public async Task<string> EnsureVadModelAsync(IProgress<ModelDownloadProgress>? progress = null, CancellationToken ct = default)
    {
        Directory.CreateDirectory(_modelsDirectory);
        var path = Path.Combine(_modelsDirectory, VadModelFileName);
        if (File.Exists(path))
        {
            return path;
        }

        var tmpPath = path + ".download";
        try
        {
            await using var stream = await _downloader.GetGgmlSileroVadModelAsync(cancellationToken: ct);
            await CopyWithProgressAsync(stream, tmpPath, VadModelApproxSize, progress, ct);
            File.Move(tmpPath, path, overwrite: true);
            return path;
        }
        catch
        {
            TryDelete(tmpPath);
            throw;
        }
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch
        {
            // Не критично — файл останется, повторное скачивание перезапишет его.
        }
    }

    private static async Task CopyWithProgressAsync(Stream source, string tmpPath, long totalBytes,
        IProgress<ModelDownloadProgress>? progress, CancellationToken ct)
    {
        var buffer = new byte[128 * 1024];
        long total = 0;
        var sw = Stopwatch.StartNew();
        var lastReport = TimeSpan.Zero;

        await using var file = File.Create(tmpPath);
        int read;
        while ((read = await source.ReadAsync(buffer, ct)) > 0)
        {
            await file.WriteAsync(buffer.AsMemory(0, read), ct);
            total += read;

            if (progress is not null && sw.Elapsed - lastReport > TimeSpan.FromMilliseconds(120))
            {
                lastReport = sw.Elapsed;
                var speed = sw.Elapsed.TotalSeconds > 0 ? total / sw.Elapsed.TotalSeconds : 0;
                progress.Report(new ModelDownloadProgress(total, totalBytes, speed));
            }
        }

        progress?.Report(new ModelDownloadProgress(total, totalBytes,
            sw.Elapsed.TotalSeconds > 0 ? total / sw.Elapsed.TotalSeconds : 0));
    }

    /// <summary>Примерный итоговый размер файла модели (байты) — для расчёта прогресса и ETA.</summary>
    internal static long GetModelApproxSize(ModelSize size) => size switch
    {
        ModelSize.Tiny => 75_000_000,
        ModelSize.Base => 142_000_000,
        ModelSize.Small => 466_000_000,
        ModelSize.Medium => 1_500_000_000,
        ModelSize.Large => 1_620_000_000,
        _ => 0,
    };

    private const long VadModelApproxSize = 2_300_000;

    /// <summary>Имя файла модели на диске, например <c>ggml-small.bin</c>.</summary>
    public static string GetModelFileName(ModelSize size) => size switch
    {
        ModelSize.Tiny => "ggml-tiny.bin",
        ModelSize.Base => "ggml-base.bin",
        ModelSize.Small => "ggml-small.bin",
        ModelSize.Medium => "ggml-medium.bin",
        ModelSize.Large => "ggml-large-v3-turbo.bin",
        _ => throw new ArgumentOutOfRangeException(nameof(size), size, null),
    };

    /// <summary>Имя файла Silero VAD на диске.</summary>
    public static string GetVadModelFileName() => VadModelFileName;

    private static GgmlType MapToGgmlType(ModelSize size) => size switch
    {
        ModelSize.Tiny => GgmlType.Tiny,
        ModelSize.Base => GgmlType.Base,
        ModelSize.Small => GgmlType.Small,
        ModelSize.Medium => GgmlType.Medium,
        // LargeV3Turbo даёт качество близкое к large при вдвое меньшем размере — практичнее для CPU.
        ModelSize.Large => GgmlType.LargeV3Turbo,
        _ => throw new ArgumentOutOfRangeException(nameof(size), size, null),
    };
}
