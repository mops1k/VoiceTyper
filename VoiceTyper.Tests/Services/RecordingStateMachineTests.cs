using NAudio.Wave;
using VoiceTyper.Core.Abstractions;
using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;

namespace VoiceTyper.Tests.Services;

public class RecordingStateMachineTests
{
    private static readonly TranscriptionOptions DefaultOptions = new(RecognitionLanguage.Ru, "API,CPU", AutoPaste: true);

    [Fact]
    public void ToggleMode_PressStarts_PressAgainProcesses()
    {
        var ctx = CreateContext(RecordingMode.Toggle, wav: new byte[] { 1, 2, 3 });

        ctx.Machine.PressRecord();
        Assert.Equal(RecordingState.Recording, ctx.Machine.State);
        Assert.Equal(1, ctx.Recorder.StartCount);

        ctx.Machine.PressRecord();
        ctx.Machine.CurrentOperation?.Wait(TimeSpan.FromSeconds(5));

        Assert.Equal(RecordingState.Idle, ctx.Machine.State);
        Assert.Equal(1, ctx.Transcription.Calls);
        Assert.Single(ctx.ReadyTexts);
        Assert.Single(ctx.Output.Calls);
        Assert.True(ctx.Output.Calls[0].AutoPaste);
    }

    [Fact]
    public void PushToTalk_ReleaseStopsRecording()
    {
        var ctx = CreateContext(RecordingMode.PushToTalk, wav: new byte[] { 1, 2, 3 });

        ctx.Machine.PressRecord();
        Assert.Equal(RecordingState.Recording, ctx.Machine.State);

        ctx.Machine.PressRecord(); // повторное нажатие в PushToTalk не должно останавливать
        Assert.Equal(RecordingState.Recording, ctx.Machine.State);

        ctx.Machine.ReleaseRecord();
        ctx.Machine.CurrentOperation?.Wait(TimeSpan.FromSeconds(5));

        Assert.Equal(RecordingState.Idle, ctx.Machine.State);
        Assert.Single(ctx.ReadyTexts);
    }

    [Fact]
    public void PushToTalk_ReleaseWithoutRecording_DoesNothing()
    {
        var ctx = CreateContext(RecordingMode.PushToTalk, wav: new byte[] { 1, 2, 3 });

        ctx.Machine.ReleaseRecord();

        Assert.Equal(RecordingState.Idle, ctx.Machine.State);
        Assert.Equal(0, ctx.Recorder.StartCount);
    }

    [Fact]
    public void EmptyRecording_ReturnsToIdleWithoutProcessing()
    {
        var ctx = CreateContext(RecordingMode.Toggle, wav: null);

        ctx.Machine.PressRecord();
        ctx.Machine.PressRecord();

        Assert.Equal(RecordingState.Idle, ctx.Machine.State);
        Assert.Equal(0, ctx.Transcription.Calls);
        Assert.Empty(ctx.ReadyTexts);
    }

    [Fact]
    public void Cancel_DuringRecording_StopsAndDiscards()
    {
        var ctx = CreateContext(RecordingMode.Toggle, wav: new byte[] { 1, 2, 3 });

        ctx.Machine.PressRecord();
        ctx.Machine.Cancel();

        Assert.Equal(RecordingState.Idle, ctx.Machine.State);
        Assert.Equal(1, ctx.Recorder.CancelCount);
        Assert.Equal(0, ctx.Transcription.Calls);
    }

    [Fact]
    public void VadMode_AutoStopsAfterSilence()
    {
        var vadSegmenter = new FakeVadSegmenter();
        var ctx = CreateContext(
            RecordingMode.Vad,
            wav: new byte[] { 1, 2, 3 },
            segmenterFactory: () => vadSegmenter,
            silenceThreshold: TimeSpan.FromSeconds(1.0));
        ctx.Recorder.NewBytes = new byte[16000]; // 8000 сэмплов = 0.5 с при 16 кГц

        ctx.Machine.PressRecord();
        Assert.Equal(RecordingState.Recording, ctx.Machine.State);

        // Ждём, пока VAD-цикл сам остановит запись (речь в 1-м чанке, дальше тишина).
        var deadline = DateTime.UtcNow.AddSeconds(15);
        while (ctx.Machine.State != RecordingState.Idle && DateTime.UtcNow < deadline)
        {
            Thread.Sleep(50);
        }

        Assert.Equal(RecordingState.Idle, ctx.Machine.State);
        Assert.True(vadSegmenter.Calls >= 2, $"VAD вызван {vadSegmenter.Calls} раз");
        Assert.Single(ctx.ReadyTexts);
        Assert.Equal(1, ctx.Recorder.StopCount);
    }

    private static (RecordingStateMachine Machine, FakeRecorder Recorder, FakeTranscription Transcription, FakeOutput Output, List<string> ReadyTexts)
        CreateContext(
            RecordingMode mode,
            byte[]? wav,
            Func<ISpeechSegmenter>? segmenterFactory = null,
            TimeSpan? silenceThreshold = null)
    {
        var recorder = new FakeRecorder { WavToReturn = wav };
        var transcription = new FakeTranscription();
        var output = new FakeOutput();
        var readyTexts = new List<string>();

        var machine = new RecordingStateMachine(
            recorder,
            transcription,
            output,
            mode,
            silenceThreshold ?? TimeSpan.FromSeconds(1.2),
            () => DefaultOptions,
            segmenterFactory);
        machine.TextReady += t => readyTexts.Add(t);

        return (machine, recorder, transcription, output, readyTexts);
    }

    private sealed class FakeRecorder : IAudioRecorder
    {
        public bool IsRecording { get; private set; }
        public WaveFormat? CaptureFormat { get; set; } = new WaveFormat(16000, 16, 1);
        public byte[]? WavToReturn { get; set; }
        public byte[] NewBytes { get; set; } = Array.Empty<byte>();
        public int StartCount { get; private set; }
        public int StopCount { get; private set; }
        public int CancelCount { get; private set; }

        public void Start()
        {
            IsRecording = true;
            StartCount++;
        }

        public byte[]? Stop()
        {
            IsRecording = false;
            StopCount++;
            return WavToReturn;
        }

        public void Cancel()
        {
            IsRecording = false;
            CancelCount++;
        }

        public byte[] DrainNewBytes() => NewBytes;

        public void Dispose()
        {
        }
    }

    private sealed class FakeTranscription : ITranscriptionService
    {
        public string ModelPath => "fake-model";
        public int Calls { get; private set; }

        public Task<string> TranscribeAsync(byte[] wavBytes, RecognitionLanguage language, string prompt, CancellationToken ct = default,
            float temperature = 0f, bool conditionOnPreviousText = false)
        {
            Calls++;
            return Task.FromResult("привет");
        }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private sealed class FakeOutput : ITextOutputService
    {
        public List<(string Text, bool AutoPaste)> Calls { get; } = new();

        public Task<bool> OutputAsync(string text, bool autoPaste, CancellationToken ct = default)
        {
            Calls.Add((text, autoPaste));
            return Task.FromResult(true);
        }
    }

    private sealed class FakeVadSegmenter : ISpeechSegmenter
    {
        public int Calls { get; private set; }

        public IReadOnlyList<(TimeSpan Start, TimeSpan End)> DetectSpeechNoReset(float[] samples)
        {
            Calls++;
            return Calls == 1
                ? new[] { (TimeSpan.Zero, TimeSpan.FromMilliseconds(300)) }
                : Array.Empty<(TimeSpan, TimeSpan)>();
        }

        public void ResetState()
        {
        }

        public void Dispose()
        {
        }
    }
}
