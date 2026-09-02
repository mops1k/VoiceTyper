using System.ComponentModel;
using VoiceTyper.Core.Localization;

namespace VoiceTyper.App.Models;

/// <summary>
/// Пункт левой навигации. Хранит стабильный <see cref="Key"/> (используется навигацией
/// и <c>ConverterParameter</c>), глиф Segoe MDL2 и <see cref="Label"/>, который
/// резолвится из ресурсов по ключу <c>Nav_{Key}</c>.
/// Подписан на <see cref="Loc"/>, поэтому при смене языка интерфейса поднимает
/// <see cref="PropertyChanged"/> для <see cref="Label"/>, и текст пункта обновляется на лету.
/// </summary>
public sealed class LocalizedNavItem : INotifyPropertyChanged
{
    public LocalizedNavItem(string key, string glyph)
    {
        Key = key;
        Glyph = glyph;
        Loc.Instance.PropertyChanged += (_, _) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Label)));
    }

    public string Key { get; }

    public string Glyph { get; }

    public string Label => Loc.Instance["Nav_" + Key];

    public event PropertyChangedEventHandler? PropertyChanged;
}
