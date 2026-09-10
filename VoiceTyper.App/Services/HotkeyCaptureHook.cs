using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using VoiceTyper.Core.Models;

namespace VoiceTyper.App.Services;

/// <summary>
/// Захват комбинации клавиш через низкоуровневый хук клавиатуры Windows
/// (<c>WH_KEYBOARD_LL</c>). В отличие от обработки <c>KeyDown</c> окна хук
/// глобальный и не зависит от фокуса/активности окна, поэтому корректно ловит
/// комбинации с клавишей Win (сама Win на время захвата подавляется, чтобы
/// не открывалось «Пуск» и не уходил фокус). Работает однократно: после захвата
/// комбинации или отмены хук снимается.
/// </summary>
public sealed class HotkeyCaptureHook : IDisposable
{
    private const int WhKeyboardLl = 13;
    private const int WmKeyDown = 0x0100;
    private const int WmKeyUp = 0x0101;
    private const int WmSysKeyDown = 0x0104;
    private const int WmSysKeyUp = 0x0105;
    private const uint WmQuit = 0x0012;

    private const int VkShift = 0x10;
    private const int VkControl = 0x11;
    private const int VkAlt = 0x12;
    private const int VkEscape = 0x1B;
    private const int VkLShift = 0xA0;
    private const int VkRShift = 0xA1;
    private const int VkLControl = 0xA2;
    private const int VkRControl = 0xA3;
    private const int VkLAlt = 0xA4;
    private const int VkRAlt = 0xA5;
    private const int VkLWin = 0x5B;
    private const int VkRWin = 0x5C;
    private const int VkF1 = 0x70;
    private const int VkF24 = 0x87;

    private readonly ManualResetEventSlim _started = new(false);
    private readonly HashSet<int> _down = new();
    private Thread? _thread;
    private TaskCompletionSource<HotkeyGesture?>? _tcs;
    private Action? _onModifierRequired;
    private HookProc? _proc;
    private IntPtr _hook = IntPtr.Zero;
    private uint _threadId;

    /// <summary>
    /// Захватывает одну комбинацию. Возвращает жест либо <c>null</c> при отмене
    /// (Escape). <paramref name="onModifierRequired"/> вызывается на фоновом потоке,
    /// когда нажата клавиша без модификатора (не F-клавиша).
    /// </summary>
    public Task<HotkeyGesture?> CaptureAsync(Action? onModifierRequired = null)
    {
        Stop();

        _onModifierRequired = onModifierRequired;
        _tcs = new TaskCompletionSource<HotkeyGesture?>(TaskCreationOptions.RunContinuationsAsynchronously);
        _down.Clear();
        _started.Reset();

        _thread = new Thread(Pump)
        {
            IsBackground = true,
            Name = "VoiceTyper.HotkeyCapture",
        };
        _thread.Start();
        _started.Wait();

        return _tcs.Task;
    }

    private void Pump()
    {
        _threadId = GetCurrentThreadId();
        HookProc proc = HookCallback;
        _proc = proc;
        _hook = SetWindowsHookEx(WhKeyboardLl, proc, GetModuleHandle(null), 0);
        _started.Set();

        if (_hook == IntPtr.Zero)
        {
            _tcs?.TrySetResult(null);
            return;
        }

        try
        {
            while (GetMessage(out var msg, IntPtr.Zero, 0, 0))
            {
                TranslateMessage(ref msg);
                DispatchMessage(ref msg);
            }
        }
        finally
        {
            if (_hook != IntPtr.Zero)
            {
                UnhookWindowsHookEx(_hook);
                _hook = IntPtr.Zero;
            }
        }
    }

