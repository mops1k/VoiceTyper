using System.Diagnostics;
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

    /// <summary>Удаляет устаревшие (fp16) файлы моделей, ставшие ненужными после перехода на q5-квантизацию.</summary>
    void CleanupLegacyModels();
}

/// <summary>
/// Хранит модели в <c>%LOCALAPPDATA%\VoiceTyper\models</c>.
/// Скачивание идёт напрямую с HuggingFace по фиксированным URL (черновик официального
/// <c>download-ggml-model.sh</c> — квантованные Q5_0/Q5_1-варианты), файл пишется
/// во временный файл и атомарно переименовывается по завершении.
/// </summary>
public sealed class ModelManager : IModelManager
{
    public const string DefaultModelsDirectory = "models";
    private const string VadModelFileName = "ggml-silero-v6.2.0.bin";

    /// <summary>Базовый URL ggml-моделей Whisper (репозиторий ggerganov/whisper.cpp).</summary>
    private const string ModelBaseUrl = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";

    /// <summary>Базовый URL модели Silero VAD (репозиторий ggml-org/whisper-vad).</summary>
    private const string VadBaseUrl = "https://huggingface.co/ggml-org/whisper-vad/resolve/main/";

    private readonly string _modelsDirectory;
    private readonly HttpClient _http;

    public ModelManager(string? modelsDirectory = null, HttpClient? http = null)
    {
        _modelsDirectory = modelsDirectory
                           ?? Path.Combine(
                               Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                               "VoiceTyper",
                               DefaultModelsDirectory);
        _http = http ?? CreateHttpClient();
    }

    private static HttpClient CreateHttpClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromMinutes(30) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("VoiceTyper/1.0");
        return client;
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

    public void CleanupLegacyModels()
    {
        Directory.CreateDirectory(_modelsDirectory);
        foreach (var legacyName in LegacyModelFileNames)
        {
            var path = Path.Combine(_modelsDirectory, legacyName);
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
    }

    public async Task<string> EnsureModelAsync(ModelSize size, IProgress<ModelDownloadProgress>? progress = null, CancellationToken ct = default)
    {
        Directory.CreateDirectory(_modelsDirectory);
        var path = Path.Combine(_modelsDirectory, GetModelFileName(size));
        if (File.Exists(path))
        {
            return path;
        }

        var url = ModelBaseUrl + GetModelFileName(size);
        return await DownloadAsync(url, path, GetModelApproxSize(size), progress, ct);
    }

    public async Task<string> EnsureVadModelAsync(IProgress<ModelDownloadProgress>? progress = null, CancellationToken ct = default)
    {
        Directory.CreateDirectory(_modelsDirectory);
        var path = Path.Combine(_modelsDirectory, VadModelFileName);
        if (File.Exists(path))
        {
            return path;
        }

        var url = VadBaseUrl + VadModelFileName;
        return await DownloadAsync(url, path, VadModelApproxSize, progress, ct);
    }

    private async Task<string> DownloadAsync(string url, string path, long expectedSize,
        IProgress<ModelDownloadProgress>? progress, CancellationToken ct)
    {
        var tmpPath = path + ".download";
        try
        {
            long totalBytes = expectedSize;
            using var request = new HttpRequestMessage(HttpMethod.Get, url);
            using var response = await _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct);
            response.EnsureSuccessStatusCode();
            if (response.Content.Headers.ContentLength is { } len && len > 0)
            {
                totalBytes = len;
            }

            await using var stream = await response.Content.ReadAsStreamAsync(ct);
            await CopyWithProgressAsync(stream, tmpPath, totalBytes, progress, ct);
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

    /// <summary>Точный размер квантованного файла модели (байты) — для расчёта прогресса и ETA.</summary>
    public static long GetModelApproxSize(ModelSize size) => size switch
    {
        ModelSize.Tiny => 43_537_433,
        ModelSize.Base => 81_768_585,
        ModelSize.Small => 264_464_607,
        ModelSize.Medium => 823_369_779,
        ModelSize.Large => 874_188_075,
        _ => 0,
    };

    private const long VadModelApproxSize = 885_098;

    /// <summary>Имя файла модели на диске, например <c>ggml-small-q8_0.bin</c>.</summary>
    public static string GetModelFileName(ModelSize size) => size switch
    {
        ModelSize.Tiny => "ggml-tiny-q8_0.bin",
        ModelSize.Base => "ggml-base-q8_0.bin",
        ModelSize.Small => "ggml-small-q8_0.bin",
        ModelSize.Medium => "ggml-medium-q8_0.bin",
        ModelSize.Large => "ggml-large-v3-turbo-q8_0.bin",
        _ => throw new ArgumentOutOfRangeException(nameof(size), size, null),
    };

    /// <summary>Устаревшие имена (fp16 и q5), остававшиеся от предыдущих версий до перехода на q8.</summary>
    private static readonly string[] LegacyModelFileNames =
    {
        "ggml-tiny.bin",
        "ggml-base.bin",
        "ggml-small.bin",
        "ggml-medium.bin",
        "ggml-large-v3-turbo.bin",
        "ggml-tiny-q5_1.bin",
        "ggml-base-q5_1.bin",
        "ggml-small-q5_1.bin",
        "ggml-medium-q5_0.bin",
        "ggml-large-v3-turbo-q5_0.bin",
    };

    /// <summary>Имя файла Silero VAD на диске.</summary>
    public static string GetVadModelFileName() => VadModelFileName;
}
