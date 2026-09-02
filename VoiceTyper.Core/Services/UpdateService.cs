using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;
using VoiceTyper.Core.Models;

namespace VoiceTyper.Core.Services;

/// <summary>Сервис проверки наличия обновлений и загрузки установщика.</summary>
public interface IUpdateService
{
    /// <summary>
    /// Проверяет наличие новой версии через GitHub Releases API.
    /// Не бросает исключений на сетевых/HTTP-ошибках — возвращает <see cref="UpdateCheckResult"/> с видом Failed.
    /// </summary>
    Task<UpdateCheckResult> CheckForUpdateAsync(string currentVersion, CancellationToken ct = default);

    /// <summary>
    /// Скачивает установщик во временный файл, проверяет SHA-256 (если задан) и
    /// атомарно переносит его в целевой каталог. Возвращает путь к установщику.
    /// </summary>
    Task<string> DownloadInstallerAsync(UpdateInfo update, IProgress<double>? progress = null, CancellationToken ct = default);
}

/// <summary>
/// Проверяет обновления в GitHub-репозитории <c>mops1k/VoiceTyper</c> (публичные релизы)
/// и скачивает установщик в <c>%LOCALAPPDATA%\VoiceTyper\updates</c>.
/// </summary>
public sealed class GithubUpdateService : IUpdateService
{
    public const string Repository = "mops1k/VoiceTyper";
    public const string BaseUrl = "https://api.github.com/repos/" + Repository;

    /// <summary>Имя ассета-установщика, должно совпадать с <c>OutputBaseFilename</c> в installer.iss.</summary>
    public static readonly string SetupAssetRegex = @"^VoiceTyper-\d[^/]*?-Setup\.exe$";

    private readonly HttpClient _http;
    private readonly string _updatesDirectory;

    public GithubUpdateService(HttpClient? http = null, string? updatesDirectory = null)
    {
        _http = http ?? new HttpClient { Timeout = TimeSpan.FromMinutes(30) };
        _updatesDirectory = updatesDirectory
                            ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                "VoiceTyper",
                                "updates");
    }

    public string UpdatesDirectory => _updatesDirectory;

    public async Task<UpdateCheckResult> CheckForUpdateAsync(string currentVersion, CancellationToken ct = default)
    {
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, BaseUrl + "/releases/latest");
            request.Headers.UserAgent.Add(new ProductInfoHeaderValue("VoiceTyper", "1.0"));
            request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));

            using var response = await _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct);

            if (response.StatusCode == HttpStatusCode.NotFound)
            {
                return Failed("Релиз не найден. Репозиторий недоступен, или он приватный.");
            }

            if (response.StatusCode == HttpStatusCode.Forbidden)
            {
                return Failed("Доступ к GitHub запрещён (403).");
            }

            if (response.StatusCode == HttpStatusCode.TooManyRequests)
            {
                return Failed("Превышен лимит запросов к GitHub.");
            }

            response.EnsureSuccessStatusCode();

            await using var stream = await response.Content.ReadAsStreamAsync(ct);
            using var doc = await JsonDocument.ParseAsync(stream, cancellationToken: ct);
            var root = doc.RootElement;

            var tagName = root.TryGetProperty("tag_name", out var tag) ? tag.GetString() : null;
            var version = tagName?.TrimStart('v') ?? string.Empty;
            var body = root.TryGetProperty("body", out var bodyEl) ? bodyEl.GetString() : null;
            var prerelease = root.TryGetProperty("prerelease", out var preEl) && preEl.ValueKind == JsonValueKind.True;

            UpdateAsset? asset = null;
            if (root.TryGetProperty("assets", out var assets) && assets.ValueKind == JsonValueKind.Array)
            {
                foreach (var item in assets.EnumerateArray())
                {
                    if (!item.TryGetProperty("name", out var nameEl) || string.IsNullOrWhiteSpace(nameEl.GetString()))
                    {
                        continue;
                    }

                    if (!Regex.IsMatch(nameEl.GetString()!, SetupAssetRegex))
                    {
                        continue;
                    }

                    var url = item.TryGetProperty("browser_download_url", out var urlEl) ? urlEl.GetString() : null;
                    long? size = item.TryGetProperty("size", out var sizeEl) && sizeEl.TryGetInt64(out var s) ? s : null;
                    asset = new UpdateAsset(nameEl.GetString()!, url, size);
                    break;
                }
            }

            if (asset is null)
            {
                return Failed("Установщик в релизе не найден.");
            }

            if (string.IsNullOrWhiteSpace(asset.InstallerUrl))
            {
                return Failed("У установщика отсутствует ссылка для скачивания.");
            }

            var sha256 = ExtractSha256(body);

            if (UpdateVersionComparer.Compare(version, currentVersion) <= 0)
            {
                return new UpdateCheckResult(UpdateCheckResultKind.UpToDate, null, null);
            }

            var info = new UpdateInfo(version, asset.InstallerUrl, sha256, asset.SizeBytes, body, prerelease);
            return new UpdateCheckResult(UpdateCheckResultKind.UpdateAvailable, info, null);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (JsonException)
        {
            return Failed("Некорректный ответ сервера.");
        }
        catch (HttpRequestException ex)
        {
            return Failed("Не удалось выполнить запрос: " + ex.Message);
        }
    }

    public async Task<string> DownloadInstallerAsync(UpdateInfo update, IProgress<double>? progress = null, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(update.InstallerUrl))
        {
            throw new InvalidOperationException("Не указан URL установщика.");
        }

        Directory.CreateDirectory(_updatesDirectory);
        var path = Path.Combine(_updatesDirectory, $"VoiceTyper-{update.Version}-Setup.exe");
        var tmpPath = path + ".download";

        try
        {
            using var response = await _http.GetAsync(update.InstallerUrl, HttpCompletionOption.ResponseHeadersRead, ct);
            response.EnsureSuccessStatusCode();

            var total = response.Content.Headers.ContentLength;

            await using (var source = await response.Content.ReadAsStreamAsync(ct))
            {
                await using var file = File.Create(tmpPath);
                var buffer = new byte[128 * 1024];
                long downloaded = 0;
                int read;
                while ((read = await source.ReadAsync(buffer, ct)) > 0)
                {
                    await file.WriteAsync(buffer.AsMemory(0, read), ct);
                    downloaded += read;
                    if (progress is not null && total is > 0)
                    {
                        progress.Report(Math.Min(1.0, (double)downloaded / total.Value));
                    }
                }

                await file.FlushAsync(ct);
            }

            progress?.Report(1.0);

            if (!string.IsNullOrWhiteSpace(update.Sha256))
            {
                var actual = await ComputeSha256Async(tmpPath, ct);
                if (!string.Equals(actual, update.Sha256, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidOperationException("Контрольная сумма SHA-256 не совпала. Установщик удалён.");
                }
            }

            File.Move(tmpPath, path, overwrite: true);
            return path;
        }
        catch
        {
            TryDelete(tmpPath);
            throw;
        }
    }

    private static UpdateCheckResult Failed(string message) =>
        new(UpdateCheckResultKind.Failed, null, message);

    /// <summary>Извлекает SHA-256 из тела релиза (строка вида <c>SHA256: &lt;64 hex&gt;</c>).</summary>
    private static string? ExtractSha256(string? body)
    {
        if (string.IsNullOrWhiteSpace(body))
        {
            return null;
        }

        var match = Regex.Match(body, @"(?im)^\s*sha-?256\s*:\s*([0-9a-fA-F]{64})\s*$");
        return match.Success ? match.Groups[1].Value : null;
    }

    private static async Task<string> ComputeSha256Async(string path, CancellationToken ct)
    {
        await using var stream = File.OpenRead(path);
        var hash = await SHA256.HashDataAsync(stream, ct);
        return Convert.ToHexString(hash);
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private sealed record UpdateAsset(string Name, string? InstallerUrl, long? SizeBytes);
}

