using System.Runtime.InteropServices;
using VoiceTyper.Core.Localization;
using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;

namespace VoiceTyper.App.Services;

/// <summary>
/// Регистрация глобальных хоткеев через Win32 RegisterHotKey (без WPF).
/// На фоновом потоке выполняется цикл сообщений; RegisterHotKey с hWnd=0
/// привязывается к вызывающему потоку, поэтому регистрации выполняются
/// на этом же фоновом потоке. События нажатий приходят на фоновом потоке.
/// </summary>
public sealed class HotkeyService : IHotkeyService
{
    private const int WmHotkey = 0x0312;
    private const uint WmQuit = 0x0012;
    private const uint ModAlt = 0x0001;
    private const uint ModControl = 0x0002;
    private const uint ModShift = 0x0004;
    private const uint ModWin = 0x0008;
    private const int RecordId = 1;
    private const int CancelId = 2;

    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(30);
    private static readonly TimeSpan PumpIdleDelay = TimeSpan.FromMilliseconds(25);

    private readonly object _gate = new();
    private readonly Queue<Action> _actions = new();
    private readonly Dictionary<int, Action> _handlers = new();
    private readonly List<int> _registered = new();
    private readonly AutoResetEvent _wake = new(false);
    private readonly Thread _thread;
    private volatile bool _running = true;

    public HotkeyService()
    {
        _thread = new Thread(PumpLoop)
        {
            IsBackground = true,
            Name = "VoiceTyper.Hotkeys",
        };
        _thread.Start();
    }

    public event Action? RecordPressed;
    public event Action? CancelPressed;

    /// <summary>Виртуальный код главной клавиши хоткея записи (0 — не зарегистрирован).</summary>
    public int RecordKeyVk { get; private set; }

    public IReadOnlyList<string> ApplySettings(AppSettings settings)
    {
        UnregisterAll();
        var errors = new List<string>();

        var recordOk = Register(RecordId, settings.RecordHotkey, RecordPressed);
        RecordKeyVk = recordOk ? ToVirtualKey(HotkeyParser.Parse(settings.RecordHotkey).Key) : 0;
        if (!recordOk)
        {
            errors.Add(Loc.Format("Hotkeys_RegistrationErrorRecord", settings.RecordHotkey));
        }

        var cancelOk = Register(CancelId, settings.CancelHotkey, CancelPressed);
        if (!cancelOk)
        {
            errors.Add(Loc.Format("Hotkeys_RegistrationErrorCancel", settings.CancelHotkey));
        }

        return errors;
    }

    public void UnregisterAll() => RunOnPump(() =>
    {
        foreach (var id in _registered)
        {
            _ = UnregisterHotKey(IntPtr.Zero, id);
        }

        _registered.Clear();
        _handlers.Clear();
    });

    public void Dispose()
    {
        UnregisterAll();
        _running = false;
        _wake.Set();
    }
    private bool Register(int id, string gestureText, Action? handler)
    {
        if (!HotkeyParser.TryParse(gestureText, out var gesture))
        {
            return false;
        }

        var vk = ToVirtualKey(gesture.Key);
        if (vk == 0)
        {
            return false;
        }

        var mods = ToModifiers(gesture.Modifiers);

        var ok = RunOnPump(() =>
        {
            _ = UnregisterHotKey(IntPtr.Zero, id);
            _registered.Remove(id);
            var success = RegisterHotKey(IntPtr.Zero, id, mods, vk);
            if (success)
            {
                _registered.Add(id);
                _handlers[id] = handler ?? (() => { });
            }
            else
            {
                _handlers.Remove(id);
            }

            return success;
        });

        return ok;
    }

    /// <summary>Выполняет действие на потоке цикла сообщений (без результата).</summary>
    private void RunOnPump(Action action) => RunOnPump(() =>
    {
        action();
        return true;
    });

    /// <summary>Выполняет действие на потоке цикла сообщений и ждёт результат.</summary>
    private bool RunOnPump(Func<bool> action)
    {
        if (Thread.CurrentThread == _thread)
        {
            return action();
        }

        var done = new ManualResetEventSlim(false);
        var result = false;

        lock (_gate)
        {
            _actions.Enqueue(() =>
            {
                try
                {
                    result = action();
                }
                finally
                {
                    done.Set();
                }
            });
        }

        _wake.Set();
        done.Wait(TimeSpan.FromSeconds(3));
        return result;
    }

