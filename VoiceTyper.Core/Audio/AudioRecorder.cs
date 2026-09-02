using NAudio;
using NAudio.CoreAudioApi;
using NAudio.Wave;
using VoiceTyper.Core.Abstractions;

#pragma warning disable CS0618 // WasapiCapture намеренно используется как надёжный способ захвата для широкого круга устройств.

namespace VoiceTyper.Core.Audio;

/// <summary>
/// Захват звука с микрофона. Пробует несколько бэкендов и использует первый рабочий:
/// 0) RAW-WASAPI (инструментированный перебор комбинаций, как в Chromium);
/// 1) NAudio WASAPI;
/// 2) MME WaveIn.
/// Потокобезопасность: буфер — List&lt;byte&gt; под lock, состояние — явный флаг, остановка
/// сначала останавливает устройство, затем отписывается и уничтожает (не теряет хвост).
/// </summary>
public sealed class AudioRecorder : IAudioRecorder
{
    private enum Backend
    {
        None,
        Native,
        RawWasapi,
        Wasapi,
        Mme,
    }

    private static readonly WaveFormat[] MmeFormats =
    {
        new(16000, 16, 1),
        new(44100, 16, 1),
        new(44100, 16, 2),
        new(48000, 16, 1),
        new(48000, 16, 2),
    };

    private readonly string? _deviceId;
    private readonly List<byte> _buffer = new();
    private readonly object _lock = new();

    private Backend _backend = Backend.None;
    private WasapiCapture? _capture;
    private WaveIn? _waveIn;
    private RawWasapiCapture? _raw;
    private NativeWasapiCapture? _native;
    private int _drainedBytes;
    private bool _isRecordingActive;
    private bool _canceled;
    private string? _lastError;
    private string? _rawDiagnostic;
    private string? _wasapiDiagnostic;

    public AudioRecorder(string? deviceId = null)
    {
        _deviceId = deviceId;
    }

    /// <summary>Активный бэкенд захвата (для логирования).</summary>
    public string ActiveBackend => _backend switch
    {
        Backend.Native => "NATIVE-WASAPI",
        Backend.RawWasapi => "RAW-WASAPI",
        Backend.Wasapi => "WASAPI",
        Backend.Mme => "MME",
        _ => "нет",
    };

    /// <summary>Состояние записи по явному флагу (не зависит от перезаписываемых ссылок).</summary>
    public bool IsRecording => _isRecordingActive;

    public WaveFormat? CaptureFormat { get; private set; }

    /// <summary>Применять подавление фонового шума к записи.</summary>
    public bool NoiseReductionEnabled { get; set; }

    public void Start()
    {
        lock (_lock)
        {
            if (_isRecordingActive)
            {
                return;
            }

            _buffer.Clear();
            _drainedBytes = 0;
            _canceled = false;
            _lastError = null;
        }

        if (TryStartNative())
        {
            MarkStarted();
            return;
        }

        if (TryStartRawWasapi())
        {
            MarkStarted();
            return;
        }

        if (TryStartWasapi())
        {
            MarkStarted();
            return;
        }

        if (TryStartMme())
        {
            MarkStarted();
            return;
        }

        throw new InvalidOperationException(
            $"не удаётся открыть устройство (WASAPI/MME). {_lastError ?? "неизвестна"}"
            + (string.IsNullOrWhiteSpace(_wasapiDiagnostic) ? string.Empty : " | WASAPI: " + _wasapiDiagnostic)
            + (string.IsNullOrWhiteSpace(_rawDiagnostic) ? string.Empty : " | RAW-WASAPI: " + _rawDiagnostic));
    }

    private void MarkStarted()
    {
        lock (_lock)
        {
            _isRecordingActive = true;
        }
    }

    public byte[] DrainNewBytes()
    {
        lock (_lock)
        {
            if (_buffer.Count <= _drainedBytes)
            {
                return Array.Empty<byte>();
            }

            var newBytes = _buffer.GetRange(_drainedBytes, _buffer.Count - _drainedBytes).ToArray();
            _drainedBytes = _buffer.Count;
            return newBytes;
        }
    }

    public byte[]? Stop()
    {
        lock (_lock)
        {
            if (!_isRecordingActive)
            {
                return null;
            }

            _isRecordingActive = false;
        }

        StopCapture();

        byte[] raw;
        lock (_lock)
        {
            raw = _buffer.ToArray();
            _buffer.Clear();
            _drainedBytes = 0;
        }

        if (raw.Length == 0 || CaptureFormat is null)
        {
            return null;
        }

        return WavBuilder.ConvertTo16KHzMonoWav(raw, CaptureFormat, NoiseReductionEnabled);
    }

    public void Cancel()
    {
        lock (_lock)
        {
            _canceled = true;
            _isRecordingActive = false;
        }

        StopCapture();

        lock (_lock)
        {
            _buffer.Clear();
            _drainedBytes = 0;
        }
    }

