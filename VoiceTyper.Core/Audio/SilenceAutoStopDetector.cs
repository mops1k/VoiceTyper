using VoiceTyper.Core.Abstractions;

namespace VoiceTyper.Core.Audio;

/// <summary>Причина остановки записи в VAD-режиме.</summary>
public enum VadStopReason
{
    /// <summary>Остановки нет.</summary>
    None,

    /// <summary>Речь так и не началась — сработал лимит ожидания до речи.</summary>
    NoSpeechIdle,

    /// <summary>Фраза закончилась, пауза превысила порог тишины.</summary>
    TrailingSilence,
}

/// <summary>
/// Детектор авто-останова записи по тишине для VAD-режима.
///
/// Запись останавливается только когда фраза действительно закончилась:
/// тишина считается от последнего «активного» момента речи (сегменты VAD
/// И/ИЛИ уровень сигнала выше шумового пола), а не от устаревшего сегмента —
/// Silero в потоковом режиме возвращает сегмент только после подтверждения
/// конца речи, поэтому во время непрерывной речи новых сегментов может не быть.
///
/// Если пользователь так и не начал говорить — останавливаемся через
/// <see cref="MaxIdleBeforeSpeech"/>.
/// </summary>
public sealed class SilenceAutoStopDetector
{
    private readonly ISpeechSegmenter _segmenter;
    private readonly TimeSpan _silenceThreshold;
    private readonly double _holdSeconds;

    /// <summary>Максимальное время «тишины до первой речи», после которого запись прерывается.</summary>
    public static readonly TimeSpan MaxIdleBeforeSpeech = TimeSpan.FromSeconds(5);

    // Энергетическая оценка «сейчас идёт речь» (запас от шумового пола).
    // Порог намеренно низкий (3× пола): тихая речь не должна считаться тишиной,
    // иначе запись обрывается посреди непрерывной диктовки.
    private const double NoiseFloorInit = 1e-5;
    private const double NoiseFloorMin = 1e-8;
    private const double EnergySpeechRatio = 3.0;
    private const double FloorDropMix = 0.3;   // быстрый спуск к уровню тишины
    private const double FloorRiseFactor = 1.001; // очень медленный подъём к шуму

    private double _fedSeconds;
    private double _lastSpeechEndSeconds;
    private bool _speechDetected;
    private double _noiseFloor = NoiseFloorInit;
    private int _consecutiveInactiveChunks;

    /// <summary>Суммарное время аудио, поданного в детектор (для диагностики).</summary>
    public double TotalFedSeconds => _fedSeconds;

    public SilenceAutoStopDetector(ISpeechSegmenter segmenter, TimeSpan silenceThreshold)
    {
        _segmenter = segmenter;
        _silenceThreshold = silenceThreshold;
        _holdSeconds = Math.Min(0.8, Math.Max(0.3, silenceThreshold.TotalSeconds / 2));
    }

    /// <summary>Подаёт новый фрагмент сэмплов (16 кГц моно). Возвращает <c>true</c>, если пора остановить запись.</summary>
    public bool Process(float[] samples) => Process(samples, out _);

    /// <summary>Как <see cref="Process(float[])"/>, дополнительно возвращает причину остановки.</summary>
    public bool Process(float[] samples, out VadStopReason reason)
    {
        reason = VadStopReason.None;
        if (samples.Length == 0)
        {
            return false;
        }

        var chunkDuration = samples.Length / (double)WavBuilder.TargetSampleRate;
        var chunkEnd = _fedSeconds + chunkDuration;

        var segments = _segmenter.DetectSpeechNoReset(samples);
        var speechNearEnd = false;

        foreach (var (_, end) in segments)
        {
            var endSeconds = end.TotalSeconds;

            // Границы сегмента могут быть относительны к текущему фрагменту
            // либо ко всему потоку (whisper не сбрасывает счётчик). Нормализуем
            // к абсолютному времени, чтобы не прибавлять базу дважды.
            var absEnd = endSeconds <= chunkDuration ? _fedSeconds + endSeconds : endSeconds;

            _speechDetected = true;
            if (absEnd > _lastSpeechEndSeconds)
            {
                _lastSpeechEndSeconds = absEnd;
            }

            var gapFromChunkEnd = chunkEnd - absEnd;
            if (gapFromChunkEnd >= 0 && gapFromChunkEnd <= _holdSeconds)
            {
                // Речь тянется до конца фрагмента: VAD ещё не подтвердил её конец.
                speechNearEnd = true;
            }
        }

        var energy = MeanSquare(samples);
        var energyActive = energy > _noiseFloor * EnergySpeechRatio;

        // Асимметричный шумовой пол: быстро тянем вниз к тишине, а во время «активной»
        // речи почти не поднимаем — иначе тихий голос перестаёт считаться речью и
        // запись обрывается посреди диктовки.
        if (energy < _noiseFloor)
        {
            _noiseFloor = _noiseFloor * (1 - FloorDropMix) + energy * FloorDropMix;
        }
        else if (!energyActive)
        {
            _noiseFloor = Math.Max(NoiseFloorMin, _noiseFloor * FloorRiseFactor);
        }

        var activeChunk = energyActive || speechNearEnd;
        if (energyActive)
        {
            _speechDetected = true;
            _lastSpeechEndSeconds = Math.Max(_lastSpeechEndSeconds, chunkEnd);
        }
        else if (speechNearEnd)
        {
            // VAD сообщил конец речи, но он близко к границе фрагмента —
            // считаем фразу ещё идущей, чтобы не съесть тишину раньше времени.
            _lastSpeechEndSeconds = Math.Max(_lastSpeechEndSeconds, chunkEnd);
        }

        _fedSeconds = chunkEnd;

        if (!_speechDetected)
        {
            if (_fedSeconds >= MaxIdleBeforeSpeech.TotalSeconds)
            {
                reason = VadStopReason.NoSpeechIdle;
                return true;
            }

            return false;
        }

        _consecutiveInactiveChunks = activeChunk ? 0 : _consecutiveInactiveChunks + 1;

        var silenceSeconds = _fedSeconds - _lastSpeechEndSeconds;
        if (_consecutiveInactiveChunks >= 1 && silenceSeconds >= _silenceThreshold.TotalSeconds)
        {
            reason = VadStopReason.TrailingSilence;
            return true;
        }

        return false;
    }

    private static double MeanSquare(float[] samples)
    {
        double sum = 0;
        foreach (var s in samples)
        {
            sum += (double)s * s;
        }

        return sum / samples.Length;
    }
}