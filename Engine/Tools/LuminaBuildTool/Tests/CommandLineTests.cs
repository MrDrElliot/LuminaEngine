using LuminaBuildTool.Core;
using Xunit;

namespace LuminaBuildTool.Tests;

public class CommandLineTests
{
    [Theory]
    [InlineData("true", true)]
    [InlineData("1", true)]
    [InlineData("on", true)]
    [InlineData("yes", true)]
    [InlineData("TRUE", true)]
    [InlineData("On", true)]
    [InlineData("false", false)]
    [InlineData("0", false)]
    [InlineData("off", false)]
    [InlineData("no", false)]
    [InlineData("FALSE", false)]
    [InlineData("Off", false)]
    public void GetBool_ParsesKnownSpellings(string Value, bool Expected)
    {
        CommandLine Args = new(new[] { $"-KeepGoing={Value}" });

        Assert.Equal(Expected, Args.GetBool("KeepGoing", !Expected));
    }

    [Fact]
    public void GetBool_ReturnsDefaultWhenAbsent()
    {
        CommandLine Args = new(Array.Empty<string>());

        Assert.True(Args.GetBool("KeepGoing", true));
        Assert.False(Args.GetBool("KeepGoing", false));
    }

    [Fact]
    public void GetBool_ThrowsOnInvalidValue()
    {
        CommandLine Args = new(new[] { "-KeepGoing=yepp" });

        Assert.Throws<BuildException>(() => Args.GetBool("KeepGoing", false));
    }
}
