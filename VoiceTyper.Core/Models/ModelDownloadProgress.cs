namespace VoiceTyper.Core.Models;

/// <summary>Прогресс загрузки модели.</summary>
public sealed record ModelDownloadProgress(long BytesDownloaded, long TotalBytes, double BytesPerSecond)
{
    /// <summary>Доля загрузки 0..1.</summary>
    public double Fraction => TotalBytes > 0 ? Math.Min(1.0, (double)BytesDownloaded / TotalBytes) : 0;

    /// <summary>Оставшееся время (null, если данных недостаточно).</summary>
    public TimeSpan? Remaining =>
        TotalBytes > 0 && BytesDownloaded < TotalBytes && BytesPerSecond > 0
            ? TimeSpan.FromSeconds((TotalBytes - BytesDownloaded) / BytesPerSecond)
            : null;
}
