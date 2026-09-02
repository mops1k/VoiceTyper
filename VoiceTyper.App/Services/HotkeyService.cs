using System.Runtime.InteropServices;
using System.Windows.Input;
using System.Windows.Threading;
using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;

namespace VoiceTyper.App.Services;

/// <summary>
/// Обнаружение отпускания горячей клавиши для режима push-to-talk.
/// RegisterHotKey отдаёт только событие нажатия; отпускание отслеживаем
/// опросом GetAsyncKeyState по виртуальной клавише.
/// </summary>
public static class HotkeyReleaseDetector
{
    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(30);

    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);

    /// <summary>Ждёт, пока главная клавиша хоткея будет отпущена (или отмена).</summary>
    public static Task WaitForKeyRelease(Key key, CancellationToken ct = default)
    {
        var vk = KeyInterop.VirtualKeyFromKey(key);
        return Task.Run(async () =>
        {
            while (!ct.IsCancellationRequested && IsDown(vk))
            {
                await Task.Delay(PollInterval, ct);
            }
        }, ct);
    }

    private static bool IsDown(int vk) => (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/// <summary>Регистрация глобальных хоткеев через NHotkey.Wpf (RegisterHotKey).</summary>
public sealed class HotkeyService
{
    public event Action? RecordPressed;
    public event Action? CancelPressed;

    public Key RecordKey { get; private set; } = Key.Space;
    public Key CancelKey { get; private set; } = Key.Escape;

    /// <summary>
    /// Перерегистрирует хоткеи согласно настройкам. Возвращает список ошибок регистрации
    /// (например, сочетание уже занято другим приложением).
    /// </summary>
    public IReadOnlyList<string> ApplySettings(AppSettings settings)
    {
        var errors = new List<string>();
        var record = Register("Record", settings.RecordHotkey, RecordPressed);
        if (record.HasValue)
        {
            RecordKey = record.Value;
        }
        else
        {
            errors.Add($"Не удалось зарегистрировать хоткей записи '{settings.RecordHotkey}' — возможно, он уже занят.");
        }

        var cancel = Register("Cancel", settings.CancelHotkey, CancelPressed);
        if (cancel.HasValue)
        {
            CancelKey = cancel.Value;
        }
        else
        {
            errors.Add($"Не удалось зарегистрировать хоткей отмены '{settings.CancelHotkey}'.");
        }

        return errors;
    }

    public void UnregisterAll()
    {
        NHotkey.Wpf.HotkeyManager.Current.Remove("Record");
        NHotkey.Wpf.HotkeyManager.Current.Remove("Cancel");
    }

    private static Key? Register(string name, string gestureText, Action? handler)
    {
        NHotkey.Wpf.HotkeyManager.Current.Remove(name);

        if (!HotkeyParser.TryParse(gestureText, out var gesture))
        {
            return null;
        }

        if (!Enum.TryParse<Key>(gesture.Key, ignoreCase: true, out var key))
        {
            return null;
        }

        try
        {
            NHotkey.Wpf.HotkeyManager.Current.AddOrReplace(
                name,
                key,
                ToWpfModifiers(gesture.Modifiers),
                noRepeat: true,
                (_, e) =>
                {
                    e.Handled = true;
                    handler?.Invoke();
                });
            return key;
        }
        catch (Exception)
        {
            // Сочетание занято системой или другим приложением.
            return null;
        }
    }

    private static ModifierKeys ToWpfModifiers(HotkeyModifiers modifiers)
    {
        var result = ModifierKeys.None;
        if (modifiers.HasFlag(HotkeyModifiers.Control))
        {
            result |= ModifierKeys.Control;
        }

        if (modifiers.HasFlag(HotkeyModifiers.Alt))
        {
            result |= ModifierKeys.Alt;
        }

        if (modifiers.HasFlag(HotkeyModifiers.Shift))
        {
            result |= ModifierKeys.Shift;
        }

        if (modifiers.HasFlag(HotkeyModifiers.Win))
        {
            result |= ModifierKeys.Windows;
        }

        return result;
    }
}
