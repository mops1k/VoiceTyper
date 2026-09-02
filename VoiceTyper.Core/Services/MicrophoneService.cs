using NAudio.CoreAudioApi;

namespace VoiceTyper.Core.Services;

/// <summary>Информация о доступном микрофоне.</summary>
public sealed record MicrophoneDevice(string Id, string Name);

/// <summary>Перечисление устройств захвата звука.</summary>
public interface IMicrophoneService
{
    /// <summary>Список активных микрофонов. Пустой — если устройств нет.</summary>
    IReadOnlyList<MicrophoneDevice> GetMicrophones();
}

public sealed class MicrophoneService : IMicrophoneService
{
    public IReadOnlyList<MicrophoneDevice> GetMicrophones()
    {
        try
        {
            using var enumerator = new MMDeviceEnumerator();
            return enumerator.EnumerateAudioEndPoints(DataFlow.Capture, DeviceState.Active)
                .Select(d => new MicrophoneDevice(d.ID, d.FriendlyName))
                .ToArray();
        }
        catch
        {
            return Array.Empty<MicrophoneDevice>();
        }
    }
}
