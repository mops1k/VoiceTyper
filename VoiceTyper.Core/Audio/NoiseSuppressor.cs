namespace VoiceTyper.Core.Audio;

/// <summary>
/// Лёгкое подавление фонового шума (без внешних моделей): ВЧ-фильтр, убирающий
/// постоянную составляющую/гул, и адаптивный подавитель, который приглушает
/// тихие участки ниже оценённого «шумового пола». Работает на float-сэмплах 16 кГц моно.
/// </summary>
public static class NoiseSuppressor
{
    /// <summary>Обрабатывает буфер на месте.</summary>
    public static void Process(float[] samples, int sampleRate = WavBuilder.TargetSampleRate)
    {
        const int frameSize = 256;
        const double noiseFloorInit = 1e-4;
        const double thresholdRatio = 3.0;
        const double dampGain = 0.35;

        double noiseFloor = noiseFloorInit;
        double gain = 1.0;
        double lpX = 0, lpY = 0;

        for (var start = 0; start < samples.Length; start += frameSize)
        {
            var n = Math.Min(frameSize, samples.Length - start);

            // ВЧ-фильтр (~80 Гц) + подсчёт энергии кадра.
            double energy = 0;
            for (var i = 0; i < n; i++)
            {
                var x = samples[start + i];
                var y = 0.94 * (lpY + x - lpX); // y[n] = a*(y[n-1] + x[n] - x[n-1])
                lpX = x;
                lpY = y;
                samples[start + i] = (float)y;
                energy += y * y;
            }

            energy /= n;

            // Адаптивный «шумовой пол»: быстро вниз, медленно вверх.
            noiseFloor = energy < noiseFloor ? energy : noiseFloor * 1.02 + energy * 0.001;

            var isSpeech = energy > noiseFloor * thresholdRatio;
            var target = isSpeech ? 1.0 : dampGain;
            gain = gain * 0.7 + target * 0.3;

            if (gain < 0.999)
            {
                for (var i = 0; i < n; i++)
                {
                    samples[start + i] *= (float)gain;
                }
            }
        }
    }
}
