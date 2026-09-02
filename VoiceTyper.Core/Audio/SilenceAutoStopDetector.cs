using VoiceTyper.Core.Abstractions;

namespace VoiceTyper.Core.Audio;

/// <summary>
/// Детектор авто-останова записи по тишине для VAD-режима.
///
/// Логика чисто временная: подаём фрагменты аудио (16 кГц моно) в <see cref="ISpeechSegmenter"/>,
/// отслеживаем время последней речи и сигнализируем об остановке, когда тишина
/// превышает порог. Если пользователь так и не начал говорить — останавливаемся
/// через <see cref="MaxIdleBeforeSpeech"/>.
/// </summary>
public sealed class SilenceAutoStopDetector
{
    private readonly ISpeechSegmenter _segmenter;
    private readonly TimeSpan _silenceThreshold;

    /// <summary>Максимальное время «тишины до первой речи», после которого запись прерывается.</summary>
    public static readonly TimeSpan MaxIdleBeforeSpeech = TimeSpan.FromSeconds(5);

    private double _chunkBaseSeconds;
    private double _lastSpeechEndAbsolute;
    private bool _speechDetected;

    public SilenceAutoStopDetector(ISpeechSegmenter segmenter, TimeSpan silenceThreshold)
    {
        _segmenter = segmenter;
        _silenceThreshold = silenceThreshold;
    }

    /// <summary>Подаёт новый фрагмент сэмплов (16 кГц моно). Возвращает <c>true</c>, если пора остановить запись.</summary>
    public bool Process(float[] samples)
    {
        if (samples.Length == 0)
        {
            return false;
        }

        var chunkDuration = samples.Length / (double)WavBuilder.TargetSampleRate;
        var segments = _segmenter.DetectSpeechNoReset(samples);

        foreach (var (_, end) in segments)
        {
            _speechDetected = true;
            var absolute = _chunkBaseSeconds + end.TotalSeconds;
            if (absolute > _lastSpeechEndAbsolute)
            {
                _lastSpeechEndAbsolute = absolute;
            }
        }

        _chunkBaseSeconds += chunkDuration;

        if (!_speechDetected)
        {
            return _chunkBaseSeconds >= MaxIdleBeforeSpeech.TotalSeconds;
        }

        return _chunkBaseSeconds - _lastSpeechEndAbsolute >= _silenceThreshold.TotalSeconds;
    }
}