    public void Dispose() => Cancel();

    private bool TryStartNative()
    {
        try
        {
            var native = new NativeWasapiCapture((data, count, rate, ch) =>
            {
                lock (_lock)
                {
                    if (_canceled)
                    {
                        return;
                    }

                    _buffer.AddRange(data);
                }
            });

            if (!native.TryStart(48000, 2))
            {
                native.Dispose();
                _lastError = "NATIVE-WASAPI: mc_start вернул ошибку";
                return false;
            }

            _native = native;
            _backend = Backend.Native;
            CaptureFormat = new WaveFormat(48000, 16, 2);
            return true;
        }
        catch (Exception ex)
        {
            _lastError = $"NATIVE-WASAPI ({ex.GetType().Name}): {ex.Message}";
            _native?.Dispose();
            _native = null;
            return false;
        }
    }

    private bool TryStartRawWasapi()
    {
        try
        {
            var raw = new RawWasapiCapture(_deviceId);
            var dataCallback = new Action<byte[], int>((data, count) =>
            {
                lock (_lock)
                {
                    if (_canceled)
                    {
                        return;
                    }

                    for (var i = 0; i < count; i++)
                    {
                        _buffer.Add(data[i]);
                    }
                }
            });

            if (!raw.TryStart(dataCallback, out var diagnostic))
            {
                _rawDiagnostic = diagnostic;
                _lastError = diagnostic;
                raw.Dispose();
                return false;
            }

            _raw = raw;
            _backend = Backend.RawWasapi;
            CaptureFormat = new WaveFormat(raw.SampleRate, raw.BitsPerSample, raw.Channels);
            return true;
        }
        catch (Exception ex)
        {
            _lastError = $"RAW-WASAPI: {ex.Message}";
            return false;
        }
    }

    private bool TryStartWasapi()
    {
        try
        {
            WasapiCapture capture;

            if (!string.IsNullOrEmpty(_deviceId) && _deviceId != "0")
            {
                var enumerator = new MMDeviceEnumerator();
                var device = enumerator.GetDevice(_deviceId);
                capture = new WasapiCapture(device);
            }
            else
            {
                var enumerator = new MMDeviceEnumerator();
                var defaultDevice = enumerator.GetDefaultAudioEndpoint(DataFlow.Capture, Role.Communications);
                capture = new WasapiCapture(defaultDevice);
            }

            capture.ShareMode = AudioClientShareMode.Shared;

            capture.DataAvailable += OnDataAvailable;
            capture.StartRecording();
            _capture = capture;
            _backend = Backend.Wasapi;
            CaptureFormat = capture.WaveFormat;
            return true;
        }
        catch (Exception ex)
        {
            _lastError = $"WASAPI ({ex.GetType().Name}): {ex.Message}";
            _wasapiDiagnostic = _lastError;
            CleanupWasapi();
            return false;
        }
    }

    private bool TryStartMme()
    {
        foreach (var format in MmeFormats)
        {
            var waveIn = new WaveIn
            {
                WaveFormat = format,
                BufferMilliseconds = 100,
                DeviceNumber = 0,
            };

            try
            {
                waveIn.DataAvailable += OnDataAvailable;
                waveIn.StartRecording();
                _waveIn = waveIn;
                _backend = Backend.Mme;
                CaptureFormat = format;
                return true;
            }
            catch (Exception ex)
            {
                _lastError = $"MME ({format}): {ex.Message}";
                waveIn.DataAvailable -= OnDataAvailable;
                waveIn.Dispose();
            }
        }

        return false;
    }

    private void StopCapture()
    {
        if (_native is not null)
        {
            try
            {
                _native.Dispose();
            }
            catch (Exception)
            {
            }

            _native = null;
        }

        if (_capture is not null)
        {
            try
            {
                _capture.StopRecording();
            }
            catch (Exception)
            {
            }

            _capture.DataAvailable -= OnDataAvailable;
            _capture.Dispose();
            _capture = null;
        }

        if (_waveIn is not null)
        {
            try
            {
                _waveIn.StopRecording();
            }
            catch (Exception)
            {
            }

            _waveIn.DataAvailable -= OnDataAvailable;
            _waveIn.Dispose();
            _waveIn = null;
        }

        if (_raw is not null)
        {
            _raw.Dispose();
            _raw = null;
        }

        _backend = Backend.None;
    }

    private void CleanupWasapi()
    {
        if (_capture is not null)
        {
            _capture.DataAvailable -= OnDataAvailable;
            _capture.Dispose();
            _capture = null;
        }
    }

    private void OnDataAvailable(object? sender, WaveInEventArgs e)
    {
        if (e.BytesRecorded == 0)
        {
            return;
        }

        lock (_lock)
        {
            if (_canceled)
            {
                return;
            }

            for (var i = 0; i < e.BytesRecorded; i++)
            {
                _buffer.Add(e.Buffer[i]);
            }
        }
    }
}
