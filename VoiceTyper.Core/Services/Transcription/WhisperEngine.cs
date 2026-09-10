using System.Text;
using Whisper.net;
using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Services.Transcription;

/// <summary>
/// Инференс Whisper на CPU через Whisper.net (whisper.cpp).
/// Фабрика (и загруженная модель) кэшируется — создание processor'а на каждый вызов дешёво,
/// а повторная загрузка модели (~1–3 сек) не выполняется.
/// </summary>
public sealed class WhisperEngine : ITranscriptionEngine
{
    private readonly int _threads;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private WhisperFactory? _factory;

    public WhisperEngine(string modelPath)
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

    /// <summary>
    /// Глубокий прогрев: один инференс на ~0.3 с тишины, чтобы прогрелись compute-пути
    /// whisper.cpp (первый whisper_full заметно медленнее последующих).
    /// </summary>
    public async Task WarmupAsync(CancellationToken ct = default)
    {
        if (_factory is null)
        {
            Warmup();
        }

        await TranscribeAsync(BuildSilentWav(), RecognitionLanguage.Auto, prompt: "", ct, bestOf: 1);
    }

    /// <summary>Короткий WAV 16 кГц / моно / PCM16 из тишины (~0.3 с) для прогрева.</summary>
    private static byte[] BuildSilentWav()
    {
        const int sampleRate = 16000;
        const int samples = 4800; // 0.3 с
        using var ms = new MemoryStream();
        ms.Write("RIFF"u8);
        ms.Write(BitConverter.GetBytes(36 + samples * 2));
        ms.Write("WAVE"u8);
        ms.Write("fmt "u8);
        ms.Write(BitConverter.GetBytes(16));
        ms.Write(BitConverter.GetBytes((short)1));
        ms.Write(BitConverter.GetBytes((short)1));
        ms.Write(BitConverter.GetBytes(sampleRate));
        ms.Write(BitConverter.GetBytes(sampleRate * 2));
        ms.Write(BitConverter.GetBytes((short)2));
        ms.Write(BitConverter.GetBytes((short)16));
        ms.Write("data"u8);
        ms.Write(BitConverter.GetBytes(samples * 2));
        ms.Write(new byte[samples * 2]); // тишина: все нули
        return ms.ToArray();
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
