using VoiceTyper.Core.Audio;

namespace VoiceTyper.Tests.Audio;

public class SilenceTrimmerTests
{
    private const int SampleRate = WavBuilder.TargetSampleRate;

    [Fact]
    public void Trim_RemovesLeadingAndTrailingSilence()
    {
        // 0.5 с тишины + 1 с речи + 0.5 с тишины
        var samples = Concat(Silence(0.5), Speech(1.0), Silence(0.5));

        var trimmed = SilenceTrimmer.Trim(samples);

        // Речь 1 с + запас по краям (0.25 + 0.25) = ~1.5 с
        var expectedSeconds = 1.0 + 2 * 0.25;
        Assert.InRange(trimmed.Length / (double)SampleRate, expectedSeconds - 0.1, expectedSeconds + 0.1);
    }

    [Fact]
    public void Trim_PureSilence_ReturnsEmpty()
    {
        var samples = Silence(1.0);

        var trimmed = SilenceTrimmer.Trim(samples);

        Assert.Empty(trimmed);
    }

    [Fact]
    public void Trim_AllSpeech_KeepsLength()
    {
        var samples = Speech(2.0);

        var trimmed = SilenceTrimmer.Trim(samples);

        // Сплошная речь (все кадры активны) — длина почти не меняется.
        Assert.InRange(trimmed.Length / (double)SampleRate, 1.9, 2.0);
    }

    [Fact]
    public void Trim_ShortSignal_BelowFrameSize_ReturnsSame()
    {
        var samples = new float[100];

        var trimmed = SilenceTrimmer.Trim(samples);

        Assert.Equal(samples, trimmed);
    }

    [Fact]
    public void Trim_LongInternalPause_KeepsBothPhrasesAndCompressesGap()
    {
        // Сценарий пользователя: 5 с тишины + фраза + 5 с паузы + фраза + 5 с тишины.
        var samples = Concat(Silence(5.0), Speech(1.0), Silence(5.0), Speech(1.0), Silence(5.0));

        var trimmed = SilenceTrimmer.Trim(samples);

        var seconds = trimmed.Length / (double)SampleRate;
        // Обе фразы сохранились (1 + 1 = 2 с) + запас по краям (0.25+0.25) + короткий разрыв (~0.3 с).
        Assert.InRange(seconds, 2.4, 3.2);
    }

    [Fact]
    public void Trim_LongInternalPause_DoesNotContainTheLongPause()
    {
        var samples = Concat(Silence(0.2), Speech(1.0), Silence(5.0), Speech(1.0), Silence(0.2));

        var trimmed = SilenceTrimmer.Trim(samples);

        // В результате не должно быть непрерывной 5-секундной тишины (она заменена коротким разрывом).
        var seconds = trimmed.Length / (double)SampleRate;
        Assert.True(seconds < 4.0, $"Expected compressed length, got {seconds:0.00} s");
    }

    [Fact]
    public void Trim_QuietSpeech_BelowFixedThreshold_IsNotCut()
    {
        // Тихая речь — амплитуда ниже бывшего фиксированного порога 0.01,
        // но выше фонового шума. Адаптивный порог должен её сохранить.
        var samples = Concat(Silence(0.3), QuietSpeech(1.0), Silence(0.3));

        var trimmed = SilenceTrimmer.Trim(samples);

        Assert.True(trimmed.Length > 0, "Quiet speech was incorrectly trimmed away");
        var seconds = trimmed.Length / (double)SampleRate;
        Assert.InRange(seconds, 1.0, 1.8);
    }

    [Fact]
    public void Trim_KeepsBothPhrasesWhenSeparatedByPause_WithQuietSpeech()
    {
        // Тихая речь + длинная пауза: обе фразы должны сохраниться, пауза — сжаться.
        var samples = Concat(Silence(0.3), QuietSpeech(0.8), Silence(5.0), QuietSpeech(0.8), Silence(0.3));

        var trimmed = SilenceTrimmer.Trim(samples);

        var seconds = trimmed.Length / (double)SampleRate;
        Assert.InRange(seconds, 1.8, 2.8);
    }

    private static float[] Concat(params float[][] arrays)
    {
        var total = arrays.Sum(a => a.Length);
        var result = new float[total];
        var offset = 0;
        foreach (var a in arrays)
        {
            Array.Copy(a, 0, result, offset, a.Length);
            offset += a.Length;
        }

        return result;
    }

    /// <summary>Тишина (нули).</summary>
    private static float[] Silence(double seconds) => new float[(int)(SampleRate * seconds)];

    /// <summary>Речь — синус с амплитудой выше порога.</summary>
    private static float[] Speech(double seconds) => Tone(seconds, 0.5f);

    /// <summary>Тихая речь — синус с амплитудой ниже бывшего фиксированного порога 0.01.</summary>
    private static float[] QuietSpeech(double seconds) => Tone(seconds, 0.005f);

    private static float[] Tone(double seconds, float amplitude)
    {
        var count = (int)(SampleRate * seconds);
        var data = new float[count];
        for (var i = 0; i < count; i++)
        {
            data[i] = (float)(Math.Sin(2 * Math.PI * 440 * i / SampleRate) * amplitude);
        }

        return data;
    }
}
