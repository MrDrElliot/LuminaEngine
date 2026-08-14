using LuminaBuildTool.Configuration;

namespace LuminaBuildTool.Graph;

/// <summary>A fully resolved target: every module it needs, in build order, with output paths assigned.</summary>
public sealed class BuildTarget
{
    public BuildTarget(TargetRules Rules, TargetInfo Info, BuildDirectories Directories)
    {
        this.Rules = Rules;
        this.Info = Info;
        this.Directories = Directories;
    }

    public TargetRules Rules { get; }

    public TargetInfo Info { get; }

    public BuildDirectories Directories { get; }

    public string Name => Rules.Name;

    /// <summary>Every module in the target, ordered so dependencies come before dependents.</summary>
    public List<BuildModule> Modules { get; } = new();

    /// <summary>Module that produces the target's executable, or null for library-only targets.</summary>
    public BuildModule? LaunchModule { get; set; }

    /// <summary>Plugins contributing modules to this target.</summary>
    public List<PluginDescriptor> EnabledPlugins { get; } = new();

    /// <summary>Every rules file the target was assembled from. Changing any invalidates the build.</summary>
    public List<string> RulesFiles { get; } = new();

    /// <summary>Prebuilt files that must sit beside the binaries at run time, unioned across modules.</summary>
    public List<RuntimeDependency> RuntimeDependencies { get; } = new();

    public string BinariesDirectory => Rules.OutputDirectoryOverride.Length > 0
        ? Rules.OutputDirectoryOverride
        : Directories.BinariesDirectory(Info.Platform);

    public string IntermediateDirectory =>
        Directories.ObjectDirectory(Name, Info.Platform, Info.Configuration, Info.Type);

    public BuildModule? FindModule(string ModuleName)
    {
        return Modules.FirstOrDefault(M => string.Equals(M.Name, ModuleName, StringComparison.OrdinalIgnoreCase));
    }

    public override string ToString() => $"{Name} {Info.PlatformName} {Info.Configuration}";
}
