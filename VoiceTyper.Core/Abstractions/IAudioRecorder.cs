namespace VoiceTyper.Core.Abstractions;

/// <summary>Захват звука с микрофона в формате WAV 16 кГц / моно / 16 бит.</summary>
public interface IAudioRecorder : IDisposable
{
    bool IsRecording { get; }

    /// <summary>Формат, в котором устройство отдаёт данные (заполняется после Start).</summary>
    NAudio.Wave.WaveFormat? CaptureFormat { get; }

    /// <summary>Начинает запись с устройства по умолчанию.</summary>
    void Start();

    /// <summary>
    /// Останавливает запись и возвращает WAV 16 кГц / моно / 16 бит.
    /// Возвращает <c>null</c>, если ничего не записано.
    /// </summary>
    byte[]? Stop();

    /// <summary>Останавливает запись и отбрасывает результат.</summary>
    void Cancel();

    /// <summary>Возвращает накопленные с момента последнего вызова байты (для VAD-режима).</summary>
    byte[] DrainNewBytes();
}
