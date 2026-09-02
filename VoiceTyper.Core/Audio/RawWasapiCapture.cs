using System.Runtime.InteropServices;

namespace VoiceTyper.Core.Audio;

/// <summary>
/// Прямой WASAPI-захват с инструментированным перебором комбинаций.
/// Пробует несколько вариантов (event/polling, размеры буфера, CLSCTX, роли устройства),
/// логирует HRESULT каждого и использует первый, дающий рабочий IAudioCaptureClient.
/// </summary>
internal sealed class RawWasapiCapture : IDisposable
{
    private const int EventCallbackFlag = 0x40000000;
    private const long HundredMs = 10_000_000; // 100 мс в 100-нс единицах (как в Chromium)

    private readonly string? _deviceId;
    private Thread? _thread;
    private volatile bool _running;

    public RawWasapiCapture(string? deviceId) => _deviceId = deviceId;

    /// <summary>Формат захвата (после успешного старта).</summary>
    public int SampleRate { get; private set; }
    public int Channels { get; private set; }
    public int BitsPerSample { get; private set; }

    /// <summary>
    /// Пытается открыть захват перебором комбинаций. В <paramref name="dataCallback"/>
    /// приходят сырые PCM-байты (формат — см. свойства). Возвращает диагностику (какая комбинация сработала или HRESULT'ы).
    /// </summary>
    public bool TryStart(Action<byte[], int> dataCallback, out string diagnostic)
    {
        diagnostic = string.Empty;
        try
        {
            _running = true;
            var diag = new List<string>();

            foreach (var role in new[] { 0, 1 }) // eConsole, eCommunications
            {
                if (!TryRole(role, dataCallback, diag, out var client))
                {
                    continue;
                }

                diagnostic = $"RAW-WASAPI role={role}: " + diag[^1];
                return true;
            }

            diagnostic = "RAW-WASAPI: " + string.Join(" | ", diag);
            _running = false;
            return false;
        }
        catch
        {
            _running = false;
            return false;
        }
    }

    private bool TryRole(int role, Action<byte[], int> dataCallback, List<string> diag, out RawClient client)
    {
        client = null!;
        try
        {
            var dev = ResolveDevice(role);
            if (dev is null) { diag.Add($"role {role}: нет устройства"); return false; }

            var capIid = typeof(IAudioCaptureClient).GUID;

            // Решение для Intel Smart Sound: EXCLUSIVE + PCM 48k 2ch (shared всегда даёт E_INVALIDARG).
            if (TryExclusive(dev, dataCallback, diag, out client))
            {
                return true;
            }

            // Комбинации: (буфер, period, событийный?)
            (long buf, long period, int flags)[] combos =
            {
                (HundredMs, 0, EventCallbackFlag), // как Chromium
                (100000, 0, EventCallbackFlag),     // ~10 мс
                (0, 0, 0),                          // polling, как cpal fallback
                (HundredMs, 0, 0),
            };

            foreach (var (buf, pe, fl) in combos)
            {
                var audioClient = Activate(dev);
                if (audioClient is null) { diag.Add($"role {role} buf={buf} fl=0x{fl:X}: Activate=null"); continue; }

                IAudioCaptureClient? capture = null;
                try
                {
                    audioClient.GetMixFormat(out var formatPtr);
                    var fmt = Marshal.PtrToStructure<WAVE>(formatPtr);
                    var fmtDesc = $"{fmt.rate}Hz {fmt.ch}ch {fmt.bits}bit tag={fmt.tag}";

                    var initHr = audioClient.Initialize(0, fl, buf, pe, formatPtr, IntPtr.Zero);
                    if (initHr != 0)
                    {
                        diag.Add($"role {role} buf={buf} fl=0x{fl:X} fmt={fmtDesc}: init=0x{initHr:X8}");
                        continue;
                    }

                    int svcHr = audioClient.GetService(ref capIid, out IntPtr capPtr);
                    if (svcHr != 0 || capPtr == IntPtr.Zero)
                    {
                        diag.Add($"role {role} buf={buf} fl=0x{fl:X} fmt={fmtDesc}: getservice=0x{svcHr:X8}");
                        continue;
                    }

                    capture = (IAudioCaptureClient)Marshal.GetTypedObjectForIUnknown(capPtr, typeof(IAudioCaptureClient));
                    SampleRate = (int)fmt.rate;
                    Channels = (int)fmt.ch;
                    BitsPerSample = (int)fmt.bits;
                    diag.Add($"role {role} buf={buf} fl=0x{fl:X}: OK ({fmt.rate}Hz {fmt.ch}ch {fmt.bits}bit)");

                    client = new RawClient(audioClient, capture);
                    _thread = new Thread(() => Loop(capture, dataCallback)) { IsBackground = true };
                    _thread.Start();
                    return true;
                }
                finally
                {
                    if (client is null && capture is null)
                    {
                        Marshal.ReleaseComObject(audioClient);
                    }
                }
            }
        }
        catch (Exception ex)
        {
            diag.Add($"role {role}: {ex.GetType().Name}: {ex.Message}");
        }

        return false;
    }

