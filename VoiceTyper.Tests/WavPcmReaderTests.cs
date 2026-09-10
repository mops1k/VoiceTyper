using VoiceTyper.Core.Audio;
using VoiceTyper.Core.Services.Transcription;

namespace VoiceTyper.Tests;

/// <summary>Тесты разбора WAV и конверсии PCM16→float для входа Parakeet.</summary>
public class WavPcmReaderTests
{
    /// <summary>Собирает каноничный WAV: RIFF/fmt/data, PCM16 моно 16 кГц.</summary>
    private static byte[] BuildWav(short[] samples)
    {
        var ms = new MemoryStream();
        var dataLen = samples.Length * 2;
        // RIFF-заголовок (12) + fmt (8+16) + data (8+dataLen), паддинг учитывается при чётности.
        ms.Write("RIFF"u8);
        ms.Write(BitConverter.GetBytes(36 + dataLen));
        ms.Write("WAVE"u8);
        ms.Write("fmt "u8);
        ms.Write(BitConverter.GetBytes(16));
        ms.Write(BitConverter.GetBytes((short)1)); // PCM
        ms.Write(BitConverter.GetBytes((short)1)); // mono
        ms.Write(BitConverter.GetBytes(16000));    // 16 кГц
        ms.Write(BitConverter.GetBytes(32000));    // byte rate = 16000*1*2
        ms.Write(BitConverter.GetBytes((short)2)); // block align
        ms.Write(BitConverter.GetBytes((short)16)); // bits
        ms.Write("data"u8);
        ms.Write(BitConverter.GetBytes(dataLen));
        foreach (var s in samples)
        {
            ms.Write(BitConverter.GetBytes(s));
        }

        return ms.ToArray();
    }

    [Fact]
    public void To16KHzMonoFloats_ConvertsPlusMinusFullScale()
    {
        var wav = BuildWav(new short[] { 32767, -32768, 0, 16384 });

        var floats = WavPcmReader.To16KHzMonoFloats(wav);

        Assert.Equal(4, floats.Length);
        Assert.Equal(0.99996948f, floats[0], 5);
        Assert.Equal(-1.0f, floats[1], 5);
        Assert.Equal(0f, floats[2]);
        Assert.Equal(0.5f, floats[3], 5);
    }

    [Fact]
    public void To16KHzMonoFloats_ParsesNonCanonicalChunks()
    {
        // Не каноничная структура: LIST-чанк между заголовком и fmt/data.
        var samples = new short[] { -16384 };
        var ms = new MemoryStream();
        ms.Write("RIFF"u8);
        var total = 12 + (8 + 4) + 4 + (8 + 16) + 4 + (8 + samples.Length * 2);
        ms.Write(BitConverter.GetBytes(total - 8));
        ms.Write("WAVE"u8);
        ms.Write("LIST"u8);
        ms.Write(BitConverter.GetBytes(4));
        ms.Write("INFO"u8);
        ms.Write("fmt "u8);
        ms.Write(BitConverter.GetBytes(16));
        ms.Write(BitConverter.GetBytes((short)1));
        ms.Write(BitConverter.GetBytes((short)1));
        ms.Write(BitConverter.GetBytes(16000));
        ms.Write(BitConverter.GetBytes(32000));
        ms.Write(BitConverter.GetBytes((short)2));
        ms.Write(BitConverter.GetBytes((short)16));
        ms.Write("data"u8);
        ms.Write(BitConverter.GetBytes(samples.Length * 2));
        foreach (var v in samples)
        {
            ms.Write(BitConverter.GetBytes(v));
        }

        var floats = WavPcmReader.To16KHzMonoFloats(ms.ToArray());

        var single = Assert.Single(floats);
        Assert.Equal(-0.5f, single, 5);
    }

    [Fact]
    public void To16KHzMonoFloats_RejectsStereo()
    {
        var wav = BuildWav(new short[] { 0, 0 });
        var ms = new MemoryStream();
        ms.Write("RIFF"u8);
        ms.Write(BitConverter.GetBytes(36 + 4));
        ms.Write("WAVE"u8);
        ms.Write("fmt "u8);
        ms.Write(BitConverter.GetBytes(16));
        ms.Write(BitConverter.GetBytes((short)1));
        ms.Write(BitConverter.GetBytes((short)2)); // stereo
        ms.Write(BitConverter.GetBytes(16000));
        ms.Write(BitConverter.GetBytes(64000));
        ms.Write(BitConverter.GetBytes((short)4));
        ms.Write(BitConverter.GetBytes((short)16));
        ms.Write("data"u8);
        ms.Write(BitConverter.GetBytes(4));
        ms.Write(new byte[] { 0, 0, 0, 0 });

        Assert.Throws<NotSupportedException>(() => WavPcmReader.To16KHzMonoFloats(ms.ToArray()));
    }

    [Fact]
    public void To16KHzMonoFloats_RejectsNonPcmFormat()
    {
        var ms = new MemoryStream();
        ms.Write("RIFF"u8);
        ms.Write(BitConverter.GetBytes(36 + 4));
        ms.Write("WAVE"u8);
        ms.Write("fmt "u8);
        ms.Write(BitConverter.GetBytes(16));
        ms.Write(BitConverter.GetBytes((short)28)); // MPEG ADPCM
        ms.Write(BitConverter.GetBytes((short)1));
        ms.Write(BitConverter.GetBytes(16000));
        ms.Write(BitConverter.GetBytes(32000));
        ms.Write(BitConverter.GetBytes((short)2));
        ms.Write(BitConverter.GetBytes((short)16));
        ms.Write("data"u8);
        ms.Write(BitConverter.GetBytes(4));
        ms.Write(new byte[] { 0, 0, 0, 0 });

        Assert.Throws<NotSupportedException>(() => WavPcmReader.To16KHzMonoFloats(ms.ToArray()));
    }
}
