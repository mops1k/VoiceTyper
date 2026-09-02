namespace VoiceTyper.Core.Models;

using VoiceTyper.Core.Services;

/// <summary>
/// Модификаторы горячей клавиши. WPF-независимое представление;
/// в слое приложения конвертируется в <c>System.Windows.Input.ModifierKeys</c>.
/// </summary>
[Flags]
public enum HotkeyModifiers
{
    None = 0,
    Alt = 1,
    Control = 2,
    Shift = 4,
    Win = 8,
}

/// <summary>
/// Горячая клавиша в WPF-независимом виде: набор модификаторов + имя клавиши
/// (имя совпадает с именами <c>System.Windows.Input.Key</c>, например <c>Space</c>, <c>F12</c>).
/// </summary>
public sealed record HotkeyGesture(HotkeyModifiers Modifiers, string Key)
{
    public override string ToString() => HotkeyParser.Format(this);
}
