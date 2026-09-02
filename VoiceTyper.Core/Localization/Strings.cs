using System.Globalization;
using System.Resources;

namespace VoiceTyper.Core.Localization;

/// <summary>
/// Строковые ресурсы приложения. Нейтральный ресурс (без суффикса культуры) — русский,
/// <c>Strings.en.resx</c> — английский. Доступ к значениям — через <see cref="ResourceManager"/>
/// и <see cref="Culture"/> (используется в <see cref="Loc"/>).
/// </summary>
public static class Strings
{
    private static readonly ResourceManager ResourceManagerInstance =
        new("VoiceTyper.Core.Localization.Strings", typeof(Strings).Assembly);

    /// <summary>Менеджер ресурсов, читающий нейтральную (русскую) и <c>en</c> сборки.</summary>
    public static ResourceManager ResourceManager => ResourceManagerInstance;

    /// <summary>Текущая культура для извлечения строк (через <see cref="Loc.Apply"/>).</summary>
    public static CultureInfo Culture { get; set; } = CultureInfo.InvariantCulture;
}
