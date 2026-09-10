using VoiceTyper.App.Services;

namespace VoiceTyper.Tests.Services;

public class HotkeyServiceTests
{
    [Theory]
    [InlineData("A")]
    [InlineData("Z")]
    [InlineData("D0")]
    [InlineData("D1")]
    [InlineData("D9")]
    [InlineData("Space")]
    [InlineData("Enter")]
    [InlineData("Escape")]
    [InlineData("Tab")]
    [InlineData("Left")]
    [InlineData("PageDown")]
    [InlineData("F1")]
    [InlineData("F12")]
    [InlineData("F24")]
    [InlineData("NumPad0")]
    [InlineData("NumPad9")]
    [InlineData("OemTilde")]
    [InlineData("OemQuestion")]
    public void VirtualKeyToName_RoundTripsKnownKeys(string name)
    {
        var vk = HotkeyService.ToVirtualKey(name);

        Assert.NotEqual(0, vk);
        Assert.Equal(name, HotkeyService.VirtualKeyToName(vk));
    }

    [Fact]
    public void VirtualKeyToName_ReturnsNullForUnknownCode()
    {
        Assert.Null(HotkeyService.VirtualKeyToName(0x00));
    }
}
