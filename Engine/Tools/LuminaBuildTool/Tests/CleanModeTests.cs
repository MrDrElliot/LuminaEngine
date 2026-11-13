using LuminaBuildTool.Modes;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class CleanModeTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("CleanModeTests").FullName;

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    [Fact]
    public void DeleteBinaries_KeepsDotNetButDeletesSiblings()
    {
        string BinariesDirectory = Path.Combine(TempDirectory, "Binaries");
        string DotNetDirectory = Path.Combine(BinariesDirectory, "DotNet");
        string Win64Directory = Path.Combine(BinariesDirectory, "Win64");

        Directory.CreateDirectory(DotNetDirectory);
        Directory.CreateDirectory(Win64Directory);

        bool bDeleted = CleanMode.DeleteBinaries(BinariesDirectory, bDryRun: false);

        Assert.True(bDeleted);
        Assert.True(Directory.Exists(DotNetDirectory));
        Assert.False(Directory.Exists(Win64Directory));
    }

    [Fact]
    public void DeleteSolutionFiles_MatchesExactExtensionOnly()
    {
        string SolutionFile = Path.Combine(TempDirectory, "Foo.sln");
        string SolutionXFile = Path.Combine(TempDirectory, "Foo.slnx");
        string SettingsFile = Path.Combine(TempDirectory, "Foo.sln.DotSettings.user");

        File.WriteAllText(SolutionFile, string.Empty);
        File.WriteAllText(SolutionXFile, string.Empty);
        File.WriteAllText(SettingsFile, string.Empty);

        bool bDeleted = CleanMode.DeleteSolutionFiles(TempDirectory, bDryRun: false);

        Assert.True(bDeleted);
        Assert.False(File.Exists(SolutionFile));
        Assert.False(File.Exists(SolutionXFile));
        Assert.True(File.Exists(SettingsFile));
    }

    [Theory]
    [InlineData("-Full", "Full")]
    [InlineData("-Key=Value", "Key")]
    [InlineData("-Key:Value", "Key")]
    public void OptionName_ParsesFlagsAndKeyValuePairs(string Argument, string Expected)
    {
        Assert.Equal(Expected, CleanMode.OptionName(Argument));
    }

    [Fact]
    public void OptionName_ReturnsNullForBarePositional()
    {
        Assert.Null(CleanMode.OptionName("SomeTarget"));
    }

    [Fact]
    public void ForwardedOptions_DropsCleanOnlyOptionsAndPositionals()
    {
        string[] Arguments =
        [
            "Clean", "-Full", "-regenerate", "-Project=Game.lproject", "/EngineRoot:H:/Engine",
            "-Configuration=Debug", "-Verbose", "--", "-Platform=Linux",
        ];

        Assert.Equal(["-Configuration=Debug", "-Verbose"], CleanMode.ForwardedOptions(Arguments));
    }
}
