using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;

namespace VoiceTyper.Tests.Services;

public class ModelManagerTests : IDisposable
{
    private readonly string _tempDir = Path.Combine(Path.GetTempPath(), "VoiceTyperModelTests", Guid.NewGuid().ToString("N"));

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

    [Theory]
    [InlineData(ModelSize.Tiny, "ggml-tiny.bin")]
    [InlineData(ModelSize.Base, "ggml-base.bin")]
    [InlineData(ModelSize.Small, "ggml-small.bin")]
    [InlineData(ModelSize.Medium, "ggml-medium.bin")]
    [InlineData(ModelSize.Large, "ggml-large-v3-turbo.bin")]
    public void GetModelFileName_MapsSizes(ModelSize size, string expected)
    {
        Assert.Equal(expected, ModelManager.GetModelFileName(size));
    }

    [Fact]
    public void GetVadModelFileName_ReturnsSileroName()
    {
        Assert.Equal("ggml-silero-v6.2.0.bin", ModelManager.GetVadModelFileName());
    }

    [Fact]
    public async Task EnsureModelAsync_WhenFileExists_ReturnsWithoutDownload()
    {
        var manager = new ModelManager(_tempDir);
        var path = Path.Combine(_tempDir, ModelManager.GetModelFileName(ModelSize.Small));
        Directory.CreateDirectory(_tempDir);
        await File.WriteAllTextAsync(path, "fake-model");

        var result = await manager.EnsureModelAsync(ModelSize.Small);

        Assert.Equal(path, result);
    }

    [Fact]
    public async Task EnsureVadModelAsync_WhenFileExists_ReturnsWithoutDownload()
    {
        var manager = new ModelManager(_tempDir);
        var path = Path.Combine(_tempDir, ModelManager.GetVadModelFileName());
        Directory.CreateDirectory(_tempDir);
        await File.WriteAllTextAsync(path, "fake-vad");

        var result = await manager.EnsureVadModelAsync();

        Assert.Equal(path, result);
    }

    [Fact]
    public void ModelsDirectory_DefaultsToLocalAppData()
    {
        var manager = new ModelManager();

        Assert.EndsWith(Path.Combine("VoiceTyper", "models"), manager.ModelsDirectory);
    }
}
