using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Services.Transcription;

/// <summary>Контракт движка распознавания речи (speech-to-text).</summary>
public interface ITranscriptionEngine : IAsyncDisposable
{
    /// <summary>Путь к используемой модели.</summary>
    string ModelPath { get; }

    /// <summary>
    /// Транскрибирует WAV-файл (16 кГц / моно / 16 бит) в текст.
    /// <paramref name="bestOf"/> — число кандидатов при жадном сэмплировании (1 — самый быстрый,
    /// больше — точнее). Для финального результата можно задать 3–5.
    /// </summary>
    Task<string> TranscribeAsync(byte[] wavBytes, RecognitionLanguage language, string prompt, CancellationToken ct = default,
        float temperature = 0f, bool conditionOnPreviousText = false, int bestOf = 1);

    /// <summary>Прогревает движок: принудительно грузит модель в память, чтобы первая диктовка не ждала загрузку.</summary>
    void Warmup();

    /// <summary>
    /// Глубокий прогрев: выполняет один короткий инференс на тишине, чтобы прогрелись
    /// compute-пути движка (JIT/аллокации/кэш графов). Вызывайте один раз после старта,
    /// в фоновом порядке; первая реальная диктовка после него уже быстрая.
    /// </summary>
    Task WarmupAsync(CancellationToken ct = default);
}
