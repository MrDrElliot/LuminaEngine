using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class BuildOptionsTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("BuildOptionsTests").FullName;

    public BuildOptionsTests()
    {
        Directory.CreateDirectory(Path.Combine(TempDirectory, "Engine", "Source", "Runtime"));
        Directory.CreateDirectory(Path.Combine(TempDirectory, "Engine", "Build"));
        File.WriteAllText(
            Path.Combine(TempDirectory, "Engine", "Build", "BuildConfiguration.json"),
            """{ "Features": { "Tracy": "auto" } }""");
    }

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    private BuildOptions Load(params string[] Args)
    {
        return BuildOptions.Load(BuildDirectories.Discover(TempDirectory, null), new CommandLine(Args));
    }

    [Fact]
    public void ConfiguredFeatureTakesTheCommandLineValue()
    {
        Assert.Equal(FeatureMode.Off, Load("-Tracy=off").GetMode("Tracy"));
    }

    [Fact]
    public void UnconfiguredFeatureTakesTheCommandLineValue()
    {
        BuildOptions Options = Load("-DevUI=off");

        Assert.Equal(FeatureMode.Off, Options.GetMode("DevUI"));
        Assert.Equal(FeatureMode.Off, Options.WithoutPgo().GetMode("DevUI"));
        Assert.Equal(FeatureMode.Auto, Options.GetMode("GpuProfiling"));
    }

    [Fact]
    public void MalformedValueThrowsForConfiguredFeature()
    {
        Assert.Throws<BuildException>(() => Load("-Tracy=maybe"));
    }

    [Fact]
    public void MalformedValueThrowsForUnconfiguredFeature()
    {
        BuildOptions Options = Load("-DevUI=maybe");

        Assert.Throws<BuildException>(() => Options.GetMode("DevUI"));
    }
}
