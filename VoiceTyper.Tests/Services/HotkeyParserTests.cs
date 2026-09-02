using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;

namespace VoiceTyper.Tests.Services;

public class HotkeyParserTests
{
    [Theory]
    [InlineData("Ctrl+Alt+Space", HotkeyModifiers.Control | HotkeyModifiers.Alt, "Space")]
    [InlineData("ctrl+alt+space", HotkeyModifiers.Control | HotkeyModifiers.Alt, "Space")]
    [InlineData("F12", HotkeyModifiers.None, "F12")]
    [InlineData("Shift+Win+Ctrl+A", HotkeyModifiers.Control | HotkeyModifiers.Shift | HotkeyModifiers.Win, "A")]
    [InlineData("Alt+F4", HotkeyModifiers.Alt, "F4")]
    public void TryParse_ParsesValidGestures(string text, HotkeyModifiers expectedMods, string expectedKey)
    {
        var ok = HotkeyParser.TryParse(text, out var gesture);

        Assert.True(ok);
        Assert.Equal(expectedMods, gesture.Modifiers);
        Assert.Equal(expectedKey, gesture.Key);
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData(null)]
    [InlineData("Ctrl")]
    [InlineData("Ctrl+Alt")]
    [InlineData("UnknownKey+Ctrl")]
    [InlineData("Ctrl++Space")]
    public void TryParse_RejectsInvalidGestures(string? text)
    {
        var ok = HotkeyParser.TryParse(text, out _);

        Assert.False(ok);
    }

    [Fact]
    public void Parse_ThrowsOnInvalidInput()
    {
        Assert.Throws<ArgumentException>(() => HotkeyParser.Parse("Ctrl"));
    }

    [Fact]
    public void Format_ProducesCanonicalOrder()
    {
        var gesture = new HotkeyGesture(
            HotkeyModifiers.Win | HotkeyModifiers.Control | HotkeyModifiers.Shift | HotkeyModifiers.Alt,
            "Space");

        Assert.Equal("Ctrl+Alt+Shift+Win+Space", HotkeyParser.Format(gesture));
    }

    [Fact]
    public void RoundTrip_PreservesGesture()
    {
        var original = HotkeyParser.Parse("Ctrl+Alt+Space");

        var formatted = HotkeyParser.Format(original);
        var parsed = HotkeyParser.Parse(formatted);

        Assert.Equal(original, parsed);
    }
}
