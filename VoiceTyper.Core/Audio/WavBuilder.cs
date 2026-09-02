using NAudio.Wave;
using NAudio.Wave.SampleProviders;

namespace VoiceTyper.Core.Audio;

/// <summary>
/// Конвертирует сырые PCM-байты в целевой формат Whisper: WAV 16 кГц / моно / 16-бит.
/// Whisper требует именно 16 кГц моно, а устройства захвата обычно отдают 44.1/48 кГц стерео.
/// </summary>
public static class WavBuilder
{
    /// <summary>Целевая частота дискретизации для Whisper.</summary>
    public const int TargetSampleRate = 16000;

    /// <summary>Преобразует сырые PCM-байты в WAV (16 кГц, моно, 16 бит).</summary>
    public static byte[] ConvertTo16KHzMonoWav(byte[] rawPcm, WaveFormat sourceFormat, bool noiseReduction = false)
    {
        var floats = ReadMono16KHzFloats(rawPcm, sourceFormat);
        if (noiseReduction)
        {
            NoiseSuppressor.Process(floats);
        }

        using var outStream = new MemoryStream();
        var provider = new SampleToWaveProvider16(new FloatListSampleProvider(floats, TargetSampleRate));
        WaveFileWriter.WriteWavFileToStream(outStream, provider);
        return outStream.ToArray();
    }

    /// <summary>Преобразует сырые PCM-байты в float-сэмплы 16 кГц моно (для Silero VAD).</summary>
    public static float[] ConvertTo16KHzMonoFloats(byte[] rawPcm, WaveFormat sourceFormat) =>
        ReadMono16KHzFloats(rawPcm, sourceFormat);

    private static float[] ReadMono16KHzFloats(byte[] rawPcm, WaveFormat sourceFormat)
    {
        using var rawStream = new RawSourceWaveStream(new MemoryStream(rawPcm), sourceFormat);
        ISampleProvider samples = rawStream.ToSampleProvider();
        if (sourceFormat.Channels > 1)
        {
            samples = new StereoToMonoSampleProvider(samples);
        }

        var resampler = new WdlResamplingSampleProvider(samples, TargetSampleRate);

        var result = new List<float>(rawPcm.Length / 2);
        var buffer = new float[8192];
        int read;
        while ((read = resampler.Read(buffer)) > 0)
        {
            result.AddRange(buffer.AsSpan(0, read).ToArray());
        }

        return result.ToArray();
    }

    private sealed class FloatListSampleProvider : ISampleProvider
    {
        private readonly float[] _data;
        private int _position;

        public FloatListSampleProvider(float[] data, int sampleRate)
        {
            _data = data;
            WaveFormat = WaveFormat.CreateIeeeFloatWaveFormat(sampleRate, 1);
        }

        public WaveFormat WaveFormat { get; }

        public int Read(float[] buffer, int offset, int count)
        {
            var available = Math.Min(count, _data.Length - _position);
            Array.Copy(_data, _position, buffer, offset, available);
            _position += available;
            return available;
        }

        public int Read(Span<float> buffer)
        {
            var available = Math.Min(buffer.Length, _data.Length - _position);
            _data.AsSpan(_position, available).CopyTo(buffer);
            _position += available;
            return available;
        }
    }
}
