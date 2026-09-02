using System.Runtime.InteropServices;

namespace VoiceTyper.Core.Audio;

/// <summary>
/// Нативный WASAPI-захват через mc_wasapi.dll (Intel Smart Sound: EXCLUSIVE + PCM16 48к стерео).
/// .NET-интероп IAudioClient на этом устройстве даёт сбой (Initialize «успешен», GetService → DEVICE_INVALIDATED),
/// поэтому весь захват делает нативная DLL.
/// </summary>
public sealed class NativeWasapiCapture : IDisposable
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void McCallback(IntPtr data, int bytes, int rate, int ch);

    internal static class Methods
    {
        [DllImport("mc_wasapi.dll", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mc_start(McCallback cb, int rate, int ch);

        [DllImport("mc_wasapi.dll", CallingConvention = CallingConvention.Cdecl)]
        internal static extern void mc_stop();
    }

    private readonly McCallback _callback;
    private readonly Action<byte[], int, int, int> _onData;
    private bool _started;

    public NativeWasapiCapture(Action<byte[], int, int, int> onData)
    {
        _onData = onData;
        _callback = Handle;
    }

    private void Handle(IntPtr data, int bytes, int rate, int ch)
    {
        if (bytes <= 0)
        {
            return;
        }

        var buf = new byte[bytes];
        Marshal.Copy(data, buf, 0, bytes);
        _onData(buf, bytes, rate, ch);
    }

    public bool TryStart(int rate, int ch)
    {
        _started = Methods.mc_start(_callback, rate, ch) == 0;
        return _started;
    }

    public void Dispose()
    {
        if (_started)
        {
            Methods.mc_stop();
            _started = false;
        }

        GC.KeepAlive(_callback);
    }
}
