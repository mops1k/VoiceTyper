using System.Text;
using Whisper.net;
using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Services;

/// <summary>Распознавание речи (speech-to-text) на базе Whisper.</summary>
public interface ITranscriptionService : IAsyncDisposable
{
    /// <summary>Путь к используемой ggml-модели.</summary>
    string ModelPath { get; }

    /// <summary>
    /// Транскрибирует WAV-файл (16 кГц / моно / 16 бит) в текст.
    /// <paramref name="bestOf"/> — число кандидатов при жадном сэмплировании (1 — самый быстрый,
    /// больше — точнее). Для финального результата можно задать 3–5, для быстрого предпросмотра — 1.
    /// </summary>
    Task<string> TranscribeAsync(byte[] wavBytes, RecognitionLanguage language, string prompt, CancellationToken ct = default,
        float temperature = 0f, bool conditionOnPreviousText = false, int bestOf = 1);

    /// <summary>Прогревает модель: принудительно грузит её в память, чтобы первая диктовка не ждала загрузку.</summary>
    void Warmup();
}

/// <summary>
/// Инференс Whisper на CPU через Whisper.net (whisper.cpp).
/// Фабрика (и загруженная модель) кэшируется — создание processor'а на каждый вызов дешёво,
/// а повторная загрузка модели (~1–3 сек) не выполняется.
/// </summary>
public sealed class WhisperTranscriptionService : ITranscriptionService
{
    private readonly int _threads;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private WhisperFactory? _factory;

    public WhisperTranscriptionService(string modelPath)
    {
        ModelPath = modelPath;
        _threads = Math.Clamp(CpuCoreInfo.GetPhysicalCoreCount(), 1, 16);
    }

    public string ModelPath { get; }

    /// <summary>
    /// Принудительно создаёт фабрику (грузит модель в память), не запуская инференс.
    /// Позволяет выполнить «прогрев» при старте приложения, чтобы первое распознавание
    /// не включало в себя задержку загрузки весов (~1–3 сек).
    /// </summary>
    public void Warmup()
    {
        _factory ??= WhisperFactory.FromPath(ModelPath);
    }

    public async Task<string> TranscribeAsync(byte[] wavBytes, RecognitionLanguage language, string prompt, CancellationToken ct = default,
        float temperature = 0f, bool conditionOnPreviousText = false, int bestOf = 1)
    {
        // whisper.cpp контекст не потокобезопасен для параллельных вызовов whisper_full(),
        // а при частичном распознавании чанк (фоновый цикл) и финализация «хвоста» могут
        // идти практически одновременно. Сериализуем все запуски инференса.
        await _gate.WaitAsync(ct);
        try
        {
            return await TranscribeCoreAsync(wavBytes, language, prompt, ct, temperature, conditionOnPreviousText, bestOf);
        }
        finally
        {
            _gate.Release();
        }
    }

    private async Task<string> TranscribeCoreAsync(byte[] wavBytes, RecognitionLanguage language, string prompt,
        CancellationToken ct, float temperature, bool conditionOnPreviousText, int bestOf)
    {
        var factory = _factory ??= WhisperFactory.FromPath(ModelPath);

        var builder = factory.CreateBuilder()
            .WithThreads(_threads)
            .WithNoSpeechThreshold(0.6f)
            .WithTemperature(temperature)
            // Greedy Search (без Beam и температурного/энтропийного фолбека). bestOf=1 —
            // самый быстрый; для финального результата можно задать больше (точнее).
            .WithGreedySamplingStrategy(g => g.WithBestOf(Math.Max(1, bestOf)))
            .WithTemperatureInc(0f)
            .WithEntropyThreshold(-1f)
            .WithLogProbThreshold(-1f);

        if (!conditionOnPreviousText)
        {
            builder = builder.WithNoContext();
        }

        switch (language)
        {
            case RecognitionLanguage.Ru:
                builder = builder.WithLanguage("ru");
                break;
            case RecognitionLanguage.En:
                builder = builder.WithLanguage("en");
                break;
            default:
                builder = builder.WithLanguageDetection();
                break;
        }

        if (!string.IsNullOrWhiteSpace(prompt))
        {
            builder = builder.WithPrompt(prompt).WithCarryInitialPrompt(true);
        }

        using var processor = builder.Build();
        using var stream = new MemoryStream(wavBytes);

        var sb = new StringBuilder();
        await foreach (var segment in processor.ProcessAsync(stream))
        {
            ct.ThrowIfCancellationRequested();
            if (!string.IsNullOrEmpty(segment.Text))
            {
                sb.Append(segment.Text);
            }
        }

        return sb.ToString().Trim();
    }

    public ValueTask DisposeAsync()
    {
        _factory?.Dispose();
        _factory = null;
        _gate.Dispose();
        return ValueTask.CompletedTask;
    }
}
