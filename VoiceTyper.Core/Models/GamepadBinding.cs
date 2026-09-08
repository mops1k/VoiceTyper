namespace VoiceTyper.Core.Models;

using VoiceTyper.Core.Services;

/// <summary>Источник устройства ввода (геймпад/контроллер).</summary>
public enum GamepadSource
{
    /// <summary>Не задано.</summary>
    None,

    /// <summary>Стандартный контроллер, совместимый с Xbox (XInput).</summary>
    XInput,

    /// <summary>Произвольный джойстик/геймпад через DirectInput.</summary>
    DirectInput,
}

/// <summary>
/// Фиксированные кнопки XInput-контроллера. Имена используются в строковом виде
/// в настройках (<c>XInput|A</c>) и при сравнении нажатий.
/// </summary>
public enum XInputPadButton
{
    /// <summary>Кнопка A (крестовик, нижняя).</summary>
    A,

    /// <summary>Кнопка B (правая).</summary>
    B,

    /// <summary>Кнопка X (левая).</summary>
    X,

    /// <summary>Кнопка Y (верхняя).</summary>
    Y,

    /// <summary>Левый бампер (LB).</summary>
    LB,

    /// <summary>Правый бампер (RB).</summary>
    RB,

    /// <summary>Левый триггер (LT).</summary>
    LT,

    /// <summary>Правый триггер (RT).</summary>
    RT,

    /// <summary>Крестовина, вверх.</summary>
    DPadUp,

    /// <summary>Крестовина, вниз.</summary>
    DPadDown,

    /// <summary>Крестовина, влево.</summary>
    DPadLeft,

    /// <summary>Крестовина, вправо.</summary>
    DPadRight,

    /// <summary>Кнопка Start (Меню).</summary>
    Start,

    /// <summary>Кнопка Back (Вид).</summary>
    Back,

    /// <summary>Кнопка-щелчок левого стика.</summary>
    LeftStick,

    /// <summary>Кнопка-щелчок правого стика.</summary>
    RightStick,
}

/// <summary>Событие нажатия кнопки геймпада: источник + идентификатор кнопки.</summary>
public sealed record GamepadInput(GamepadSource Source, string ButtonId);

/// <summary>
/// Привязка кнопки геймпада к действию в WPF-независимом виде.
/// <see cref="ButtonId"/>: для XInput — имя кнопки (<c>A</c>, <c>LB</c>, …),
/// для DirectInput — <c>&lt;productName&gt;|&lt;index&gt;</c>.
/// </summary>
public sealed record GamepadBinding(GamepadSource Source, string ButtonId)
{
    public override string ToString() => GamepadBindingParser.Format(this);
}
