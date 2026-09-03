using VoiceTyper.Core.Abstractions;
using VoiceTyper.Core.Audio;

namespace VoiceTyper.Tests.Audio;

public class SilenceAutoStopDetectorTests
{
    private const double SilenceSeconds = 0.5;
    private const int ChunkSamples = 8000; // 0.5 c при 16 кГц

    [Fact]
    public void NoSpeech_ReturnsTrueAfterMaxIdle()
    {
        var detector = new SilenceAutoStopDetector(new FakeSegmenter(), TimeSpan.FromSeconds(1));

        var chunks = (int)Math.Ceiling(SilenceAutoStopDetector.MaxIdleBeforeSpeech.TotalSeconds / SilenceSeconds);
        var shouldStop = false;
        for (var i = 0; i < chunks; i++)
        {
            shouldStop = detector.Process(SilenceChunk());
        }

        Assert.True(shouldStop);
    }

    [Fact]
    public void NoSpeech_BeforeMaxIdle_ReturnsFalse()
    {
        var detector = new SilenceAutoStopDetector(new FakeSegmenter(), TimeSpan.FromSeconds(1));

        var shouldStop = detector.Process(SilenceChunk());

        Assert.False(shouldStop);
    }

    [Fact]
    public void NoSpeech_StopsWithNoSpeechIdleReason()
    {
        var detector = new SilenceAutoStopDetector(new FakeSegmenter(), TimeSpan.FromSeconds(1));

        for (var i = 0; i < 9; i++)
        {
            Assert.False(detector.Process(SilenceChunk()));
        }

        Assert.True(detector.Process(SilenceChunk(), out var reason));
        Assert.Equal(VadStopReason.NoSpeechIdle, reason);
    }

    [Fact]
    public void SpeechThenSilence_StopsWhenSilenceExceedsThreshold()
    {
        var segmenter = new FakeSegmenter
        {
            // Первый чанк: речь 0..0.5 c. Дальше — тишина.
            Handler = i =>
                i == 0
                    ? new[] { (TimeSpan.Zero, TimeSpan.FromMilliseconds(500)) }
                    : Array.Empty<(TimeSpan, TimeSpan)>(),
        };
        var detector = new SilenceAutoStopDetector(segmenter, TimeSpan.FromSeconds(1.0));

        Assert.False(detector.Process(SilenceChunk())); // чанк 0: речь 0..0.5
        Assert.False(detector.Process(SilenceChunk())); // чанк 1: тишина 0.5..1.0 (0.5 с тишины < 1.0)
        Assert.True(detector.Process(SilenceChunk(), out var reason)); // чанк 2: тишина 1.0..1.5 (1.0 с тишины)
        Assert.Equal(VadStopReason.TrailingSilence, reason);

        segmenter.Dispose();
    }

    [Fact]
    public void SpeechContinues_DoesNotStop()
    {
        var segmenter = new FakeSegmenter
        {
            // В каждом чанке речь до конца чанка.
            Handler = _ => new[] { (TimeSpan.Zero, TimeSpan.FromMilliseconds(499)) },
        };
        var detector = new SilenceAutoStopDetector(segmenter, TimeSpan.FromSeconds(1.0));

        for (var i = 0; i < 10; i++)
        {
            Assert.False(detector.Process(SilenceChunk()));
        }

        segmenter.Dispose();
    }

    [Fact]
    public void SegmentEndingNearChunkEnd_KeepsChunkActive()
    {
        // Silero в потоковом режиме может «закрывать» сегмент с задержкой:
        // речь реально продолжается до конца фрагмента, но VAD отдаёт конец чуть раньше.
        var segmenter = new FakeSegmenter
        {
            Handler = _ => new[] { (TimeSpan.Zero, TimeSpan.FromMilliseconds(450)) },
        };
        var detector = new SilenceAutoStopDetector(segmenter, TimeSpan.FromSeconds(1.0));

        for (var i = 0; i < 20; i++)
        {
            Assert.False(detector.Process(SilenceChunk()));
        }

        segmenter.Dispose();
    }

