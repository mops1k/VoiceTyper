using VoiceTyper.Core.Localization;
using VoiceTyper.Core.Models;

namespace VoiceTyper.Tests.Localization;

public class LocTests
{
    [Fact]
    public void Apply_English_ReturnsEnglish()
    {
        Loc.Instance.Apply(AppLanguage.En);

        Assert.Equal("VoiceTyper — settings", Loc.T("App_WindowTitle"));
        Assert.Equal("Recognition language", Loc.T("General_RecognitionLanguage"));
    }

    [Fact]
    public void Apply_Russian_ReturnsRussian()
    {
        Loc.Instance.Apply(AppLanguage.Ru);

        Assert.Equal("VoiceTyper — настройки", Loc.T("App_WindowTitle"));
        Assert.Equal("Язык распознавания", Loc.T("General_RecognitionLanguage"));
    }

    [Fact]
    public void UnknownKey_ReturnsKey()
    {
        Assert.Equal("NoSuchKey_DoesNotExist", Loc.T("NoSuchKey_DoesNotExist"));
    }
}
