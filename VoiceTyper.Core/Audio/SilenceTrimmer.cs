namespace VoiceTyper.Core.Audio;

/// <summary>
/// Вырезает ведущую и замыкающую тишину и сжимает длинные внутренние паузы.
/// Работает на float-сэмплах 16 кГц моно. Уменьшает длину аудио, которое Whisper
/// прогоняет через энкодер, — распознавание быстрее.
///
/// Почему важен адаптивный порог: на тихом микрофоне «речь» может быть по амплитуде
/// ниже фиксированного порога, и тогда фрагмент фразы ошибочно принимается за тишину
/// и отрезается. Порог вычисляется от «шумового пола» самого сигнала (нижний перцентиль
/// RMS кадров), поэтому тихая речь сохраняется.
///
/// Почему важно сжимать внутренние паузы: whisper.cpp при встрече длинной тишины
/// считает фразу завершённой и обрывает декодирование, из-за чего фрагмент после
/// паузы теряется. Заменяя длинную паузу короткой, мы «склеиваем» фразы в один
/// непрерывный отрезок и не теряем конец высказывания.
/// </summary>
public static class SilenceTrimmer
{
    /// <summary>Размер кадра для оценки энергии, 10 мс при 16 кГц.</summary>
    private const int FrameSize = 160;

    /// <summary>Запас (паддинг) по краям речи, чтобы не отрезать края слов.</summary>
    private const double MarginSeconds = 0.25;

    /// <summary>Максимальная допустимая внутренняя пауза. Более длинные — сжимаются.</summary>
    private const double MaxSilenceSeconds = 0.6;

    /// <summary>Короткая внутренняя пауза, вставляемая вместо сжатой.</summary>
    private const double GapSilenceSeconds = 0.3;

    /// <summary>Доля кадров, принимаемая за «шумовой пол» (нижний перцентиль RMS).</summary>
    private const double NoiseFloorPercentile = 0.15;

    /// <summary>Во сколько раз RMS кадра должен превышать шумовой пол, чтобы считаться речью.</summary>
    private const double EnergySpeechRatio = 3.0;

    /// <summary>Абсолютный минимальный порог — защита от полностью пустого/нулевого сигнала.</summary>
    private const float AbsoluteFloor = 1e-6f;

    /// <summary>Запасной фиксированный порог, когда адаптивный не даёт активных кадров.</summary>
    private const float FallbackThreshold = 0.005f;

    /// <summary>Вырезает ведущую/замыкающую тишину и сжимает длинные паузы. Возвращает новый массив (или пустой, если речи нет).</summary>
    public static float[] Trim(float[] samples, double marginSeconds = MarginSeconds,
        double maxSilenceSeconds = MaxSilenceSeconds, double gapSilenceSeconds = GapSilenceSeconds)
    {
        if (samples.Length < FrameSize)
        {
            return samples;
        }

        var frameCount = samples.Length / FrameSize;
        var threshold = ComputeThreshold(samples, frameCount);

        var active = new bool[frameCount];
        for (var f = 0; f < frameCount; f++)
        {
            active[f] = IsActive(samples, f * FrameSize, FrameSize, threshold);
        }

        var firstActive = Array.FindIndex(active, static a => a);
        if (firstActive < 0)
        {
            // Речи нет — нечего распознавать.
            return Array.Empty<float>();
        }

        var lastActive = Array.FindLastIndex(active, static a => a);

        var margin = (int)(marginSeconds * WavBuilder.TargetSampleRate);
        var maxSilence = (int)(maxSilenceSeconds * WavBuilder.TargetSampleRate);
        var gapSilence = (int)(gapSilenceSeconds * WavBuilder.TargetSampleRate);

        // Область, которую оставляем: от первой активной рамки (с запасом) до последней (с запасом).
        var start = Math.Max(0, firstActive * FrameSize - margin);
        var end = Math.Min(samples.Length, (lastActive + 1) * FrameSize + margin);

        var result = new List<float>(end - start);
        var i = start;

        while (i < end)
        {
            var frame = i / FrameSize;
            if (active[frame])
            {
                result.Add(samples[i]);
                i++;
                continue;
            }

            // Тихий кадр — накапливаем непрерывный диапазон тишины внутри области.
            var silenceStart = i;
            while (i < end && !active[i / FrameSize])
            {
                i++;
            }

            var silenceLen = i - silenceStart;
            // Длинную паузу сжимаем до короткой; короткую (внутрислоговую) оставляем как есть.
            var keep = silenceLen > maxSilence ? gapSilence : silenceLen;
            for (var k = 0; k < keep && silenceStart + k < end; k++)
            {
                result.Add(samples[silenceStart + k]);
            }
        }

        return result.ToArray();
    }

    private static float ComputeThreshold(float[] samples, int frameCount)
    {
        var rms = new double[frameCount];
        for (var f = 0; f < frameCount; f++)
        {
            var sum = 0d;
            for (var i = 0; i < FrameSize; i++)
            {
                var s = samples[f * FrameSize + i];
                sum += (double)s * s;
            }

            rms[f] = Math.Sqrt(sum / FrameSize);
        }

        // Нижний перцентиль RMS кадров — оценка «шумового пола».
        var sorted = (double[])rms.Clone();
        Array.Sort(sorted);
        var noiseFloor = sorted[Math.Min(sorted.Length - 1, (int)(sorted.Length * NoiseFloorPercentile))];

        // Адаптивный порог: на тихом микрофоне шумовой пол мал, порог снижается, и
        // тихая речь не отрезается. Абсолютный минимум защищает от деления на ноль.
        var adaptive = Math.Max(noiseFloor * EnergySpeechRatio, AbsoluteFloor);

        // Если адаптивный порог «задрал» порог так, что активных кадров нет (и весь
        // сигнал выглядит громким) — откатываемся к щадящему фиксированному порогу.
        // Это покрывает сплошной громкий сигнал без пауз (например, тестовый тон).
        var anyActive = false;
        for (var f = 0; f < frameCount; f++)
        {
            if (rms[f] > adaptive)
            {
                anyActive = true;
                break;
            }
        }

        return anyActive ? (float)adaptive : FallbackThreshold;
    }

    private static bool IsActive(float[] samples, int start, int count, float threshold)
    {
        double sum = 0;
        for (var i = 0; i < count; i++)
        {
            var s = samples[start + i];
            sum += (double)s * s;
        }

        var rms = Math.Sqrt(sum / count);
        return rms >= threshold;
    }
}
