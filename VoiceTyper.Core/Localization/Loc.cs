using System.ComponentModel;
using System.Globalization;
using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Localization;

/// <summary>
/// Рантайм-менеджер локализации. Синглтон, поддерживает живое переключение языка:
/// <see cref="Apply"/> меняет культуру, распространяет её на потоки по умолчанию
/// и поднимает <see cref="PropertyChanged"/> (со всеми ключами), чтобы все
/// <c>{Binding [key], Source={x:Static loc:Loc.Instance}}</c> обновились на лету.
/// </summary>
public sealed class Loc : INotifyPropertyChanged
{
    public static Loc Instance { get; } = new();

    private CultureInfo _culture = new("ru");

    private Loc()
    {
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    /// <summary>Текущая культура интерфейса.</summary>
    public CultureInfo Culture => _culture;

    /// <summary>Возвращает строку по ключу; при отсутствии — сам ключ (для отлова пропусков).</summary>
    public string this[string key] => GetString(key);

    /// <summary>Статический доступ к строке по ключу (удобно из кода и логов).</summary>
    public static string T(string key) => Instance[key];

    /// <summary>Статический доступ к строке по ключу с форматированием.</summary>
    public static string Format(string key, params object?[] args) => string.Format(Instance[key], args);

    /// <summary>Применяет язык интерфейса и уведомляет все привязки.</summary>
    public void Apply(AppLanguage language)
    {
        _culture = language == AppLanguage.En ? new CultureInfo("en") : new CultureInfo("ru");
        Strings.Culture = _culture;

        Thread.CurrentThread.CurrentCulture = _culture;
        Thread.CurrentThread.CurrentUICulture = _culture;
        CultureInfo.DefaultThreadCurrentCulture = _culture;
        CultureInfo.DefaultThreadCurrentUICulture = _culture;

        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(string.Empty));
    }

    private string GetString(string key) =>
        Strings.ResourceManager.GetString(key, _culture) ?? key;
}
