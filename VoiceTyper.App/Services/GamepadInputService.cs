using System.Runtime.InteropServices;
using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;
using Vortice.DirectInput;
using Vortice.XInput;

namespace VoiceTyper.App.Services;

/// <summary>
/// Мониторинг кнопок геймпада/контроллера через XInput и DirectInput.
/// Опрос идёт на выделенном фоновом потоке (DirectInput/COM-устройства потокозависимы),
/// события <see cref="RecordPressed"/>/<see cref="RecordReleased"/>/<see cref="CancelPressed"/>
/// подаются при смене состояния привязанных кнопок. Также поддерживает захват кнопки.
/// </summary>
public sealed class GamepadInputService : IDisposable
{
    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(33);

    // Порог нажатия аналоговых триггеров XInput (0..255).
    private const int TriggerPressThreshold = 30;

    private Thread? _thread;
    private volatile bool _running;
    private bool _disposed;

    private IDirectInput8? _directInput;
    private IntPtr _hiddenWindow;
    private readonly List<JoystickDevice> _joysticks = new();

    private GamepadBinding? _recordBinding;
    private GamepadBinding? _cancelBinding;
    private bool _recordDown;
    private bool _cancelDown;

    private TaskCompletionSource<GamepadBinding?>? _captureTcs;

    /// <summary>Нажата кнопка записи (возникающий фронт).</summary>
    public event Action? RecordPressed;

    /// <summary>Кнопка записи отпущена.</summary>
    public event Action? RecordReleased;

    /// <summary>Нажата кнопка отмены (возникающий фронт).</summary>
    public event Action? CancelPressed;

    /// <summary>Прикладной разбор привязок из настроек и установка их в службу.</summary>
    public void ApplySettings(AppSettings settings)
    {
        _recordBinding = Parse(settings.RecordGamepadButton);
        _cancelBinding = Parse(settings.CancelGamepadButton);
    }

    /// <summary>Начинает захват: ожидает нажатия любой кнопки геймпада и возвращает привязку.</summary>
    public Task<GamepadBinding?> StartCapture()
    {
        _captureTcs = new TaskCompletionSource<GamepadBinding?>();
        EnsureStarted();
        return _captureTcs.Task;
    }

    /// <summary>Отменяет захват (например, Escape при потере фокуса).</summary>
    public void CancelCapture() => _captureTcs?.TrySetResult(null);

    /// <summary>Запускает фоновый поток опроса, если он ещё не запущен.</summary>
    public void Start()
    {
        if (_running)
        {
            return;
        }

        _running = true;
        _thread = new Thread(PollLoop)
        {
            IsBackground = true,
            Name = "VoiceTyper.GamepadInput",
        };
        _thread.Start();
    }

    /// <summary>Ждёт, пока кнопка записи не будет отпущена (для режима push-to-talk).</summary>
    public async Task WaitForRecordReleaseAsync(CancellationToken ct = default)
    {
        while (!ct.IsCancellationRequested && _recordDown)
        {
            await Task.Delay(PollInterval, ct).ConfigureAwait(false);
        }
    }

    private void EnsureStarted() => Start();

    private static GamepadBinding? Parse(string? text) =>
        GamepadBindingParser.TryParse(text, out var binding) ? binding : null;

    private void PollLoop()
    {
        _hiddenWindow = CreateMessageWindow();
        try
        {
            _directInput = DInput.DirectInput8Create();
            ReloadJoysticks();

            while (_running)
            {
                try
                {
                    var inputs = ReadAllInputs();
                    Process(inputs);
                }
                catch (Exception)
                {
                    // Потеря устройства или внутренняя ошибка — пересоздаём набор джойстиков.
                    ReloadJoysticks();
                }

                Thread.Sleep(PollInterval);
            }
        }
        finally
        {
            UnacquireJoysticks();
            _directInput?.Dispose();
            _directInput = null;
            if (_hiddenWindow != IntPtr.Zero)
            {
                DestroyWindow(_hiddenWindow);
                _hiddenWindow = IntPtr.Zero;
            }
        }
    }

    /// <summary>Собирает все кнопки, нажатые в текущий момент (XInput + DirectInput).</summary>
    private List<GamepadInput> ReadAllInputs()
    {
        var result = new List<GamepadInput>();
        ReadXInput(result);
        ReadDirectInput(result);
        return result;
    }

    private void ReadXInput(List<GamepadInput> result)
    {
        // Обычно один игрок; для полноты пробуем порты 0..3, но привязка работает на любом.
        for (uint userIndex = 0; userIndex < 4; userIndex++)
        {
            if (!XInput.GetState(userIndex, out var state))
            {
                continue;
            }

            AddXInputButtons(result, state.Gamepad);
        }
    }

    private static void AddXInputButtons(List<GamepadInput> result, Gamepad gamepad)
    {
        if (gamepad.LeftTrigger >= TriggerPressThreshold)
        {
            result.Add(new GamepadInput(GamepadSource.XInput, XInputPadButton.LT.ToString()));
        }

        if (gamepad.RightTrigger >= TriggerPressThreshold)
        {
            result.Add(new GamepadInput(GamepadSource.XInput, XInputPadButton.RT.ToString()));
        }

        foreach (var button in EnumerateXInputButtons(gamepad.Buttons))
        {
            result.Add(new GamepadInput(GamepadSource.XInput, button.ToString()));
        }
    }

