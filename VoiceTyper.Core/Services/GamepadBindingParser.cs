using System.Text;
using VoiceTyper.Core.Localization;
using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Services;

/// <summary>
/// Парсер привязок кнопок геймпада: <c>"XInput|A"</c> / <c>"DInput|Logitech...|3"</c> ↔ <see cref="GamepadBinding"/>.
/// Разделитель — <c>'|'</c>, потому что имя продукта DirectInput может содержать <c>':'</c>.
/// </summary>
public static class GamepadBindingParser
{
    /// <summary>Префикс XInput-привязки.</summary>
    public const string XInputPrefix = "XInput";

    /// <summary>Префикс DirectInput-привязки.</summary>
    public const string DirectInputPrefix = "DInput";

    /// <summary>Пытается разобрать строку привязки геймпада.</summary>
    public static bool TryParse(string? text, out GamepadBinding binding)
    {
        binding = default!;
        if (string.IsNullOrWhiteSpace(text))
        {
            return false;
        }

        var parts = text.Split('|', StringSplitOptions.TrimEntries);
        if (parts.Length < 2)
        {
            return false;
        }

        var source = ParseSource(parts[0]);
        if (source == GamepadSource.None)
        {
            return false;
        }

        if (source == GamepadSource.XInput)
        {
            if (parts.Length != 2 || !IsValidXInputButton(parts[1]))
            {
                return false;
            }

            binding = new GamepadBinding(source, Enum.Parse<XInputPadButton>(parts[1], ignoreCase: true).ToString());
            return true;
        }

        // DirectInput: <productName>|<index>
        if (parts.Length != 3 || !int.TryParse(parts[2], out var index) || index < 0)
        {
            return false;
        }

        binding = new GamepadBinding(source, $"{parts[1]}|{index}");
        return true;
    }

    /// <summary>Разбирает строку; бросает <see cref="ArgumentException"/> при невалидном формате.</summary>
    public static GamepadBinding Parse(string text) =>
        TryParse(text, out var binding)
            ? binding
            : throw new ArgumentException($"Невалидная привязка геймпада: '{text}'", nameof(text));

    /// <summary>Каноническая строка привязки, например <c>"XInput|A"</c> или <c>"DInput|Logitech|3"</c>.</summary>
    public static string Format(GamepadBinding binding) => binding.Source switch
    {
        GamepadSource.XInput => $"{XInputPrefix}|{binding.ButtonId}",
        GamepadSource.DirectInput => $"{DirectInputPrefix}|{binding.ButtonId}",
        _ => string.Empty,
    };

    /// <summary>
    /// Человекочитаемое отображение привязки для UI: XInput → <c>A</c>, <c>LB</c>…,
    /// DirectInput → <c>&lt;productName&gt;: Кнопка &lt;index+1&gt;</c>.
    /// </summary>
    public static string ToDisplayString(GamepadBinding? binding) => binding?.Source switch
    {
        GamepadSource.XInput => binding!.ButtonId,
        GamepadSource.DirectInput => ToDirectInputDisplay(binding!.ButtonId),
        _ => string.Empty,
    };

    /// <summary>Создаёт XInput-привязку из кнопки.</summary>
    public static GamepadBinding FromXInput(XInputPadButton button) =>
        new(GamepadSource.XInput, button.ToString());

    /// <summary>Создаёт DirectInput-привязку из имени продукта и индекса кнопки.</summary>
    public static GamepadBinding FromDirectInput(string productName, int buttonIndex) =>
        new(GamepadSource.DirectInput, $"{productName}|{buttonIndex}");

    private static GamepadSource ParseSource(string value) =>
        value.Equals(XInputPrefix, StringComparison.OrdinalIgnoreCase)
            ? GamepadSource.XInput
            : value.Equals(DirectInputPrefix, StringComparison.OrdinalIgnoreCase)
                ? GamepadSource.DirectInput
                : GamepadSource.None;

    private static bool IsValidXInputButton(string name) =>
        Enum.TryParse<XInputPadButton>(name, ignoreCase: true, out var result)
        && Enum.IsDefined(typeof(XInputPadButton), result);

    private static string ToDirectInputDisplay(string buttonId)
    {
        var parts = buttonId.Split('|', 2);
        if (parts.Length != 2 || !int.TryParse(parts[1], out var index))
        {
            return buttonId;
        }

        var label = Loc.T("Gamepad_DirectInputButton");
        return $"{parts[0]}: {string.Format(label, index + 1)}";
    }
}
