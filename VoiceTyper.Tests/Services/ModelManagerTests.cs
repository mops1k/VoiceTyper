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
    [InlineData(ModelSize.Tiny, "ggml-tiny-q8_0.bin")]
    [InlineData(ModelSize.Base, "ggml-base-q8_0.bin")]
    [InlineData(ModelSize.Small, "ggml-small-q8_0.bin")]
    [InlineData(ModelSize.Medium, "ggml-medium-q8_0.bin")]
    [InlineData(ModelSize.Large, "ggml-large-v3-turbo-q8_0.bin")]
    public void GetModelFileName_MapsSizes(ModelSize size, string expected)
    {
        Assert.Equal(expected, ModelManager.GetModelFileName(size));
    }

    [Theory]
    [InlineData(ModelSize.Tiny, 43_537_433L)]
    [InlineData(ModelSize.Base, 81_768_585L)]
    [InlineData(ModelSize.Small, 264_464_607L)]
    [InlineData(ModelSize.Medium, 823_369_779L)]
    [InlineData(ModelSize.Large, 874_188_075L)]
    public void GetModelApproxSize_ReturnsQuantizedSizes(ModelSize size, long expected)
    {
        Assert.Equal(expected, ModelManager.GetModelApproxSize(size));
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
    public void CleanupLegacyModels_RemovesOldFp16AndQ5Files()
    {
        var manager = new ModelManager(_tempDir);
        Directory.CreateDirectory(_tempDir);
        var legacyNames = new[]
        {
            "ggml-tiny.bin",
            "ggml-base.bin",
            "ggml-small.bin",
            "ggml-medium.bin",
            "ggml-large-v3-turbo.bin",
            "ggml-tiny-q5_1.bin",
            "ggml-base-q5_1.bin",
            "ggml-small-q5_1.bin",
            "ggml-medium-q5_0.bin",
            "ggml-large-v3-turbo-q5_0.bin",
        };
        foreach (var name in legacyNames)
        {
            File.WriteAllText(Path.Combine(_tempDir, name), "legacy");
        }
        var keptPath = Path.Combine(_tempDir, ModelManager.GetModelFileName(ModelSize.Small));
        File.WriteAllText(keptPath, "new");

        manager.CleanupLegacyModels();

        foreach (var name in legacyNames)
        {
            Assert.False(File.Exists(Path.Combine(_tempDir, name)), $"Legacy file should be deleted: {name}");
        }
        Assert.True(File.Exists(keptPath), "New quantized model should be kept");
    }

    [Fact]
    public void ModelsDirectory_DefaultsToLocalAppData()
    {
        var manager = new ModelManager();

        Assert.EndsWith(Path.Combine("VoiceTyper", "models"), manager.ModelsDirectory);
    }
}
