using System.Buffers.Binary;

namespace VoiceTyper.Core.Audio;

/// <summary>Разбор WAV (RIFF) в массив float-сэмплов.</summary>
public static class WavPcmReader
{
    /// <summary>
    /// Разбирает WAV-файл (PCM16, моно, 16 кГц — именно такой отдаёт текущий пайплайн записи)
    /// и конвертирует данные в сэмплы float32 нормализованные в [-1, 1]
    /// (int16/32768), как требуется для входа Parakeet.
    /// </summary>
    public static float[] To16KHzMonoFloats(byte[] wavBytes)
    {
        // RIFF-заголовок: "RIFF", размер, "WAVE: вместе 12 байт.
        if (wavBytes.Length < 12)
        {
            throw new ArgumentException("WAV слишком короткий (меньше 12 байт).", nameof(wavBytes));
        }

        if (Span8(wavBytes, 0) != "RIFF")
        {
            throw new NotSupportedException("Это не RIFF-контейнер.");
        }

        if (Span8(wavBytes, 8) != "WAVE")
        {
            throw new NotSupportedException("RIFF, но не WAV.");
        }

        // Прочесываем все чанки. Чанки выровнены на слово (2 байта).
        bool fmtOk = false;
        short audioFormat = 0, channels = 0, bitsPerSample = 0;
        int sampleRate = 0;
        byte[]? data = null;

        int pos = 12;
        while (pos + 8 <= wavBytes.Length)
        {
            var id = Span8(wavBytes, pos);
            int chunkSize = BinaryPrimitives.ReadInt32LittleEndian(wavBytes.AsSpan(pos + 4));
            if (chunkSize < 0 || pos + 8 + chunkSize > wavBytes.Length)
            {
                break; // Обратный порядок, либо битый файл — прекращаем.
            }

            if (id == "fmt ")
            {
                if (chunkSize < 16)
                    throw new NotSupportedException("Слишком короткий chunk fmt.");

                audioFormat = BinaryPrimitives.ReadInt16LittleEndian(wavBytes.AsSpan(pos + 8));
                channels = BinaryPrimitives.ReadInt16LittleEndian(wavBytes.AsSpan(pos + 10));
                sampleRate = BinaryPrimitives.ReadInt32LittleEndian(wavBytes.AsSpan(pos + 12));
                bitsPerSample = BinaryPrimitives.ReadInt16LittleEndian(wavBytes.AsSpan(pos + 22));
                fmtOk = true;
            }
            else if (id == "data")
            {
                data = wavBytes[(pos + 8)..(pos + 8 + chunkSize)];
            }

            pos += 8 + chunkSize + (chunkSize & 1);
        }

        if (!fmtOk || data is null)
        {
            throw new NotSupportedException("WAV без fmt- или data-чанка.");
        }

        if (audioFormat != 1)
        {
            throw new NotSupportedException("Требуется PCM-формат WAV (audioFormat=1).");
        }

        if (bitsPerSample != 16)
        {
            throw new NotSupportedException("Требуется PCM16 WAV (bitsPerSample=16).");
        }

        if (channels != 1)
        {
            throw new NotSupportedException("Требуется моно WAV.");
        }

        if (sampleRate != 16000)
        {
            throw new NotSupportedException("Требуется 16 кГц WAV.");
        }

        // Каждый второй байт — это int16 LE.
        // Восстанавливаем и конвертируем в float.
        var result = new float[data.Length / 2];
        for (int i = 0, b = 0; i < result.Length; i++, b += 2)
        {
            result[i] = BinaryPrimitives.ReadInt16LittleEndian(data.AsSpan(b)) / 32768f;
        }

        return result;
    }

    private static string Span8(byte[] buf, int offset) => System.Text.Encoding.ASCII.GetString(buf, offset, 4);
}