/// <summary>
/// Сравнение версий вида semver без учета build-metadata (после <c>+</c>).
/// Возвращает положительное число, если <c>a</c> новее, отрицательное — если старее, и 0 при равенстве.
/// </summary>
public static class UpdateVersionComparer
{
    /// <summary>Сравнивает две версии. Не бросает исключений на нечисловых сегментах.</summary>
    public static int Compare(string a, string b)
    {
        var (aNumbers, aPrerelease) = Parse(a);
        var (bNumbers, bPrerelease) = Parse(b);

        var length = Math.Max(aNumbers.Length, bNumbers.Length);
        for (var i = 0; i < length; i++)
        {
            var x = i < aNumbers.Length ? aNumbers[i] : 0;
            var y = i < bNumbers.Length ? bNumbers[i] : 0;
            if (x != y)
            {
                return x.CompareTo(y);
            }
        }

        // Числовые части равны: отсутствие пререлиз-суффикса считаем новее.
        var aStable = aPrerelease is null;
        var bStable = bPrerelease is null;
        if (aStable && !bStable)
        {
            return 1;
        }

        if (!aStable && bStable)
        {
            return -1;
        }

        return 0;
    }

    private static (long[] Numbers, string? Prerelease) Parse(string version)
    {
        if (string.IsNullOrWhiteSpace(version))
        {
            return (Array.Empty<long>(), null);
        }

        var core = version;
        var plus = core.IndexOf('+');
        if (plus >= 0)
        {
            core = core[..plus];
        }

        string? prerelease = null;
        var dash = core.IndexOf('-');
        if (dash >= 0)
        {
            prerelease = core[(dash + 1)..];
            core = core[..dash];
        }

        var parts = core.Split('.');
        var numbers = new long[parts.Length];
        for (var i = 0; i < parts.Length; i++)
        {
            _ = long.TryParse(parts[i], out var value);
            numbers[i] = value;
        }

        return (numbers, prerelease);
    }
}
