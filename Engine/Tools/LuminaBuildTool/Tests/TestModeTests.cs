using LuminaBuildTool.Core;
using LuminaBuildTool.Modes;
using Xunit;

namespace LuminaBuildTool.Tests;

public class TestModeTests
{
    [Fact]
    public void NegativeSuiteTimeoutSecondsThrows()
    {
        Assert.Throws<BuildException>(() => TestMode.ValidateSuiteTimeoutSeconds(-1));
    }

    [Fact]
    public void ZeroSuiteTimeoutSecondsIsAcceptedAsNoTimeout()
    {
        Assert.Equal(0, TestMode.ValidateSuiteTimeoutSeconds(0));
    }

    [Fact]
    public void PositiveSuiteTimeoutSecondsIsAccepted()
    {
        Assert.Equal(1800, TestMode.ValidateSuiteTimeoutSeconds(1800));
    }
}
