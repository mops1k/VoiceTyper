using NAudio.Wave;
using VoiceTyper.Core.Abstractions;
using VoiceTyper.Core.Audio;
using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Services;

/// <summary>Состояние записи/обработки.</summary>
public enum RecordingState
{
    Idle,
    Recording,
    Processing,
}

/// <summary>Параметры распознавания и вывода для текущего сеанса (читаются на каждый вызов).</summary>
public sealed record TranscriptionOptions(RecognitionLanguage Language, string Prompt, bool AutoPaste, float Temperature = 0f, bool ConditionOnPreviousText = false);

/// <summary>
/// Конечный автомат записи: <c>Idle → Recording → Processing → Idle</c>.
/// Поддерживает три режима:
/// <list type="bullet">
/// <item>PushToTalk — запись пока зажата клавиша;</item>
/// <item>Toggle — нажал/нажал;</item>
/// <item>Vad — авто-остановка по тишине (Silero VAD).</item>
/// </list>
/// События могут вызываться с фоновых потоков — подписчики сами машаллят на UI.
/// </summary>
public interface IRecordingStateMachine : IAsyncDisposable
{
    event Action<RecordingState>? StateChanged;
    event Action<string>? TextReady;
    event Action<string>? Failed;

    RecordingState State { get; }

    /// <summary>Текущая фоновая операция (для ожидания в тестах); может быть <c>null</c>.</summary>
    Task? CurrentOperation { get; }

    /// <summary>Обработка нажатия клавиши записи.</summary>
    void PressRecord();

    /// <summary>Обработка отпускания клавиши записи (push-to-talk).</summary>
    void ReleaseRecord();

    /// <summary>Отмена текущей записи/обработки.</summary>
    void Cancel();
}

public sealed class RecordingStateMachine : IRecordingStateMachine
{
    private static readonly TimeSpan VadPollInterval = TimeSpan.FromMilliseconds(250);

    private readonly IAudioRecorder _recorder;
    private readonly ITranscriptionService _transcription;
    private readonly ITextOutputService _output;
    private readonly Func<ISpeechSegmenter> _segmenterFactory;
    private readonly Func<TranscriptionOptions> _optionsProvider;
    private readonly RecordingMode _mode;
    private readonly TimeSpan _silenceThreshold;
    private readonly object _lock = new();

    private CancellationTokenSource? _sessionCts;
    private ISpeechSegmenter? _segmenter;
    private Task? _operation;

    public RecordingStateMachine(
        IAudioRecorder recorder,
        ITranscriptionService transcription,
        ITextOutputService output,
        RecordingMode mode,
        TimeSpan silenceThreshold,
        Func<TranscriptionOptions> optionsProvider,
        Func<ISpeechSegmenter>? segmenterFactory = null)
    {
        _recorder = recorder;
        _transcription = transcription;
        _output = output;
        _mode = mode;
        _silenceThreshold = silenceThreshold;
        _optionsProvider = optionsProvider;
        _segmenterFactory = segmenterFactory ?? (() => throw new NotSupportedException("VAD недоступен"));
    }

    public event Action<RecordingState>? StateChanged;
    public event Action<string>? TextReady;
    public event Action<string>? Failed;

    public RecordingState State { get; private set; } = RecordingState.Idle;

    public Task? CurrentOperation => _operation;

    public void PressRecord()
    {
        lock (_lock)
        {
            switch (State)
            {
                case RecordingState.Idle:
                    StartRecording();
                    break;
                case RecordingState.Recording when _mode == RecordingMode.Toggle:
                    StopAndProcess();
                    break;
            }
        }
    }

    public void ReleaseRecord()
    {
        lock (_lock)
        {
            if (State == RecordingState.Recording && _mode == RecordingMode.PushToTalk)
            {
                StopAndProcess();
            }
        }
    }

    public void Cancel()
    {
        lock (_lock)
        {
            _sessionCts?.Cancel();
            _recorder.Cancel();
            SetState(RecordingState.Idle);
        }
    }

    public ValueTask DisposeAsync()
    {
        lock (_lock)
        {
            _sessionCts?.Cancel();
            _segmenter?.Dispose();
            _segmenter = null;
            _sessionCts?.Dispose();
            _sessionCts = null;
            SetState(RecordingState.Idle);
        }

        return ValueTask.CompletedTask;
    }

    private void StartRecording()
    {
        _sessionCts?.Dispose();
        _sessionCts = new CancellationTokenSource();

        try
        {
            _recorder.Start();
        }
        catch (Exception ex)
        {
            _sessionCts?.Dispose();
            _sessionCts = null;
            Failed?.Invoke($"Не удалось открыть микрофон: {ex.Message}. Убедитесь, что устройство активно и не занято другим приложением.");
            return;
        }

        SetState(RecordingState.Recording);

        if (_mode == RecordingMode.Vad)
        {
            _segmenter = _segmenterFactory();
            var cts = _sessionCts;
            _ = Task.Run(() => VadLoopAsync(cts.Token));
        }
    }

    private void StopAndProcess()
    {
        var wav = _recorder.Stop();
        if (wav is null || wav.Length == 0)
        {
            _segmenter?.Dispose();
            _segmenter = null;
            SetState(RecordingState.Idle);
            return;
        }

        SetState(RecordingState.Processing);
        var cts = _sessionCts;
        _operation = ProcessAsync(wav, cts?.Token ?? CancellationToken.None);
    }

    private async Task ProcessAsync(byte[] wav, CancellationToken ct)
    {
        try
        {
            var options = _optionsProvider();
            var text = await _transcription.TranscribeAsync(wav, options.Language, options.Prompt, ct,
                options.Temperature, options.ConditionOnPreviousText);

            if (!string.IsNullOrWhiteSpace(text) && await _output.OutputAsync(text, options.AutoPaste, ct))
            {
                TextReady?.Invoke(text);
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            Failed?.Invoke(ex.Message);
        }
        finally
        {
            lock (_lock)
            {
                _segmenter?.Dispose();
                _segmenter = null;
                _operation = null;
                SetState(RecordingState.Idle);
            }
        }
    }

    private async Task VadLoopAsync(CancellationToken ct)
    {
        var detector = new SilenceAutoStopDetector(_segmenter!, _silenceThreshold);
        try
        {
            while (!ct.IsCancellationRequested)
            {
                byte[] newBytes;
                WaveFormat? format;
                lock (_lock)
                {
                    newBytes = _recorder.DrainNewBytes();
                    format = _recorder.CaptureFormat;
                }

                if (newBytes.Length > 0 && format is not null)
                {
                    var floats = WavBuilder.ConvertTo16KHzMonoFloats(newBytes, format);
                    if (detector.Process(floats))
                    {
                        lock (_lock)
                        {
                            if (State == RecordingState.Recording)
                            {
                                StopAndProcess();
                            }
                        }

                        return;
                    }
                }

                await Task.Delay(VadPollInterval, ct);
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (ObjectDisposedException)
        {
            // Сегментер был освобождён отменой — просто завершаем цикл.
        }
    }

    private void SetState(RecordingState newState)
    {
        if (State == newState)
        {
            return;
        }

        State = newState;
        StateChanged?.Invoke(newState);
    }
}