    /// <summary>
    /// Intel Smart Sound: shared-режим всегда даёт E_INVALIDARG. Рабочий путь — EXCLUSIVE.
    /// Устройство (здесь) принимает только PCM16 48k 2ch. Перебираем несколько PCM-форматов.
    /// </summary>
    private bool TryExclusive(IMMDevice dev, Action<byte[], int> dataCallback, List<string> diag, out RawClient client)
    {
        client = null!;
        var capIid = typeof(IAudioCaptureClient).GUID;

        (uint rate, ushort ch, ushort bits)[] fmts =
        {
            (48000, 2, 16),
            (44100, 2, 16),
            (16000, 1, 16),
            (48000, 1, 16),
            (16000, 2, 16),
        };

        foreach (var (rate, ch, bits) in fmts)
        {
            var audioClient = Activate(dev);
            if (audioClient is null) { diag.Add($"excl {rate}Hz {ch}ch {bits}bit: Activate=null"); continue; }

            IAudioCaptureClient? capture = null;
            IntPtr fmtPtr = IntPtr.Zero;
            try
            {
                fmtPtr = Marshal.AllocHGlobal(18); // sizeof(WAVEFORMATEX)
                var w = new WAVE
                {
                    tag = 1, // WAVE_FORMAT_PCM
                    ch = ch,
                    rate = rate,
                    avg = (uint)(rate * ch * (bits / 8)),
                    align = (ushort)(ch * (bits / 8)),
                    bits = bits,
                    cbsize = 0,
                };
                Marshal.StructureToPtr(w, fmtPtr, false);

                var fmtDesc = $"{rate}Hz {ch}ch {bits}bit PCM";
                // EXCLUSIVE (sharemode=1), polling, буфер 100ms, period 0, session NULL
                var initHr = audioClient.Initialize(1, 0, 1_000_000, 0, fmtPtr, IntPtr.Zero);
                if (initHr != 0)
                {
                    diag.Add($"excl {fmtDesc}: init=0x{initHr:X8}");
                    continue;
                }

                int svcHr = audioClient.GetService(ref capIid, out IntPtr capPtr);
                if (svcHr != 0 || capPtr == IntPtr.Zero)
                {
                    diag.Add($"excl {fmtDesc}: getservice=0x{svcHr:X8}");
                    continue;
                }

                capture = (IAudioCaptureClient)Marshal.GetTypedObjectForIUnknown(capPtr, typeof(IAudioCaptureClient));
                SampleRate = (int)rate;
                Channels = ch;
                BitsPerSample = bits;
                diag.Add($"excl {fmtDesc}: OK");
                client = new RawClient(audioClient, capture);
                _thread = new Thread(() => Loop(capture, dataCallback)) { IsBackground = true };
                _thread.Start();
                return true;
            }
            finally
            {
                if (fmtPtr != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(fmtPtr);
                }

                if (client is null && capture is null)
                {
                    Marshal.ReleaseComObject(audioClient);
                }
            }
        }

        return false;
    }

    private void Loop(IAudioCaptureClient capture, Action<byte[], int> cb)
    {
        int block = Channels * (BitsPerSample / 8);
        try
        {
            while (_running)
            {
                int hres = capture.GetNextPacketSize(out var frames);
                if (frames <= 0)
                {
                    Thread.Sleep(3);
                    continue;
                }

                if (capture.GetBuffer(out var data, out var f, out var fl, out _, out _) != 0)
                {
                    break;
                }

                var bytes = checked(block * (int)f);
                var buf = new byte[bytes];
                Marshal.Copy(data, buf, 0, bytes);
                capture.ReleaseBuffer(f);
                cb(buf, bytes);
            }
        }
        catch
        {
        }
    }

