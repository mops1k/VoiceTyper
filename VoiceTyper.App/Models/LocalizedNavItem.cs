using VoiceTyper.Core.Localization;

namespace VoiceTyper.App.Models;

/// <summary>
/// Пункт левой навигации. Хранит стабильный <see cref="Key"/> (используется навигацией
/// и <c>ConverterParameter</c>), глиф Segoe MDL2 и <see cref="Label"/>, который
/// резолвится из ресурсов по ключу <c>Nav_{Key}</c>.
/// </summary>
public sealed class LocalizedNavItem
{
    public LocalizedNavItem(string key, string glyph)
    {
        Key = key;
        Glyph = glyph;
    }

    public string Key { get; }

    public string Glyph { get; }

    public string Label => Loc.Instance["Nav_" + Key];
}
