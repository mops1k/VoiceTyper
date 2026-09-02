using VoiceTyper.Core.Abstractions;
using VoiceTyper.Core.Audio;

namespace VoiceTyper.Tests.Audio;

public class SilenceAutoStopDetectorTests
{
    [Fact]
    public void NoSpeech_ReturnsTrueAfterMaxIdle()
    {
        var detector = new SilenceAutoStopDetector(new FakeSegmenter(), TimeSpan.FromSeconds(1));

        var chunks = (int)Math.Ceiling(SilenceAutoStopDetector.MaxIdleBeforeSpeech.TotalSeconds / 0.5);
        var shouldStop = false;
        for (var i = 0; i < chunks; i++)
        {
            shouldStop = detector.Process(new float[8000]); // 0.5 c при 16 кГц
        }

        Assert.True(shouldStop);
    }

    [Fact]
    public void NoSpeech_BeforeMaxIdle_ReturnsFalse()
    {
        var detector = new SilenceAutoStopDetector(new FakeSegmenter(), TimeSpan.FromSeconds(1));

        var shouldStop = detector.Process(new float[8000]);

        Assert.False(shouldStop);
    }

    [Fact]
    public void SpeechThenSilence_StopsWhenSilenceExceedsThreshold()
    {
        var segmenter = new FakeSegmenter
        {
            // Первый чанк: речь 0..0.5 c. Дальше — тишина.
            Handler = i => i == 0 ? new[] { (TimeSpan.Zero, TimeSpan.FromMilliseconds(500)) } : Array.Empty<(TimeSpan, TimeSpan)>(),
        };
        var detector = new SilenceAutoStopDetector(segmenter, TimeSpan.FromSeconds(1.0));

        Assert.False(detector.Process(new float[8000])); // чанк 0: речь 0..0.5
        Assert.False(detector.Process(new float[8000])); // чанк 1: тишина 0.5..1.0 (0.5 с тишины < 1.0)
        Assert.True(detector.Process(new float[8000]));  // чанк 2: тишина 1.0..1.5 (1.0 с тишины)

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
            Assert.False(detector.Process(new float[8000]));
        }

        segmenter.Dispose();
    }

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
