using LuminaBuildTool.Configuration;
using Xunit;

namespace LuminaBuildTool.Tests;

public class CompilerWarningTests
{
    [Fact]
    public void EveryEnumValueHasATableEntry()
    {
        foreach (CompilerWarning Warning in Enum.GetValues<CompilerWarning>())
        {
            CompilerWarningInfo Info = CompilerWarnings.Get(Warning);

            Assert.Equal(Warning, Info.Warning);
        }
    }

    [Fact]
    public void IndexerReturnsDefaultForAnUnsetWarning()
    {
        WarningSettings Settings = new();

        Assert.Equal(WarningSeverity.Default, Settings[CompilerWarning.Switch]);
    }

    [Fact]
    public void IndexerReturnsTheSetValue()
    {
        WarningSettings Settings = new() { [CompilerWarning.Switch] = WarningSeverity.Fatal };

        Assert.Equal(WarningSeverity.Fatal, Settings[CompilerWarning.Switch]);
    }

    [Fact]
    public void ApplyOverwritesALevelAlreadySetOnTheReceiver()
    {
        WarningSettings Receiver = new() { [CompilerWarning.Switch] = WarningSeverity.Warning };
        WarningSettings Other = new() { [CompilerWarning.Switch] = WarningSeverity.Fatal };

        Receiver.Apply(Other);

        Assert.Equal(WarningSeverity.Fatal, Receiver[CompilerWarning.Switch]);
    }
}