    [Fact]
    public void EnergySpeech_WithoutVadSegments_KeepsRecordingActive()
    {
        // Регресс-тест бага «запись обрывается во время речи»: VAD может молчать
        // на непрерывной речи, но уровень сигнала выше шумового пола — стоп запрещён.
        var detector = new SilenceAutoStopDetector(new FakeSegmenter(), TimeSpan.FromSeconds(1.0));

        for (var i = 0; i < 40; i++)
        {
            Assert.False(detector.Process(SpeechChunk()));
        }
    }

    [Fact]
    public void QuietSpeech_WithoutVadSegments_KeepsRecordingActive()
    {
        // Тихий, но непрерывный голос не должен считаться тишиной (иначе запись
        // обрывается посреди диктовки). Амплитуда 0.01 => энергия 1e-4.
        var detector = new SilenceAutoStopDetector(new FakeSegmenter(), TimeSpan.FromSeconds(1.0));

        for (var i = 0; i < 40; i++)
        {
            Assert.False(detector.Process(SpeechChunk(amplitude: 0.01f)));
        }
    }

    [Fact]
    public void EnergySpeech_ThenSilence_StopsAfterThreshold()
    {
        var detector = new SilenceAutoStopDetector(new FakeSegmenter(), TimeSpan.FromSeconds(1.0));

        Assert.False(detector.Process(SpeechChunk())); // чанк 0: речь (энергия)
        Assert.False(detector.Process(SilenceChunk())); // чанк 1: тишина 0.5 с
        Assert.True(detector.Process(SilenceChunk(), out var reason)); // чанк 2: тишина 1.0 с
        Assert.Equal(VadStopReason.TrailingSilence, reason);
    }

    [Fact]
    public void LowLevelNoise_DoesNotKeepRecordingAlive()
    {
        // Фоновый шум ниже порога речи не должен держать запись «вечно».
        var detector = new SilenceAutoStopDetector(new FakeSegmenter(), TimeSpan.FromSeconds(1.0));

        var chunks = (int)Math.Ceiling(SilenceAutoStopDetector.MaxIdleBeforeSpeech.TotalSeconds / SilenceSeconds);
        for (var i = 0; i < chunks - 1; i++)
        {
            Assert.False(detector.Process(NoiseChunk()));
        }

        Assert.True(detector.Process(NoiseChunk(), out var reason));
        Assert.Equal(VadStopReason.NoSpeechIdle, reason);
    }

    [Fact]
    public void StreamAbsoluteSegment_IsNotAddedToChunkBase()
    {
        // Сегмент, время которого уже абсолютно (whisper не сбрасывает счётчик потока),
        // не должен ещё раз прибавляться к базе фрагмента.
        var segmenter = new FakeSegmenter
        {
            Handler = i => i == 1
                ? new[] { (TimeSpan.FromMilliseconds(700), TimeSpan.FromMilliseconds(900)) }
                : Array.Empty<(TimeSpan, TimeSpan)>(),
        };
        var detector = new SilenceAutoStopDetector(segmenter, TimeSpan.FromSeconds(1.0));

        Assert.False(detector.Process(SilenceChunk())); // чанк 0: нет речи (0..0.5)
        Assert.False(detector.Process(SilenceChunk())); // чанк 1: абсолютный сегмент, конец 0.9 с
        Assert.False(detector.Process(SilenceChunk())); // чанк 2: тишина до 1.5 (0.6 с < 1.0)
        Assert.True(detector.Process(SilenceChunk())); // чанк 3: тишина до 2.0 (1.1 с >= 1.0)

        segmenter.Dispose();
    }

    private static float[] SpeechChunk(float amplitude = 0.2f)
    {
        var samples = new float[ChunkSamples];
        Array.Fill(samples, amplitude);
        return samples;
    }

    private static float[] NoiseChunk(float amplitude = 0.002f) => SpeechChunk(amplitude);

    private static float[] SilenceChunk() => new float[ChunkSamples];

    private sealed class FakeSegmenter : ISpeechSegmenter
    {
        public Func<int, IReadOnlyList<(TimeSpan Start, TimeSpan End)>>? Handler { get; set; }
        private int _calls;

        public IReadOnlyList<(TimeSpan Start, TimeSpan End)> DetectSpeechNoReset(float[] samples)
        {
            return Handler?.Invoke(_calls++) ?? Array.Empty<(TimeSpan, TimeSpan)>();
        }

        public void ResetState()
        {
        }

        public void Dispose()
        {
        }
    }
}