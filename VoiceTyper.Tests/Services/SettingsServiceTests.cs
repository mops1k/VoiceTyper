using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;

namespace VoiceTyper.Tests.Services;

public class SettingsServiceTests : IDisposable
{
    private readonly string _tempDir = Path.Combine(Path.GetTempPath(), "VoiceTyperTests", Guid.NewGuid().ToString("N"));

    public SettingsServiceTests()
    {
        Directory.CreateDirectory(_tempDir);
    }

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
    public void Load_WhenFileMissing_ReturnsDefaults()
    {
        var service = new SettingsService(_tempDir);

        var settings = service.Load();

        Assert.Equal(RecordingMode.PushToTalk, settings.RecordingMode);
        Assert.Equal("Ctrl+Alt+Space", settings.RecordHotkey);
        Assert.True(settings.AutoPasteEnabled);
        Assert.Equal(ModelSize.Small, settings.ModelSize);
    }

    [Fact]
    public void SaveThenLoad_RoundTripsValues()
    {
        var service = new SettingsService(_tempDir);
        var expected = new AppSettings
        {
            RecordingMode = RecordingMode.Vad,
            RecordHotkey = "F12",
            CancelHotkey = "F11",
            Language = RecognitionLanguage.En,
            ModelSize = ModelSize.Medium,
            AutoPasteEnabled = false,
            TermsDictionary = "API,GPU",
            SilenceThresholdMs = 900,
            StartWithWindows = true,
            StartMinimized = false,
        };

        service.Save(expected);
        var actual = service.Load();

        Assert.Equal(expected.RecordingMode, actual.RecordingMode);
        Assert.Equal(expected.RecordHotkey, actual.RecordHotkey);
        Assert.Equal(expected.CancelHotkey, actual.CancelHotkey);
        Assert.Equal(expected.Language, actual.Language);
        Assert.Equal(expected.ModelSize, actual.ModelSize);
        Assert.Equal(expected.AutoPasteEnabled, actual.AutoPasteEnabled);
        Assert.Equal(expected.TermsDictionary, actual.TermsDictionary);
        Assert.Equal(expected.SilenceThresholdMs, actual.SilenceThresholdMs);
        Assert.Equal(expected.StartWithWindows, actual.StartWithWindows);
        Assert.Equal(expected.StartMinimized, actual.StartMinimized);
    }

    [Fact]
    public void Save_WritesJsonWithCamelCaseEnums()
    {
        var service = new SettingsService(_tempDir);
        service.Save(new AppSettings { RecordingMode = RecordingMode.Vad, Language = RecognitionLanguage.Ru });

        var json = File.ReadAllText(service.SettingsFilePath);

        Assert.Contains("\"recordingMode\": \"vad\"", json);
        Assert.Contains("\"language\": \"ru\"", json);
        Assert.Contains("\"autoPasteEnabled\": true", json);
    }

    [Fact]
    public void Load_WhenFileCorrupt_ReturnsDefaults()
    {
        File.WriteAllText(Path.Combine(_tempDir, "settings.json"), "{ not valid json !!");
        var service = new SettingsService(_tempDir);

        var settings = service.Load();

        Assert.NotNull(settings);
        Assert.Equal(RecordingMode.PushToTalk, settings.RecordingMode);
    }

    [Fact]
    public void Clone_ReturnsIndependentCopy()
    {
        var original = new AppSettings { RecordHotkey = "F12" };

        var copy = original.Clone();
        copy.RecordHotkey = "F11";

        Assert.Equal("F12", original.RecordHotkey);
        Assert.Equal("F11", copy.RecordHotkey);
    }
}