    private void PumpLoop()
    {
        while (_running)
        {
            // Выполняем накопленные действия (регистрации) на этом потоке.
            while (true)
            {
                Action? action = null;
                lock (_gate)
                {
                    if (_actions.Count > 0)
                    {
                        action = _actions.Dequeue();
                    }
                }

                if (action is null)
                {
                    break;
                }

                action();
            }

            // Разбираем очередь сообщений текущего потока.
            while (PeekMessage(out var msg, IntPtr.Zero, 0, 0, 1 /* PM_REMOVE */))
            {
                if (msg.message == WmHotkey)
                {
                    var id = msg.wParam.ToInt32();
                    Action? handler;
                    lock (_gate)
                    {
                        _handlers.TryGetValue(id, out handler);
                    }

                    handler?.Invoke();
                }
                else if (msg.message == WmQuit)
                {
                    return;
                }
            }

            _wake.WaitOne(PumpIdleDelay);
        }
    }

    /// <summary>Имя клавиши (стиль Key enum, из HotkeyParser) → виртуальный код Windows.</summary>
    public static int ToVirtualKey(string keyName)
    {
        if (string.IsNullOrEmpty(keyName))
        {
            return 0;
        }

        if (keyName.Length == 1)
        {
            var c = keyName[0];
            return c is >= 'A' and <= 'Z' or >= '0' and <= '9' ? c : 0;
        }

        return keyName switch
        {
            "Space" => 0x20,
            "Enter" => 0x0D,
            "Escape" => 0x1B,
            "Tab" => 0x09,
            "Back" => 0x08,
            "Insert" => 0x2D,
            "Delete" => 0x2E,
            "Home" => 0x24,
            "End" => 0x23,
            "PageUp" => 0x21,
            "PageDown" => 0x22,
            "Left" => 0x25,
            "Up" => 0x26,
            "Right" => 0x27,
            "Down" => 0x28,
            "PrintScreen" => 0x2C,
            "Scroll" => 0x91,
            "Pause" => 0x13,
            "CapsLock" => 0x14,
            "NumLock" => 0x90,
            _ => FunctionKey(keyName) ?? NumpadKey(keyName) ?? DigitKey(keyName) ?? OemKey(keyName) ?? 0,
        };
    }

    private static int? FunctionKey(string key) =>
        key.Length >= 2 && key[0] == 'F' && int.TryParse(key[1..], out var n) && n is >= 1 and <= 24
            ? 0x70 + n - 1
            : null;

    private static int? NumpadKey(string key) =>
        key.StartsWith("NumPad", StringComparison.Ordinal) && int.TryParse(key[6..], out var n) && n is >= 0 and <= 9
            ? 0x60 + n
            : null;

    private static int? DigitKey(string key) =>
        key.Length == 2 && key[0] == 'D' && int.TryParse(key[1..], out var n) && n is >= 0 and <= 9
            ? (n == 0 ? 0x30 : 0x30 + n)
            : null;

    private static int? OemKey(string key) => key switch
    {
        "OemPlus" => 0xBB,
        "OemMinus" => 0xBD,
        "OemComma" => 0xBC,
        "OemPeriod" => 0xBE,
        "OemQuestion" => 0xBF,
        "OemSemicolon" => 0xBA,
        "OemQuotes" => 0xDE,
        "OemOpenBrackets" => 0xDB,
        "OemCloseBrackets" => 0xDD,
        "OemPipe" => 0xDC,
        "OemTilde" => 0xC0,
        _ => null,
    };

    private static uint ToModifiers(HotkeyModifiers modifiers)
    {
        uint result = 0;
        if (modifiers.HasFlag(HotkeyModifiers.Control))
        {
            result |= ModControl;
        }

        if (modifiers.HasFlag(HotkeyModifiers.Alt))
        {
            result |= ModAlt;
        }

        if (modifiers.HasFlag(HotkeyModifiers.Shift))
        {
            result |= ModShift;
        }

        if (modifiers.HasFlag(HotkeyModifiers.Win))
        {
            result |= ModWin;
        }

        return result;
    }

    /// <summary>Ждёт, пока виртуальная клавиша будет отпущена (режим push-to-talk).</summary>
    public static Task WaitForKeyRelease(int vk, CancellationToken ct = default) =>
        Task.Run(async () =>
        {
            while (!ct.IsCancellationRequested && IsDown(vk))
            {
                await Task.Delay(PollInterval, ct);
            }
        }, ct);

    private static bool IsDown(int vk) => (GetAsyncKeyState(vk) & 0x8000) != 0;

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeMessage
    {
        public IntPtr hwnd;
        public uint message;
        public IntPtr wParam;
        public IntPtr lParam;
        public uint time;
        public int pt_x;
        public int pt_y;
    }

    [DllImport("user32.dll")]
    private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, int vk);

    [DllImport("user32.dll")]
    private static extern bool UnregisterHotKey(IntPtr hWnd, int id);

    [DllImport("user32.dll")]
    private static extern bool PeekMessage(out NativeMessage lpMsg, IntPtr hWnd, uint wMsgFilterMin, uint wMsgFilterMax, uint wRemoveMsg);

    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);
}
