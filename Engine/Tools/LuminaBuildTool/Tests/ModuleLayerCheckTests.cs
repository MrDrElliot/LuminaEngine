using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using Xunit;

namespace LuminaBuildTool.Tests;

public class ModuleLayerCheckTests
{
    private readonly TestTarget Fixture = new("LayerTest");

    private BuildTarget Target => Fixture.Target;

    private BuildModule AddModule(string Name, bool bIsThirdParty = false, params BuildModule[] Dependencies)
    {
        return Fixture.AddModule(Name, bIsThirdParty, Dependencies);
    }

    [Fact]
    public void ThirdPartyDependingOnFirstPartyThrows()
    {
        BuildModule Core = AddModule("Core");
        AddModule("Vendored", bIsThirdParty: true, Core);

        BuildException Error = Assert.Throws<BuildException>(() => ModuleLayerCheck.Verify(Target));

        Assert.Contains("Vendored is third party and depends on Core", Error.Message);
    }

    [Fact]
    public void ThirdPartyDependingOnThirdPartyPasses()
    {
        BuildModule Zlib = AddModule("Zlib", bIsThirdParty: true);
        AddModule("Png", bIsThirdParty: true, Zlib);
        AddModule("Core", bIsThirdParty: false, Zlib);

        ModuleLayerCheck.Verify(Target);
    }

    [Fact]
    public void ForbiddenDependencyReachedTransitivelyReportsChain()
    {
        BuildModule Editor = AddModule("Editor");
        BuildModule Engine = AddModule("Engine", bIsThirdParty: false, Editor);
        AddModule("Runtime", bIsThirdParty: false, Engine);
        Target.Rules.ForbidDependency("Runtime", "Editor", "runtime ships without the editor");

        BuildException Error = Assert.Throws<BuildException>(() => ModuleLayerCheck.Verify(Target));

        Assert.Contains("Runtime -> Engine -> Editor: runtime ships without the editor", Error.Message);
    }

    [Fact]
    public void ForbiddenDependencyNotReachedPasses()
    {
        AddModule("Editor");
        AddModule("Runtime");
        Target.Rules.ForbidDependency("Runtime", "Editor", "runtime ships without the editor");

        ModuleLayerCheck.Verify(Target);
    }

    [Fact]
    public void DisabledLayeringSkipsEveryCheck()
    {
        BuildModule Editor = AddModule("Editor");
        AddModule("Runtime", bIsThirdParty: true, Editor);
        Target.Rules.ForbidDependency("Runtime", "Editor", "runtime ships without the editor");
        Target.Rules.bEnforceModuleLayering = false;

        ModuleLayerCheck.Verify(Target);
    }
}