    private static IEnumerable<XInputPadButton> EnumerateXInputButtons(GamepadButtons buttons)
    {
        if (buttons.HasFlag(GamepadButtons.A))
        {
            yield return XInputPadButton.A;
        }

        if (buttons.HasFlag(GamepadButtons.B))
        {
            yield return XInputPadButton.B;
        }

        if (buttons.HasFlag(GamepadButtons.X))
        {
            yield return XInputPadButton.X;
        }

        if (buttons.HasFlag(GamepadButtons.Y))
        {
            yield return XInputPadButton.Y;
        }

        if (buttons.HasFlag(GamepadButtons.LeftShoulder))
        {
            yield return XInputPadButton.LB;
        }

        if (buttons.HasFlag(GamepadButtons.RightShoulder))
        {
            yield return XInputPadButton.RB;
        }

        if (buttons.HasFlag(GamepadButtons.DPadUp))
        {
            yield return XInputPadButton.DPadUp;
        }

        if (buttons.HasFlag(GamepadButtons.DPadDown))
        {
            yield return XInputPadButton.DPadDown;
        }

        if (buttons.HasFlag(GamepadButtons.DPadLeft))
        {
            yield return XInputPadButton.DPadLeft;
        }

        if (buttons.HasFlag(GamepadButtons.DPadRight))
        {
            yield return XInputPadButton.DPadRight;
        }

        if (buttons.HasFlag(GamepadButtons.Start))
        {
            yield return XInputPadButton.Start;
        }

        if (buttons.HasFlag(GamepadButtons.Back))
        {
            yield return XInputPadButton.Back;
        }

        if (buttons.HasFlag(GamepadButtons.LeftThumb))
        {
            yield return XInputPadButton.LeftStick;
        }

        if (buttons.HasFlag(GamepadButtons.RightThumb))
        {
            yield return XInputPadButton.RightStick;
        }

        // Служебная кнопка Guide (Xbox) в наборе привязок отсутствует — намеренно пропускаем.
    }

    private void ReadDirectInput(List<GamepadInput> result)
    {
        foreach (var joystick in _joysticks)
        {
            var device = joystick.Device;
            device.Poll();

            JoystickState state;
            try
            {
                state = device.GetCurrentJoystickState();
            }
            catch (InvalidOperationException)
            {
                continue;
            }

            var buttons = state.Buttons;
            for (var index = 0; index < buttons.Length; index++)
            {
                if (buttons[index])
                {
                    result.Add(new GamepadInput(GamepadSource.DirectInput, $"{joystick.ProductName}|{index}"));
                }
            }
        }
    }

    /// <summary>Обрабатывает нажатые кнопки: либо захват, либо триггеры запись/отмена.</summary>
    private void Process(List<GamepadInput> inputs)
    {
        if (_captureTcs is not null)
        {
            var captured = inputs.Count > 0 ? new GamepadBinding(inputs[0].Source, inputs[0].ButtonId) : null;
            var tcs = _captureTcs;
            _captureTcs = null;
            tcs?.TrySetResult(captured);
            return;
        }

        var record = inputs.Any(i => GamepadBindingMatcher.Matches(_recordBinding, i));
        var cancel = inputs.Any(i => GamepadBindingMatcher.Matches(_cancelBinding, i));

        if (record && !_recordDown)
        {
            RecordPressed?.Invoke();
        }

        if (!record && _recordDown)
        {
            RecordReleased?.Invoke();
        }

        if (cancel && !_cancelDown)
        {
            CancelPressed?.Invoke();
        }

        _recordDown = record;
        _cancelDown = cancel;
    }

    /// <summary>Пересоздаёт список DirectInput-джойстиков (при старте и при потере устройства).</summary>
    private void ReloadJoysticks()
    {
        UnacquireJoysticks();

        if (_directInput is null)
        {
            return;
        }

        try
        {
            var instances = _directInput.GetDevices(DeviceClass.GameControl, DeviceEnumerationFlags.AttachedOnly);
            foreach (var instance in instances)
            {
                var device = _directInput.CreateDevice(instance.InstanceGuid);
                device.SetDataFormat<RawJoystickState>();
                device.SetCooperativeLevel(_hiddenWindow,
                    CooperativeLevel.NonExclusive | CooperativeLevel.Background);

                if (device.Acquire() != Vortice.DirectInput.ResultCode.Ok)
                {
                    continue;
                }

                _joysticks.Add(new JoystickDevice(device, instance.ProductName));
            }
        }
        catch (Exception)
        {
            // Не удалось перечислить/захватить джойстики — остаёмся с пустым списком.
            UnacquireJoysticks();
        }
    }

    private void UnacquireJoysticks()
    {
        foreach (var joystick in _joysticks)
        {
            try
            {
                joystick.Device.Unacquire();
                joystick.Device.Dispose();
            }
            catch (Exception)
            {
                // уже отключено
            }
        }

        _joysticks.Clear();
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _running = false;
        _captureTcs?.TrySetResult(null);
        _thread?.Join(1000);
        _thread = null;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateWindowExW(
        uint exStyle, string className, string windowName, uint style,
        int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);

    [DllImport("user32.dll")]
    private static extern bool DestroyWindow(IntPtr hwnd);

    private static IntPtr CreateMessageWindow() =>
        CreateWindowExW(0, "STATIC", "VoiceTyper_GamepadInput", 0, 0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);

    private sealed record JoystickDevice(IDirectInputDevice8 Device, string ProductName);
}
