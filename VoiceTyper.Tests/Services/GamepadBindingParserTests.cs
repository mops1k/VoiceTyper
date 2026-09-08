using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;

namespace VoiceTyper.Tests.Services;

public class GamepadBindingParserTests
{
    [Theory]
    [InlineData("XInput|A", GamepadSource.XInput, "A")]
    [InlineData("XInput|LB", GamepadSource.XInput, "LB")]
    [InlineData("xinput|dpadup", GamepadSource.XInput, "DPadUp")]
    [InlineData("XInput|leftstick", GamepadSource.XInput, "LeftStick")]
    public void TryParse_ParsesValidXInput(string text, GamepadSource source, string buttonId)
    {
        var ok = GamepadBindingParser.TryParse(text, out var binding);

        Assert.True(ok);
        Assert.Equal(source, binding.Source);
        Assert.Equal(buttonId, binding.ButtonId);
    }

    [Theory]
    [InlineData("DInput|Logitech RumblePad USB|3", GamepadSource.DirectInput, "Logitech RumblePad USB|3")]
    [InlineData("DInput|Joystick: Pro|0", GamepadSource.DirectInput, "Joystick: Pro|0")]
    public void TryParse_ParsesValidDirectInput(string text, GamepadSource source, string buttonId)
    {
        var ok = GamepadBindingParser.TryParse(text, out var binding);

        Assert.True(ok);
        Assert.Equal(source, binding.Source);
        Assert.Equal(buttonId, binding.ButtonId);
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData(null)]
    [InlineData("Keyboard|A")]
    [InlineData("XInput")]
    [InlineData("XInput|A|B")]
    [InlineData("XInput|Unknown")]
    [InlineData("DInput|OnlyName")]
    [InlineData("DInput|Name|-1")]
    [InlineData("DInput|Name|abc")]
    public void TryParse_RejectsInvalidBindings(string? text)
    {
        var ok = GamepadBindingParser.TryParse(text, out _);

        Assert.False(ok);
    }

    [Fact]
    public void Parse_ThrowsOnInvalidInput()
    {
        Assert.Throws<ArgumentException>(() => GamepadBindingParser.Parse("XInput|Unknown"));
    }

    [Theory]
    [InlineData(GamepadSource.XInput, "A", "XInput|A")]
    [InlineData(GamepadSource.DirectInput, "Logitech|3", "DInput|Logitech|3")]
    public void Format_ProducesCanonicalString(GamepadSource source, string buttonId, string expected)
    {
        var text = GamepadBindingParser.Format(new GamepadBinding(source, buttonId));

        Assert.Equal(expected, text);
    }

    [Fact]
    public void RoundTrip_PreservesXInputBinding()
    {
        var original = GamepadBindingParser.Parse("XInput|A");

        var formatted = GamepadBindingParser.Format(original);
        var parsed = GamepadBindingParser.Parse(formatted);

        Assert.Equal(original, parsed);
    }

    [Fact]
    public void RoundTrip_PreservesDirectInputBinding()
    {
        var original = GamepadBindingParser.Parse("DInput|Logitech RumblePad USB|3");

        var formatted = GamepadBindingParser.Format(original);
        var parsed = GamepadBindingParser.Parse(formatted);

        Assert.Equal(original, parsed);
    }

    [Fact]
    public void FromXInput_MapsButtonToBinding()
    {
        var binding = GamepadBindingParser.FromXInput(XInputPadButton.A);

        Assert.Equal(GamepadSource.XInput, binding.Source);
        Assert.Equal("A", binding.ButtonId);
    }

    [Fact]
    public void FromDirectInput_MapsNameAndIndexToBinding()
    {
        var binding = GamepadBindingParser.FromDirectInput("Logitech", 3);

        Assert.Equal(GamepadSource.DirectInput, binding.Source);
        Assert.Equal("Logitech|3", binding.ButtonId);
    }

    [Theory]
    [InlineData(GamepadSource.XInput, "A")]
    [InlineData(GamepadSource.DirectInput, "Logitech|4")]
    public void ToDisplayString_NonEmptyForKnownSource(GamepadSource source, string buttonId)
    {
        var display = GamepadBindingParser.ToDisplayString(new GamepadBinding(source, buttonId));

        Assert.False(string.IsNullOrWhiteSpace(display));
    }

    [Fact]
    public void ToDisplayString_EmptyForNone()
    {
        Assert.Equal(string.Empty, GamepadBindingParser.ToDisplayString(null));
        Assert.Equal(string.Empty, GamepadBindingParser.ToDisplayString(new GamepadBinding(GamepadSource.None, "A")));
    }

    [Fact]
    public void Matcher_MatchesSameSourceAndButtonId()
    {
        var binding = GamepadBindingParser.Parse("XInput|A");
        var input = new GamepadInput(GamepadSource.XInput, "a");

        Assert.True(GamepadBindingMatcher.Matches(binding, input));
    }

    [Theory]
    [InlineData("XInput|A", GamepadSource.DirectInput, "A")]
    [InlineData("XInput|A", GamepadSource.XInput, "B")]
    [InlineData("DInput|Logitech|3", GamepadSource.DirectInput, "Logitech|4")]
    public void Matcher_DoesNotMatchDifferentBinding(
        string bindingText, GamepadSource inputSource, string inputButtonId)
    {
        var binding = GamepadBindingParser.Parse(bindingText);
        var input = new GamepadInput(inputSource, inputButtonId);

        Assert.False(GamepadBindingMatcher.Matches(binding, input));
    }

    [Fact]
    public void Matcher_NullBinding_DoesNotMatch()
    {
        var input = new GamepadInput(GamepadSource.XInput, "A");

        Assert.False(GamepadBindingMatcher.Matches(null, input));
    }
}
