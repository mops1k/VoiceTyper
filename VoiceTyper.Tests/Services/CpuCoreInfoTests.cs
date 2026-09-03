using VoiceTyper.Core.Services;

namespace VoiceTyper.Tests.Services;

public class CpuCoreInfoTests
{
    [Fact]
    public void GetPhysicalCoreCount_ReturnsPositive()
    {
        var count = CpuCoreInfo.GetPhysicalCoreCount();

        Assert.True(count >= 1, $"Physical core count should be >= 1, got {count}");
    }

    [Fact]
    public void GetPhysicalCoreCount_IsNotGreaterThanLogicalCount()
    {
        var physical = CpuCoreInfo.GetPhysicalCoreCount();
        var logical = Environment.ProcessorCount;

        Assert.True(physical <= logical, $"Physical ({physical}) should be <= logical ({logical})");
    }
}
