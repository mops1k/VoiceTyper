using VoiceTyper.Core.Abstractions;
using VoiceTyper.Core.Interop;
using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Services.Transcription;

/// <summary>Заведение движков распознавания (стратегия): выбор между Whisper и Parakeet.</summary>
public interface IEngineManager
{
    /// <summary>Доступен ли движок в текущей сборке (нативная библиотека рядом с приложением).</summary>
    bool IsAvailable(TranscriptionEngine engine);

    /// <summary>Создает движок нужного типа по известному пути модели.</summary>
    ITranscriptionEngine Create(TranscriptionEngine engine, string modelPath);

    /// <summary>Версия ABI нативной библиотеки Parakeet (null — библиотека не загрузилась).</summary>
    int? ParakeetAbiVersion { get; }
}

public sealed class EngineManager : IEngineManager
{
    private readonly IAppLogger? _logger;

    public EngineManager(IAppLogger? logger = null)
    {
        _logger = logger;
    }

    public int? ParakeetAbiVersion { get; private set; }

    public bool IsAvailable(TranscriptionEngine engine) => engine switch
    {
        TranscriptionEngine.Whisper => true, // Whisper.net — managed-библиотека.
        TranscriptionEngine.Parakeet => File.Exists(
            Path.Combine(AppContext.BaseDirectory, "parakeet.dll")),
        _ => throw new ArgumentOutOfRangeException(nameof(engine), engine, null),
    };

    public ITranscriptionEngine Create(TranscriptionEngine engine, string modelPath)
    {
        switch (engine)
        {
            case TranscriptionEngine.Whisper:
                return new WhisperEngine(modelPath);
            case TranscriptionEngine.Parakeet:
                if (!IsAvailable(engine))
                {
                    throw new DllNotFoundException("parakeet.dll не найдена рядом с приложением");
                }

                try
                {
                    ParakeetAbiVersion = ParakeetNative.AbiVersionNumber();
                    _logger?.Info($"[Parakeet] ABI версии {ParakeetAbiVersion}");
                }
                catch (DllNotFoundException)
                {
                    _logger?.Warn("[Parakeet] parakeet.dll не загрузилась при запросе ABI");
                }

                return new ParakeetEngine(modelPath);
            default:
                throw new ArgumentOutOfRangeException(nameof(engine), engine, null);
        }
    }
}
