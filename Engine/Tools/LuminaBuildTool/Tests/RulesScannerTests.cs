using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Rules;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class RulesScannerTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("RulesScannerTests").FullName;

    public RulesScannerTests()
    {
        Directory.CreateDirectory(Path.Combine(TempDirectory, "Engine", "Source", "Runtime"));
    }

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    private void Touch(params string[] RelativePaths)
    {
        foreach (string RelativePath in RelativePaths)
        {
            string FullPath = Path.Combine(TempDirectory, RelativePath);
            Directory.CreateDirectory(Path.GetDirectoryName(FullPath)!);
            File.WriteAllText(FullPath, string.Empty);
        }
    }

    private BuildDirectories Directories(string? ProjectRelativePath = null)
    {
        string? ProjectRoot = ProjectRelativePath is null ? null : Path.Combine(TempDirectory, ProjectRelativePath);
        return BuildDirectories.Discover(TempDirectory, ProjectRoot);
    }

    [Fact]
    public void ClassifiesTargetBuildAndSharedFilesButIgnoresUnrelatedFiles()
    {
        Touch(
            "Engine/Foo.Target.cs",
            "Engine/Foo.Build.cs",
            "Engine/Foo.BuildRules.cs",
            "Engine/Foo.cs");

        List<RulesFile> Result = new RulesScanner(Directories()).Scan();

        Assert.Equal(3, Result.Count);
        Assert.Contains(Result, F => F.Kind == RulesFileKind.Target && F.DeclaredName == "Foo");
        Assert.Contains(Result, F => F.Kind == RulesFileKind.Module && F.DeclaredName == "Foo");
        Assert.Contains(Result, F => F.Kind == RulesFileKind.Shared && F.DeclaredName == "Foo");
    }

    [Fact]
    public void IgnoresFilesUnderIgnoredDirectoriesAtAnyDepthButNotLookalikeSiblings()
    {
        Touch(
            "Engine/Source/Intermediates/Deep/Ignored.Build.cs",
            "Engine/Source/Binaries/Ignored2.Build.cs",
            "Engine/Source/obj/Ignored3.Build.cs",
            "Engine/Source/bin/Ignored4.Build.cs",
            "Engine/Source/IntermediatesCache/Kept.Build.cs",
            "Engine/Source/MyBinariesFolder/Kept2.Build.cs");

        List<RulesFile> Result = new RulesScanner(Directories()).Scan();

        Assert.Equal(
            new[] { "Kept", "Kept2" },
            Result.Select(F => F.DeclaredName).OrderBy(Name => Name, StringComparer.Ordinal).ToArray());
    }

    [Fact]
    public void LaterScanOverridesSameKindAndNameButKeepsDistinctSharedFiles()
    {
        Touch(
            "Engine/EngineModule.Build.cs",
            "Project/EngineModule.Build.cs",
            "Engine/Source/Helpers.BuildRules.cs",
            "Project/Other/Helpers.BuildRules.cs");

        List<RulesFile> Result = new RulesScanner(Directories("Project")).Scan();

        RulesFile Module = Assert.Single(Result, F => F.Kind == RulesFileKind.Module && F.DeclaredName == "EngineModule");
        Assert.Equal(PathUtils.Normalize(Path.Combine(TempDirectory, "Project", "EngineModule.Build.cs")), Module.Location);

        Assert.Equal(2, Result.Count(F => F.Kind == RulesFileKind.Shared && F.DeclaredName == "Helpers"));
    }
}
