using VoiceTyper.Core.Abstractions;
using VoiceTyper.Core.Services;

namespace VoiceTyper.Tests.Services;

public class TextOutputServiceTests
{
    private readonly FakeClipboard _clipboard = new();
    private readonly FakePaster _paster = new();

    [Fact]
    public async Task OutputAsync_WithAutoPaste_CopiesAndPastes()
    {
        var service = new TextOutputService(_clipboard, _paster);

        var ok = await service.OutputAsync("привет мир", autoPaste: true);

        Assert.True(ok);
        Assert.Contains("привет мир", _clipboard.Texts);
        Assert.Equal(1, _paster.PasteCount);
    }

    [Fact]
    public async Task OutputAsync_WithoutAutoPaste_CopiesOnly()
    {
        var service = new TextOutputService(_clipboard, _paster);

        var ok = await service.OutputAsync("привет мир", autoPaste: false);

        Assert.True(ok);
        Assert.Contains("привет мир", _clipboard.Texts);
        Assert.Equal(0, _paster.PasteCount);
    }

    [Fact]
    public async Task OutputAsync_EmptyText_ReturnsFalseAndDoesNothing()
    {
        var service = new TextOutputService(_clipboard, _paster);

        var ok = await service.OutputAsync("   ", autoPaste: true);

        Assert.False(ok);
        Assert.Empty(_clipboard.Texts);
        Assert.Equal(0, _paster.PasteCount);
    }

    private sealed class FakeClipboard : IClipboardWriter
    {
        public List<string> Texts { get; } = new();

        public Task SetTextAsync(string text, CancellationToken ct = default)
        {
            Texts.Add(text);
            return Task.CompletedTask;
        }
    }

    private sealed class FakePaster : IPasteSimulator
    {
        public int PasteCount { get; private set; }

        public void Paste() => PasteCount++;
    }
}
