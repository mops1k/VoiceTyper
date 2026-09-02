using VoiceTyper.Core.Localization;

namespace VoiceTyper.App.Models;

/// <summary>
/// Опция выпадающего списка, чьё отображаемое имя берётся из ресурсов по ключу
/// (например <c>Enum_RecordingMode_Vad</c>). Значение — сам enum.
/// </summary>
public readonly record struct LocalizedOption<T>(T Value, string Key)
{
    public string Label => Loc.Instance[Key];
}
