using System.Net;
using System.Security.Cryptography;
using System.Text;
using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;

namespace VoiceTyper.Tests.Services;

public class UpdateVersionComparerTests
{
    [Theory]
    [InlineData("1.10.0", "1.9.0", 1)]
    [InlineData("1.9.0", "1.10.0", -1)]
    [InlineData("1.0.0", "1.0.0", 0)]
    [InlineData("1.0.0", "1.0.1", -1)]
    [InlineData("1.0.0+build", "1.0.0", 0)]
    [InlineData("1.0.0-beta", "1.0.0", -1)]
    [InlineData("1.0.0", "1.0.0-beta.1", 1)]
    public void Compare_ReturnsExpectedOrder(string a, string b, int expectedSign)
    {
        var result = Math.Sign(UpdateVersionComparer.Compare(a, b));
        Assert.Equal(expectedSign, result);
    }
}

public class GithubUpdateServiceTests : IDisposable
{
    private readonly string _tempDir = Path.Combine(Path.GetTempPath(), "VoiceTyperUpdateTests", Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        try
        {
            Directory.Delete(_tempDir, recursive: true);
        }
        catch (IOException)
        {
        }
    }

    [Fact]
    public async Task CheckForUpdate_WhenTagNewer_ReturnsUpdateAvailable()
    {
        var json = ReleaseJson("v1.2.0", assets: new[] { SetupAsset("VoiceTyper-1.2.0-Setup.exe") });
        var service = CreateService(JsonHandler(json));

        var result = await service.CheckForUpdateAsync("1.0.0");

        Assert.True(result.IsAvailable);
        Assert.Equal("1.2.0", result.Update!.Version);
        Assert.Equal("https://example.com/VoiceTyper-1.2.0-Setup.exe", result.Update.InstallerUrl);
    }

    [Fact]
    public async Task CheckForUpdate_SelectsOnlySetupAsset_AndUsesBrowserDownloadUrl()
    {
        var json = ReleaseJson("v1.2.0", assets: new List<(string, string, long)>
        {
            new("VoiceTyper-1.2.0-Setup.zip", "https://example.com/archive.zip", 100),
            new("VoiceTyper-1.2.0-Setup.exe", "https://example.com/installer.exe", 42_000_000),
            new("VoiceTyper-1.2.0-Machine", "https://example.com/arm.exe", 10),
        });
        var service = CreateService(JsonHandler(json));

        var result = await service.CheckForUpdateAsync("1.0.0");

        Assert.True(result.IsAvailable);
        Assert.Equal("https://example.com/installer.exe", result.Update!.InstallerUrl);
        Assert.Equal(42_000_000, result.Update.SizeBytes);
    }

    [Fact]
    public async Task CheckForUpdate_ExtractsSha256FromBody()
    {
        const string hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        var json = ReleaseJson("v1.2.0", body: $"Список изменений\r\nSHA256: {hash}\r\nзавершено",
            assets: new[] { SetupAsset("VoiceTyper-1.2.0-Setup.exe") });
        var service = CreateService(JsonHandler(json));

        var result = await service.CheckForUpdateAsync("1.0.0");

        Assert.True(result.IsAvailable);
        Assert.Equal(hash, result.Update!.Sha256);
    }

    [Fact]
    public async Task CheckForUpdate_NoShaInBody_ReturnsNullSha256()
    {
        var json = ReleaseJson("v1.2.0", body: "Простые заметки к релизу.",
            assets: new[] { SetupAsset("VoiceTyper-1.2.0-Setup.exe") });
        var service = CreateService(JsonHandler(json));

        var result = await service.CheckForUpdateAsync("1.0.0");

        Assert.True(result.IsAvailable);
        Assert.Null(result.Update!.Sha256);
    }

    [Fact]
    public async Task CheckForUpdate_WhenTagNotNewer_ReturnsUpToDate()
    {
        var json = ReleaseJson("v1.0.0", assets: new[] { SetupAsset("VoiceTyper-1.0.0-Setup.exe") });
        var service = CreateService(JsonHandler(json));

        var result = await service.CheckForUpdateAsync("1.0.0");

        Assert.True(result.IsUpToDate);
        Assert.Null(result.Update);
    }

