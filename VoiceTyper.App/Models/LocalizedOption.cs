using System.ComponentModel;
using VoiceTyper.Core.Localization;

namespace VoiceTyper.App.Models;

/// <summary>
/// Опция выпадающего списка, чьё отображаемое имя берётся из ресурсов по ключу
/// (например <c>Enum_RecordingMode_Vad</c>). Значение — сам enum.
/// Подписан на <see cref="Loc"/>, поэтому при смене языка интерфейса поднимает
/// <see cref="PropertyChanged"/> для <see cref="Label"/>, и выбранное значение
/// обновляется на лету (через <c>DisplayMemberPath="Label"</c>).
/// </summary>
public sealed class LocalizedOption<T> : INotifyPropertyChanged
{
    public LocalizedOption(T value, string key)
    {
        Value = value;
        Key = key;
        Loc.Instance.PropertyChanged += (_, _) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Label)));
    }

    public T Value { get; }

    public string Key { get; }

    public string Label => Loc.Instance[Key];

    public event PropertyChangedEventHandler? PropertyChanged;
}
