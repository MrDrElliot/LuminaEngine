using LuminaBuildTool.Configuration;
using LuminaBuildTool.Graph;

namespace LuminaBuildTool.Tests;

internal sealed class TestTarget
{
    private sealed class FakeTargetRules : TargetRules
    {
        public FakeTargetRules(TargetInfo Target) : base(Target) { }
    }

    private sealed class FakeModuleRules : ModuleRules
    {
        public FakeModuleRules(TargetInfo Target) : base(Target) { }
    }

    private readonly TargetInfo Info;

    public TestTarget(string Name, string? EngineRoot = null, string? ProjectRoot = null)
    {
        Info = new TargetInfo(
            Name, TargetType.Editor, BuildPlatform.Windows64, BuildConfiguration.Development,
            BuildDirectories.Discover(EngineRoot, ProjectRoot));

        Target = new BuildTarget(MakeTargetRules(), Info, Info.Directories);
    }

    public BuildTarget Target { get; }

    public string ModuleDirectory { get; init; } = string.Empty;

    public TargetRules MakeTargetRules()
    {
        TargetRules.PendingConstruction = new TargetRules.ConstructionContext { RulesFile = string.Empty, RulesDirectory = string.Empty };

        try
        {
            return new FakeTargetRules(Info);
        }
        finally
        {
            TargetRules.PendingConstruction = null;
        }
    }

    public ModuleRules MakeModuleRules(string Name)
    {
        ModuleRules.PendingConstruction = new ModuleRules.ConstructionContext
        {
            Name = Name, RulesFile = string.Empty, ModuleDirectory = ModuleDirectory, PluginName = string.Empty,
        };

        try
        {
            return new FakeModuleRules(Info);
        }
        finally
        {
            ModuleRules.PendingConstruction = null;
        }
    }

    public BuildModule AddModule(string Name, bool bIsThirdParty = false, params BuildModule[] Dependencies)
    {
        ModuleRules Rules = MakeModuleRules(Name);
        Rules.bIsThirdParty = bIsThirdParty;

        BuildModule Module = new(Rules, new ModuleSourceSet());
        Module.PrivateDependencies.AddRange(Dependencies);
        Target.Modules.Add(Module);
        return Module;
    }
}