    private IntPtr HookCallback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode < 0)
        {
            return CallNextHookEx(_hook, nCode, wParam, lParam);
        }

        var message = wParam.ToInt32();
        var isDown = message is WmKeyDown or WmSysKeyDown;
        var isUp = message is WmKeyUp or WmSysKeyUp;
        if (!isDown && !isUp)
        {
            return CallNextHookEx(_hook, nCode, wParam, lParam);
        }

        var info = Marshal.PtrToStructure<KbdLlHookStruct>(lParam);
        var vk = (int)info.vkCode;

        // Отслеживаем состояние по событиям самого хука: GetAsyncKeyState
        // ненадёжен для клавиши Win, которую мы подавляем ниже.
        if (isDown)
        {
            _down.Add(vk);
        }
        else
        {
            _down.Remove(vk);
        }

        // Подавляем клавишу Win: иначе откроется «Пуск» и окно потеряет фокус.
        if (vk is VkLWin or VkRWin)
        {
            return 1;
        }

        if (!isDown || IsModifier(vk))
        {
            return CallNextHookEx(_hook, nCode, wParam, lParam);
        }

        // Escape без модификаторов отменяет захват.
        if (vk == VkEscape && ReadModifiers() == HotkeyModifiers.None)
        {
            Complete(null);
            return 1;
        }

        var mods = ReadModifiers();
        var isFunctionKey = vk is >= VkF1 and <= VkF24;

        if (mods == HotkeyModifiers.None && !isFunctionKey)
        {
            _onModifierRequired?.Invoke();
            return CallNextHookEx(_hook, nCode, wParam, lParam);
        }

        var name = HotkeyService.VirtualKeyToName(vk);
        if (name is null)
        {
            return CallNextHookEx(_hook, nCode, wParam, lParam);
        }

        Complete(new HotkeyGesture(mods, name));
        return 1;
    }

    private void Complete(HotkeyGesture? gesture)
    {
        var tcs = _tcs;
        if (tcs is null || !tcs.TrySetResult(gesture))
        {
            return;
        }

        PostThreadMessage(_threadId, WmQuit, IntPtr.Zero, IntPtr.Zero);
    }

    private static bool IsModifier(int vk) =>
        vk is VkShift or VkControl or VkAlt
            or VkLShift or VkRShift
            or VkLControl or VkRControl
            or VkLAlt or VkRAlt
            or VkLWin or VkRWin;

    private HotkeyModifiers ReadModifiers()
    {
        var mods = HotkeyModifiers.None;
        if (_down.Contains(VkControl) || _down.Contains(VkLControl) || _down.Contains(VkRControl))
        {
            mods |= HotkeyModifiers.Control;
        }

        if (_down.Contains(VkAlt) || _down.Contains(VkLAlt) || _down.Contains(VkRAlt))
        {
            mods |= HotkeyModifiers.Alt;
        }

        if (_down.Contains(VkShift) || _down.Contains(VkLShift) || _down.Contains(VkRShift))
        {
            mods |= HotkeyModifiers.Shift;
        }

        if (_down.Contains(VkLWin) || _down.Contains(VkRWin))
        {
            mods |= HotkeyModifiers.Win;
        }

        return mods;
    }

    /// <summary>Снимает хук и останавливает поток захвата.</summary>
    public void Stop()
    {
        var thread = _thread;
        if (thread is { IsAlive: true })
        {
            if (_threadId != 0)
            {
                PostThreadMessage(_threadId, WmQuit, IntPtr.Zero, IntPtr.Zero);
            }

            thread.Join(500);
        }

        _thread = null;
        _tcs = null;
        _onModifierRequired = null;
    }

    public void Dispose() => Stop();

    private delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct KbdLlHookStruct
    {
        public uint vkCode;
        public uint scanCode;
        public uint flags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Msg
    {
        public IntPtr hwnd;
        public uint message;
        public IntPtr wParam;
        public IntPtr lParam;
        public uint time;
        public int pt_x;
        public int pt_y;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetWindowsHookEx(int idHook, HookProc lpfn, IntPtr hMod, uint dwThreadId);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnhookWindowsHookEx(IntPtr hhk);

    [DllImport("user32.dll")]
    private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetMessage(out Msg lpMsg, IntPtr hWnd, uint wMsgFilterMin, uint wMsgFilterMax);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TranslateMessage(ref Msg lpMsg);

    [DllImport("user32.dll")]
    private static extern IntPtr DispatchMessage(ref Msg lpMsg);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostThreadMessage(uint idThread, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr GetModuleHandle(string? lpModuleName);
}
