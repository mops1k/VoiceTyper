using Whisper.net;
using VoiceTyper.Core.Abstractions;

namespace VoiceTyper.Core.Audio;

/// <summary>
/// Обёртка над Silero VAD из Whisper.net. Отвечает за детекцию речевых сегментов
/// в потоке записываемого аудио. Модель VAD загружается лениво при первом использовании.
/// </summary>
public sealed class SileroSpeechSegmenter : ISpeechSegmenter
{
    private readonly string _vadModelPath;
    private readonly int _threads;
    private WhisperVadFactory? _factory;
    private WhisperVadProcessor? _processor;

    public SileroSpeechSegmenter(string vadModelPath)
    {
        _vadModelPath = vadModelPath;
        _threads = Math.Clamp(Environment.ProcessorCount / 2, 2, 8);
    }

    public IReadOnlyList<(TimeSpan Start, TimeSpan End)> DetectSpeechNoReset(float[] samples)
    {
        var processor = GetProcessor();
        return processor.DetectSpeechNoReset(samples)
            .Select(s => (s.Start, s.End))
            .ToArray();
    }

    public void ResetState()
    {
        _processor?.ResetState();
    }

    public void Dispose()
    {
        _processor?.Dispose();
        _factory?.Dispose();
    }

    private WhisperVadProcessor GetProcessor()
    {
        _factory ??= WhisperVadFactory.FromPath(_vadModelPath);
        _processor ??= _factory.CreateBuilder()
            .WithThreads(_threads)
            .WithThreshold(0.5f)
            .WithMinSpeechDuration(TimeSpan.FromMilliseconds(250))
            .Build();
        return _processor;
    }
}
