using NAudio.Wave;
using VoiceTyper.Core.Audio;

namespace VoiceTyper.Tests.Audio;

public class WavBuilderTests
{
    [Fact]
    public void ConvertTo16KHzMonoWav_FromMono44K_Returns16KHzMono16Bit()
    {
        var raw = GenerateSinePcm16(frequency: 440, sampleRate: 44100, channels: 1, seconds: 1.0);

        var wav = WavBuilder.ConvertTo16KHzMonoWav(raw, new WaveFormat(44100, 16, 1));

        using var reader = new WaveFileReader(new MemoryStream(wav));
        Assert.Equal(16000, reader.WaveFormat.SampleRate);
        Assert.Equal(1, reader.WaveFormat.Channels);
        Assert.Equal(16, reader.WaveFormat.BitsPerSample);
        Assert.InRange(reader.TotalTime.TotalSeconds, 0.95, 1.05);
    }

    [Fact]
    public void ConvertTo16KHzMonoWav_FromStereo48K_ReturnsMono16KHz()
    {
        var raw = GenerateSinePcm16(frequency: 440, sampleRate: 48000, channels: 2, seconds: 0.5);

        var wav = WavBuilder.ConvertTo16KHzMonoWav(raw, new WaveFormat(48000, 16, 2));

        using var reader = new WaveFileReader(new MemoryStream(wav));
        Assert.Equal(16000, reader.WaveFormat.SampleRate);
        Assert.Equal(1, reader.WaveFormat.Channels);
        Assert.InRange(reader.TotalTime.TotalSeconds, 0.45, 0.55);
    }

    [Fact]
    public void ConvertTo16KHzMonoWav_EmptyInput_ReturnsWavHeader()
    {
        var wav = WavBuilder.ConvertTo16KHzMonoWav(Array.Empty<byte>(), new WaveFormat(16000, 16, 1));

        Assert.NotNull(wav);
        Assert.True(wav.Length >= 44, "WAV-заголовок должен присутствовать");
    }

    [Fact]
    public void ConvertTo16KHzMonoFloats_LengthMatchesDuration()
    {
        var raw = GenerateSinePcm16(frequency: 440, sampleRate: 48000, channels: 2, seconds: 0.5);

        var floats = WavBuilder.ConvertTo16KHzMonoFloats(raw, new WaveFormat(48000, 16, 2));

        Assert.InRange(floats.Length, 16000 * 0.45, 16000 * 0.55);
    }

    [Fact]
    public void ConvertTo16KHzMonoWav_TrimsLeadingAndTrailingSilence()
    {
        // 1 с тишины + 0.5 с речи + 1 с тишины.
        var silence = (int)(16000 * 1.0);
        var speech = (int)(16000 * 0.5);
        var raw = new byte[(silence + speech + silence) * 2];
        var idx = 0;
        for (var i = 0; i < silence; i++)
        {
            WriteShort(raw, ref idx, 0);
        }
        for (var i = 0; i < speech; i++)
        {
            var value = (short)(Math.Sin(2 * Math.PI * 440 * i / 16000) * short.MaxValue * 0.5);
            WriteShort(raw, ref idx, value);
        }
        for (var i = 0; i < silence; i++)
        {
            WriteShort(raw, ref idx, 0);
        }

        var wav = WavBuilder.ConvertTo16KHzMonoWav(raw, new WaveFormat(16000, 16, 1));

        using var reader = new WaveFileReader(new MemoryStream(wav));
        // Тишина (2 с) вырезана, осталась речи 0.5 с + запас 0.5 с ≈ 1.0 с.
        Assert.InRange(reader.TotalTime.TotalSeconds, 0.7, 1.2);
    }

    private static void WriteShort(byte[] buffer, ref int index, short value)
    {
        buffer[index++] = (byte)(value & 0xFF);
        buffer[index++] = (byte)((value >> 8) & 0xFF);
    }

    private static byte[] GenerateSinePcm16(double frequency, int sampleRate, int channels, double seconds)
    {
        var sampleCount = (int)(sampleRate * seconds);
        var bytes = new byte[sampleCount * channels * 2];
        var idx = 0;
        for (var i = 0; i < sampleCount; i++)
        {
            var value = (short)(Math.Sin(2 * Math.PI * frequency * i / sampleRate) * short.MaxValue * 0.5);
            for (var c = 0; c < channels; c++)
            {
                bytes[idx++] = (byte)(value & 0xFF);
                bytes[idx++] = (byte)((value >> 8) & 0xFF);
            }
        }

        return bytes;
    }
}