    public void Dispose()
    {
        _running = false;
        _thread?.Join(500);
    }

    /// <summary>
    /// Возвращает endpoint: выбранный <see cref="_deviceId"/>, если задан, иначе дефолтное устройство записи.
    /// </summary>
    private IMMDevice? ResolveDevice(int role)
    {
        var iid = typeof(IMMDeviceEnumerator).GUID;
        var clsid = new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E");
        if (CoCreateInstance(ref clsid, IntPtr.Zero, 1, ref iid, out var en) != 0)
        {
            return null;
        }

        if (!string.IsNullOrEmpty(_deviceId) && _deviceId != "0")
        {
            if (en.GetDevice(_deviceId, out var sel) != 0)
            {
                return null;
            }

            return sel;
        }

        if (en.GetDefaultAudioEndpoint(0, role, out var dev) != 0)
        {
            return null;
        }

        return dev;
    }

    private static IAudioClient? Activate(IMMDevice dev)
    {
        var aci = typeof(IAudioClient).GUID;
        if (dev.Activate(ref aci, 0x17 /*CLSCTX_ALL, как Chromium*/, IntPtr.Zero, out var o) != 0)
        {
            return null;
        }

        return o as IAudioClient;
    }

    [DllImport("ole32.dll")] private static extern int CoCreateInstance(ref Guid clsid, IntPtr outer, int clsCtx, ref Guid iid, out IMMDeviceEnumerator enumerator);
    [DllImport("ole32.dll")] private static extern int CoTaskMemFree(IntPtr p);

    [StructLayout(LayoutKind.Sequential)] private struct WAVE { public ushort tag, ch; public uint rate, avg; public ushort align, bits, cbsize; }

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceEnumerator { [PreserveSig] int EnumAudioEndpoints(int a, int b, out IntPtr c); [PreserveSig] int GetDefaultAudioEndpoint(int a, int b, out IMMDevice d); [PreserveSig] int GetDevice(string id, out IMMDevice d); [PreserveSig] int RegisterEndpointNotificationCallback(IntPtr c); [PreserveSig] int UnregisterEndpointNotificationCallback(IntPtr c); }
    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDevice { [PreserveSig] int Activate(ref Guid iid, int ctx, IntPtr ap, [MarshalAs(UnmanagedType.IUnknown)] out object o); [PreserveSig] int OpenPropertyStore(int a, out IntPtr p); [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id); [PreserveSig] int GetState(out int s); }
    [ComImport, Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IAudioClient
    {
        [PreserveSig] int Initialize(int sm, int fl, long bd, long pe, IntPtr f, IntPtr sessionGuid);
        [PreserveSig] int GetBufferSize(out uint s);
        [PreserveSig] int GetStreamLatency(out long l);
        [PreserveSig] int GetCurrentPadding(out uint pad);
        [PreserveSig] int IsFormatSupported(int sm, IntPtr f, out IntPtr cm);
        [PreserveSig] int GetMixFormat(out IntPtr f);
        [PreserveSig] int GetDevicePeriod(out long d, out long m);
        [PreserveSig] int Start(); [PreserveSig] int Stop(); [PreserveSig] int Reset();
        [PreserveSig] int SetEventHandle(IntPtr h);
        [PreserveSig] int GetService(ref Guid iid, out IntPtr ppv);
    }
    [ComImport, Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IAudioCaptureClient { [PreserveSig] int GetBuffer(out IntPtr d, out uint fr, out uint fl, out long dp, out long qp); [PreserveSig] int ReleaseBuffer(uint fr); [PreserveSig] int GetNextPacketSize(out uint fr); }

    private sealed class RawClient : IDisposable
    {
        private readonly IAudioClient _client;
        private readonly IAudioCaptureClient _capture;
        public RawClient(IAudioClient client, IAudioCaptureClient capture) { _client = client; _capture = capture; }
        public void Dispose() { try { _client.Stop(); } catch { } }
    }
}
