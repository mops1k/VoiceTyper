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

    [Theory]
    [InlineData(ParakeetModelSize.Q4K, "tdt-0.6b-v3-q4_k.gguf")]
    [InlineData(ParakeetModelSize.Q5K, "tdt-0.6b-v3-q5_k.gguf")]
    [InlineData(ParakeetModelSize.Q6K, "tdt-0.6b-v3-q6_k.gguf")]
    [InlineData(ParakeetModelSize.Q8_0, "tdt-0.6b-v3-q8_0.gguf")]
    public void GetParakeetModelFileName_MapsQuants(ParakeetModelSize size, string expected)
    {
        Assert.Equal(expected, ModelManager.GetParakeetModelFileName(size));
    }

    [Theory]
    [InlineData(ParakeetModelSize.Q4K, 675_200_864L)]
    [InlineData(ParakeetModelSize.Q5K, 741_867_360L)]
    [InlineData(ParakeetModelSize.Q6K, 812_700_512L)]
    [InlineData(ParakeetModelSize.Q8_0, 940_663_680L)]
    public void GetParakeetModelApproxSize_ReturnsGgufSizes(ParakeetModelSize size, long expected)
    {
        Assert.Equal(expected, ModelManager.GetParakeetModelApproxSize(size));
    }

    [Fact]
    public async Task EnsureParakeetModelAsync_WhenFileExists_ReturnsWithoutDownload()
    {
        var manager = new ModelManager(_tempDir);
        var path = Path.Combine(_tempDir, ModelManager.GetParakeetModelFileName(ParakeetModelSize.Q8_0));
        Directory.CreateDirectory(_tempDir);
        await File.WriteAllTextAsync(path, "fake-gguf");

        var result = await manager.EnsureParakeetModelAsync(ParakeetModelSize.Q8_0);

        Assert.Equal(path, result);
        Assert.True(manager.IsParakeetModelDownloaded(ParakeetModelSize.Q8_0));
    }

    [Fact]
    public void DeleteParakeetModel_RemovesFileAndReturnsFalseWhenMissing()
    {
        var manager = new ModelManager(_tempDir);
        Directory.CreateDirectory(_tempDir);
        var path = Path.Combine(_tempDir, ModelManager.GetParakeetModelFileName(ParakeetModelSize.Q5K));
        File.WriteAllText(path, "gguf");

        Assert.True(manager.DeleteParakeetModel(ParakeetModelSize.Q5K));
        Assert.False(File.Exists(path));
        Assert.False(manager.DeleteParakeetModel(ParakeetModelSize.Q5K));
    }
}
