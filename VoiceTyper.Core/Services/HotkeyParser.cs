using System.Text;
using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Services;

/// <summary>
/// Синтаксический парсер горячих клавиш: <c>"Ctrl+Alt+Space"</c> ↔ <see cref="HotkeyGesture"/>.
/// Регистронезависим, поддерживает синонимы модификаторов (Control/Ctrl, Win/Windows/Meta).
/// </summary>
public static class HotkeyParser
{
    private static readonly Dictionary<string, HotkeyModifiers> ModifierMap = new(StringComparer.OrdinalIgnoreCase)
    {
        ["alt"] = HotkeyModifiers.Alt,
        ["ctrl"] = HotkeyModifiers.Control,
        ["control"] = HotkeyModifiers.Control,
        ["shift"] = HotkeyModifiers.Shift,
        ["win"] = HotkeyModifiers.Win,
        ["windows"] = HotkeyModifiers.Win,
        ["meta"] = HotkeyModifiers.Win,
        ["super"] = HotkeyModifiers.Win,
        ["cmd"] = HotkeyModifiers.Win,
    };

    /// <summary>Порядок отображения модификаторов в канонической строке.</summary>
    private static readonly (HotkeyModifiers Flag, string Name)[] DisplayOrder =
    {
        (HotkeyModifiers.Control, "Ctrl"),
        (HotkeyModifiers.Alt, "Alt"),
        (HotkeyModifiers.Shift, "Shift"),
        (HotkeyModifiers.Win, "Win"),
    };

    /// <summary>Пытается разобрать строку вида <c>"Ctrl+Alt+Space"</c>.</summary>
    public static bool TryParse(string? text, out HotkeyGesture gesture)
    {
        gesture = default!;
        if (string.IsNullOrWhiteSpace(text))
        {
            return false;
        }

        var parts = text.Split('+', StringSplitOptions.TrimEntries);
        if (parts.Length == 0)
        {
            return false;
        }

        var modifiers = HotkeyModifiers.None;
        var keyName = NormalizeKeyName(parts[^1]);

        // Если ключ сам оказался модификатором ("Ctrl+Alt") — это невалидная жестикуляция.
        if (ModifierMap.ContainsKey(keyName))
        {
            return false;
        }

        for (var i = 0; i < parts.Length - 1; i++)
        {
            if (!ModifierMap.TryGetValue(parts[i], out var mod))
            {
                return false;
            }

            modifiers |= mod;
        }

        if (string.IsNullOrWhiteSpace(keyName))
        {
            return false;
        }

        gesture = new HotkeyGesture(modifiers, keyName);
        return true;
    }

    /// <summary>Разбирает строку; бросает <see cref="ArgumentException"/> при невалидном формате.</summary>
    public static HotkeyGesture Parse(string text) =>
        TryParse(text, out var gesture)
            ? gesture
            : throw new ArgumentException($"Невалидная горячая клавиша: '{text}'", nameof(text));

    /// <summary>Каноническая строка жестикуляцию, например <c>"Ctrl+Alt+Space"</c>.</summary>
    public static string Format(HotkeyGesture gesture)
    {
        var sb = new StringBuilder();
        foreach (var (flag, name) in DisplayOrder)
        {
            if (gesture.Modifiers.HasFlag(flag))
            {
                sb.Append(name);
                sb.Append('+');
            }
        }

        sb.Append(gesture.Key);
        return sb.ToString();
    }

    /// <summary>
    /// Приводит имя клавиши к каноническому виду, совместимому с именами
    /// <c>System.Windows.Input.Key</c>: первая буква в верхнем регистре, остальное — как введено
    /// (<c>"space"</c> → <c>"Space"</c>, <c>"NumPad0"</c> сохраняется).
    /// </summary>
    private static string NormalizeKeyName(string name)
    {
        if (string.IsNullOrEmpty(name))
        {
            return name;
        }

        return char.ToUpperInvariant(name[0]) + name[1..];
    }
}