    [Fact]
    public async Task CheckForUpdate_NoSetupAsset_ReturnsFailed()
    {
        var json = ReleaseJson("v1.2.0", assets: new List<(string, string, long)>
        {
            new("VoiceTyper-1.2.0-Setup.zip", "https://example.com/archive.zip", 100),
        });
        var service = CreateService(JsonHandler(json));

        var result = await service.CheckForUpdateAsync("1.0.0");

        Assert.True(result.IsFailed);
        Assert.Null(result.Update);
        Assert.Contains("Установщик", result.Error);
    }

    [Fact]
    public async Task CheckForUpdate_NotFound_ReturnsFailed()
    {
        var service = CreateService(new StubHttpMessageHandler(_ =>
            new HttpResponseMessage(HttpStatusCode.NotFound)));

        var result = await service.CheckForUpdateAsync("1.0.0");

        Assert.True(result.IsFailed);
        Assert.Null(result.Update);
    }

    [Fact]
    public async Task DownloadInstaller_MatchingSha_ReturnsInstallerPath()
    {
        var bytes = Encoding.UTF8.GetBytes("fake installer content for download");
        var sha = Convert.ToHexString(SHA256.HashData(bytes));
        var handler = new StubHttpMessageHandler(_ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new ByteArrayContent(bytes),
        });
        var service = CreateService(handler);
        var update = new UpdateInfo("1.2.0", "https://example.com/VoiceTyper-1.2.0-Setup.exe", sha, bytes.Length, null, false);

        var path = await service.DownloadInstallerAsync(update);

        Assert.Equal(Path.Combine(service.UpdatesDirectory, "VoiceTyper-1.2.0-Setup.exe"), path);
        Assert.True(File.Exists(path));
    }

    [Fact]
    public async Task DownloadInstaller_ShaMismatch_ThrowsAndDeletesFile()
    {
        var bytes = Encoding.UTF8.GetBytes("fake installer content for mismatch");
        var handler = new StubHttpMessageHandler(_ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new ByteArrayContent(bytes),
        });
        var service = CreateService(handler);
        var update = new UpdateInfo("1.2.0", "https://example.com/VoiceTyper-1.2.0-Setup.exe", new string('0', 64), bytes.Length, null, false);
        var targetPath = Path.Combine(service.UpdatesDirectory, "VoiceTyper-1.2.0-Setup.exe");

        await Assert.ThrowsAsync<InvalidOperationException>(() => service.DownloadInstallerAsync(update));

        Assert.False(File.Exists(targetPath));
        Assert.False(File.Exists(targetPath + ".download"));
    }

    private GithubUpdateService CreateService(HttpMessageHandler handler) =>
        new(new HttpClient(handler), _tempDir);

    private static (string, string, long) SetupAsset(string name) =>
        (name, $"https://example.com/{name}", 42_000_000);

    private static StubHttpMessageHandler JsonHandler(string json) => new(_ =>
        new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent(json, Encoding.UTF8, "application/json"),
        });

    private static string ReleaseJson(string tag, string? body = null,
        IEnumerable<(string Name, string Url, long Size)>? assets = null)
    {
        var assetsJson = assets is null
            ? "[]"
            : "[" + string.Join(",", assets.Select(a =>
                $"{{\"name\":\"{a.Name}\",\"browser_download_url\":\"{a.Url}\",\"size\":{a.Size}}}")) + "]";
        var bodyJson = body is null ? "null" : "\"" + EscapeJson(body) + "\"";
        return $"{{\"tag_name\":\"{tag}\",\"prerelease\":false,\"body\":{bodyJson},\"assets\":{assetsJson}}}";
    }

    private static string EscapeJson(string s) =>
        s.Replace("\\", "\\\\")
            .Replace("\"", "\\\"")
            .Replace("\r\n", "\\n")
            .Replace("\n", "\\n")
            .Replace("\r", "\\n");

    private sealed class StubHttpMessageHandler : HttpMessageHandler
    {
        private readonly Func<HttpRequestMessage, HttpResponseMessage> _respond;

        public StubHttpMessageHandler(Func<HttpRequestMessage, HttpResponseMessage> respond) => _respond = respond;

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            Task.FromResult(_respond(request));
    }
}
