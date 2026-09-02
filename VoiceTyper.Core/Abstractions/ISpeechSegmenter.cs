namespace VoiceTyper.Core.Abstractions;

/// <summary>Детектор речевых сегментов (Silero VAD). Сэмплы ожидаются 16 кГц моно.</summary>
public interface ISpeechSegmenter : IDisposable
{
    /// <summary>
    /// Подаёт фрагмент сэмплов и возвращает речевые сегменты этого фрагмента
    /// без сброса внутреннего состояния (для непрерывного потока).
    /// </summary>
    IReadOnlyList<(TimeSpan Start, TimeSpan End)> DetectSpeechNoReset(float[] samples);

    /// <summary>Сбрасывает внутреннее состояние VAD.</summary>
    void ResetState();
}
