namespace VoiceTyper.Core.Models;

/// <summary>Результат проверки наличия обновления.</summary>
public enum UpdateCheckResultKind
{
    UpToDate,
    UpdateAvailable,
    Failed,
}

/// <summary>Информация о доступном обновлении.</summary>
public sealed record UpdateInfo(
    string Version,
    string? InstallerUrl,
    string? Sha256,
    long? SizeBytes,
    string? ReleaseNotes,
    bool IsPrerelease);

/// <summary>Результат проверки наличия обновления.</summary>
public sealed record UpdateCheckResult(
    UpdateCheckResultKind Kind,
    UpdateInfo? Update,
    string? Error)
{
    /// <summary>Новых версий нет.</summary>
    public bool IsUpToDate => Kind == UpdateCheckResultKind.UpToDate;

    /// <summary>Доступна новая версия.</summary>
    public bool IsAvailable => Kind == UpdateCheckResultKind.UpdateAvailable;

    /// <summary>Проверка завершилась ошибкой.</summary>
    public bool IsFailed => Kind == UpdateCheckResultKind.Failed;
}
