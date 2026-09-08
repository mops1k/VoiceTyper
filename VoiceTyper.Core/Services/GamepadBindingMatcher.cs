using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Services;

/// <summary>
/// Сопоставление нажатия геймпада с привязанной кнопкой. Сравнение идёт по
/// <see cref="GamepadSource"/> и идентификатору кнопки (без учёта регистра).
/// </summary>
public static class GamepadBindingMatcher
{
    /// <summary>Совпадает ли нажатие <paramref name="input"/> с привязкой <paramref name="binding"/>.</summary>
    public static bool Matches(GamepadBinding? binding, GamepadInput input)
    {
        if (binding is null || input is null || binding.Source == GamepadSource.None)
        {
            return false;
        }

        return binding.Source == input.Source
               && string.Equals(binding.ButtonId, input.ButtonId, StringComparison.OrdinalIgnoreCase);
    }
}
