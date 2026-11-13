using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class ReflectionStepTests : IDisposable
{
    private readonly string EngineRoot = Path.Combine(Path.GetTempPath(), $"ReflectionStepTests-{Guid.NewGuid():N}");

    private readonly TestTarget Fixture;

    public ReflectionStepTests()
    {
        Directory.CreateDirectory(Path.Combine(EngineRoot, "Engine", "Source", "Runtime"));
        Fixture = new TestTarget("ReflectionStepTest", EngineRoot);
    }

    public void Dispose()
    {
        Directory.Delete(EngineRoot, recursive: true);
    }

    [Fact]
    public void EnumeratedShardPathsMatchIndexedShardPaths()
    {
        List<string> Shards = ReflectionStep.EnumerateUnityShardPaths(EngineRoot).ToList();

        Assert.Equal(ReflectionStep.UnityShardCount, Shards.Count);
        Assert.Equal(Enumerable.Range(0, ReflectionStep.UnityShardCount).Select(Shard => ReflectionStep.GetUnityShardPath(EngineRoot, Shard)), Shards);
    }

    [Theory]
    [InlineData("Module.h", true)]
    [InlineData("Module.inl", false)]
    [InlineData("Module.IPP", false)]
    public void IncludeFragmentsAreNotParsed(string FileName, bool bExpected)
    {
        Assert.Equal(bExpected, ReflectionStep.IsParseableHeader(FileItem.Get(Path.Combine(EngineRoot, FileName))));
    }

    [Fact]
    public void LayoutStampFollowsRoutedBindingsDirectory()
    {
        BuildModule Module = Fixture.AddModule("Game");
        string Fallback = Path.Combine(Fixture.Target.Directories.OutputRoot, "Intermediates", "CSharpBindings", "Game", "Game.Layout.generated.cs");
        Assert.Equal(Fallback, ReflectionStep.GetLayoutStampPath(Fixture.Target, Module));

        string Routed = Path.Combine(EngineRoot, "Bindings");
        Module.Rules.CSharpBindingsDirectory = Routed;
        Assert.Equal(Fallback, ReflectionStep.GetLayoutStampPath(Fixture.Target, Module));

        Module.Rules.bRouteCSharpTypeBindings = true;
        Assert.Equal(Path.Combine(Routed, "Game.Layout.generated.cs"), ReflectionStep.GetLayoutStampPath(Fixture.Target, Module));
    }

    [Fact]
    public void EngineReferenceProjectsSkipModulesTheTargetAlreadyReflects()
    {
        JsonStore.Save(ReflectionStep.EngineManifestPath(Fixture.Target), new EngineReflectionManifest
        {
            Projects =
            {
                new ReflectionProjectEntry { Name = "core", Path = "Engine", CSharpBindingsDir = "Engine" },
                new ReflectionProjectEntry { Name = "Renderer", CSharpBindingsDir = "Bindings" },
            },
        });

        ReflectionInputDocument Document = new() { Projects = { new ReflectionProjectEntry { Name = "Core", Path = "Own" } } };
        ReflectionStep.AppendEngineReferenceProjects(Fixture.Target, Document);

        Assert.Equal(new[] { "Core", "Renderer" }, Document.Projects.Select(P => P.Name));
        Assert.Equal("Own", Document.Projects[0].Path);
        Assert.False(Document.Projects[0].ReferenceOnly);
        Assert.True(Document.Projects[1].ReferenceOnly);
        Assert.Empty(Document.Projects[1].CSharpBindingsDir);
    }
}
