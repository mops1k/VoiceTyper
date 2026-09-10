using System.Runtime.InteropServices;
using VoiceTyper.Core.Audio;
using VoiceTyper.Core.Interop;
using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Services.Transcription;

/// <summary>
/// Инференс NVIDIA Parakeet (parakeet-tdt-0.6b-v3) на CPU через нативный parakeet.cpp
/// (C-API parakeet_capi_*). Контекст создается лениво (модель грузится в память один раз)
/// и кэшируется до <see cref="DisposeAsync"/>; вызовы инференса сериализуются (контекст
/// не потокобезопасен).
///
/// Параметры <c>language</c>/<c>prompt</c>/<c>temperature</c>/<c>conditionOnPreviousText</c>/
/// <c>bestOf</c> НЕ применяются: v3 авто-определяет язык (25 европейских языков, вкл. RU),
/// а у движка нет понятий промпта и пересэмплирования гипотез. Логирование ABI выполняется
/// в App при создании движка.
/// </summary>
public sealed class ParakeetEngine : ITranscriptionEngine
{
    private readonly SemaphoreSlim _gate = new(1, 1);
    private IntPtr _ctx;
    private bool _disposed;

    public ParakeetEngine(string modelPath)
    {
        ModelPath = modelPath;
    }

    public string ModelPath { get; }

    /// <summary>
    /// Принудительно загружает модель в память, не запуская инференс (прогрев при старте).
    /// </summary>
    public void Warmup()
    {
        if (_disposed || _ctx != IntPtr.Zero)
        {
            return;
        }

        _ctx = ParakeetNative.Load(ModelPath);
        if (_ctx == IntPtr.Zero)
        {
            throw new InvalidOperationException(
                "parakeet: не удалось загрузить модель (gguf-файл не прочитан)");
        }
    }

    /// <summary>
    /// Глубокий прогрев: один инференс на ~0.3 с тишины, чтобы инициализировать
    /// контекст gguf-модели и compute-пути (первый вызов заметно медленнее последующих).
    /// </summary>
    public async Task WarmupAsync(CancellationToken ct = default)
    {
        if (_ctx == IntPtr.Zero)
        {
            Warmup();
        }

        await TranscribeAsync(BuildSilentWavAsync(), RecognitionLanguage.Auto, prompt: "", ct);
    }

    /// <summary>Короткий WAV 16 кГц / моно / PCM16 из тишины (~0.3 с) для прогрева.</summary>
    private static byte[] BuildSilentWavAsync()
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
        // Контекст не потокобезопасен — все запуски инференса сериализуем (как у Whisper).
        await _gate.WaitAsync(ct);
        try
        {
            if (_ctx == IntPtr.Zero)
            {
                Warmup();
            }

            return await Task.Run(() => TranscribeCore(wavBytes), ct);
        }
        finally
        {
            _gate.Release();
        }
    }

    private string TranscribeCore(byte[] wavBytes)
    {
        // Труба: WAV (16 кГц моно PCM16) → float32 → parakeet_capi_transcribe_pcm.
        var floats = WavPcmReader.To16KHzMonoFloats(wavBytes);

        // v3 авто-определяет язык: явно язык не задаём (targetLang="" = язык модели).
        var result = ParakeetNative.TranscribePcmLang(_ctx, floats, floats.Length, 16000, decoder: 0, targetLang: "");
        if (result == IntPtr.Zero)
        {
            var err = string.IsNullOrWhiteSpace(ParakeetNative.LastErrorText(_ctx))
                ? "неизвестная ошибка"
                : ParakeetNative.LastErrorText(_ctx);
            throw new InvalidOperationException($"parakeet: {err}");
        }

        var text = Marshal.PtrToStringUTF8(result) ?? "";
        ParakeetNative.FreeString(result);
        return text.Trim();
    }

    public ValueTask DisposeAsync()
    {
        lock (this)
        {
            if (!_disposed)
            {
                if (_ctx != IntPtr.Zero)
                {
                    ParakeetNative.Free(_ctx);
                    _ctx = IntPtr.Zero;
                }

                _gate.Dispose();
                _disposed = true;
            }
        }

        return ValueTask.CompletedTask;
    }
}
