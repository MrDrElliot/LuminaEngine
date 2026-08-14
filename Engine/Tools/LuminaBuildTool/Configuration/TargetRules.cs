namespace LuminaBuildTool.Configuration;

/// <summary>A dependency edge the rules declare must not exist.</summary>
/// <param name="ModuleName">The module that must stay clear of the dependency.</param>
/// <param name="DependencyName">What it must not reach, directly or through anything else.</param>
/// <param name="Reason">Why, quoted back in the error. A rule without one becomes folklore.</param>
public sealed record ForbiddenDependency(string ModuleName, string DependencyName, string Reason);

/// <summary>What is being built. Handed to every TargetRules and ModuleRules constructor.</summary>
public sealed class TargetInfo
{
    public TargetInfo(
        string Name,
        TargetType Type,
        BuildPlatform Platform,
        BuildConfiguration Configuration,
        BuildDirectories Directories,
        BuildOptions? Options = null,
        bool bMonolithic = false)
    {
        this.Name = Name;
        this.Type = Type;
        this.Platform = Platform;
        this.Configuration = Configuration;
        this.Directories = Directories;
        this.Options = Options ?? BuildOptions.Empty;
        this.bMonolithic = bMonolithic;
    }

    /// <summary>Every module links into one executable rather than producing a shared library each.</summary>
    public bool bMonolithic { get; }

    /// <summary>Optional-feature switches for this build.</summary>
    public BuildOptions Options { get; }

    /// <summary>Resolved directory layout, so rules can locate engine paths without guessing.</summary>
    public BuildDirectories Directories { get; }

    /// <summary>Engine installation root.</summary>
    public string EngineDirectory => Directories.EngineRoot;

    /// <summary>Engine/Source, the root of the engine's own C++ tree.</summary>
    public string EngineSourceDirectory => Directories.EngineSourceDirectory;

    /// <summary>Game project root, or null when building the engine itself.</summary>
    public string? ProjectDirectory => Directories.ProjectRoot;

    /// <summary>True when the build machine has an NVIDIA display adapter.</summary>
    public bool bHostHasNvidiaGpu => Core.HostCapabilities.bHasNvidiaGpu;

    /// <summary>True when the build machine has an AMD display adapter.</summary>
    public bool bHostHasAmdGpu => Core.HostCapabilities.bHasAmdGpu;

    public string Name { get; }

    public TargetType Type { get; }

    public BuildPlatform Platform { get; }

    public BuildConfiguration Configuration { get; }

    public bool bWithEditor => Type == TargetType.Editor;

    public bool bShipping => Configuration == BuildConfiguration.Shipping;

    public bool bDebug => Configuration == BuildConfiguration.Debug;

    /// <summary>Directory name under Binaries, for example "Windows64".</summary>
    public string PlatformName => Platform.GetOutputDirectoryName();

    /// <summary>Operating system without architecture, for example "Windows".</summary>
    public string SystemName => Platform.GetSystemName();

    public string ArchitectureName => Platform.GetArchitectureName();

    public override string ToString() => $"{Name} {PlatformName} {Configuration}";
}

/// <summary>A .NET project the build drives through the dotnet SDK.</summary>
/// <param name="ProjectFile">Absolute path of the .csproj.</param>
/// <param name="OutputAssembly">Assembly it produces. Declared so freshness needs no MSBuild call.</param>
public sealed record ManagedProject(string ProjectFile, string OutputAssembly);

/// <summary>Base class for a Target.cs file.</summary>
public abstract class TargetRules
{
    /// <summary>Identity published pre-construction, so a Target.cs can resolve paths relative to itself.</summary>
    internal sealed class ConstructionContext
    {
        public required string RulesFile { get; init; }

        public required string RulesDirectory { get; init; }
    }

    [ThreadStatic]
    internal static ConstructionContext? PendingConstruction;

    protected TargetRules(TargetInfo Target)
    {
        this.Target = Target;
        Name = Target.Name;
        Type = Target.Type;
        LaunchModuleName = Target.Name;
        bUseDebugCrt = Target.Configuration == BuildConfiguration.Debug;

        ConstructionContext Context = PendingConstruction
            ?? throw new InvalidOperationException(
                "TargetRules was constructed outside the rules assembly. Target.cs types are created by LuminaBuildTool.");

        RulesFile = Context.RulesFile;
        RulesDirectory = Context.RulesDirectory;
    }

    /// <summary>Resolves a path relative to the Target.cs file. Absolute inputs pass through.</summary>
    public string TargetPath(string RelativePath)
    {
        return Path.IsPathRooted(RelativePath)
            ? Path.GetFullPath(RelativePath)
            : Path.GetFullPath(Path.Combine(RulesDirectory, RelativePath));
    }

    /// <summary>What is being built. Read only; do not mutate from rules.</summary>
    public TargetInfo Target { get; }

    /// <summary>Target name. Defaults to the Target.cs file's base name.</summary>
    public string Name { get; set; }

    public TargetType Type { get; set; }

    /// <summary>Module that produces this target's executable.</summary>
    public string LaunchModuleName { get; set; }

    /// <summary>Modules built though nothing links them: plugins, and tools the launch module needs.</summary>
    public List<string> ExtraModuleNames { get; } = new();

    /// <summary>Builds this target in a different configuration than the one requested.</summary>
    public BuildConfiguration? ConfigurationOverride { get; set; }

    /// <summary>Refuse to build when the module graph violates the layering the rules declare.</summary>
    public bool bEnforceModuleLayering { get; set; } = true;

    /// <summary>Dependency edges that must not exist, for layering the graph cannot state itself.</summary>
    public List<ForbiddenDependency> ForbiddenDependencies { get; } = new();

    /// <summary>Declares that one module must never end up depending on another.</summary>
    public void ForbidDependency(string ModuleName, string DependencyName, string Reason)
    {
        ForbiddenDependencies.Add(new ForbiddenDependency(ModuleName, DependencyName, Reason));
    }

    /// <summary>Definitions applied to every module in the target, including third-party modules.</summary>
    public List<string> GlobalDefinitions { get; } = new();

    /// <summary>Compiler options applied to every module in the target.</summary>
    public List<string> GlobalCompilerOptions { get; } = new();

    /// <summary>Linker options applied to every linked binary in the target.</summary>
    public List<string> GlobalLinkerOptions { get; } = new();

    /// <summary>Warning numbers suppressed target wide.</summary>
    public List<string> GlobalDisabledWarnings { get; } = new();

    /// <summary>Tools built to completion before this target starts, such as the reflection generator.</summary>
    public List<string> PreBuildTargetNames { get; } = new();

    /// <summary>Targets this output is unusable without, built at this target's own type and configuration.</summary>
    public List<string> RequiredTargetNames { get; } = new();

    /// <summary>.NET projects built as part of this target.</summary>
    public List<ManagedProject> ManagedProjects { get; } = new();

    /// <summary>Adds a .NET project, resolving both paths relative to the Target.cs file.</summary>
    public void AddManagedProject(string ProjectFile, string OutputAssembly)
    {
        ManagedProjects.Add(new ManagedProject(TargetPath(ProjectFile), TargetPath(OutputAssembly)));
    }

    /// <summary>Include this target in a whole-solution build.</summary>
    public bool bBuildByDefault { get; set; } = true;

    /// <summary>Make this the solution's startup target.</summary>
    public bool bIsStartupTarget { get; set; }

    /// <summary>Executable the IDE launches.</summary>
    public string DebuggerCommand { get; set; } = string.Empty;

    /// <summary>Arguments passed to the launched executable.</summary>
    public string DebuggerArguments { get; set; } = string.Empty;

    /// <summary>Working directory for the launched executable. Empty uses the engine root.</summary>
    public string DebuggerWorkingDirectory { get; set; } = string.Empty;

    /// <summary>Publish this target's reflected modules as the engine manifest that project builds read.</summary>
    public bool bPublishesEngineReflectionManifest { get; set; } = true;

    /// <summary>Plugins to enable by name on top of the ones marked enabled by default.</summary>
    public List<string> EnabledPlugins { get; } = new();

    /// <summary>Plugins to force off even when they are enabled by default.</summary>
    public List<string> DisabledPlugins { get; } = new();

    /// <summary>Link every module statically into one executable instead of a shared library each.</summary>
    public bool bMonolithic { get; set; }

    /// <summary>Default for whether this target's modules compile as unity files.</summary>
    public bool bUseUnityBuild { get; set; } = true;

    /// <summary>Approximate source bytes packed into one unity file.</summary>
    public int UnityBuildBytesPerFile { get; set; } = 384 * 1024;

    /// <summary>Below this many mergeable sources, compile file by file: no repeated header parsing to save.</summary>
    public int MinFilesForUnityBuild { get; set; } = 3;

    /// <summary>Emit debug symbols.</summary>
    public bool bDebugSymbols { get; set; } = true;

    /// <summary>Enable link time code generation.</summary>
    public bool bLinkTimeCodeGeneration { get; set; }

    public bool bIncrementalLinking { get; set; } = true;

    /// <summary>C++ language standard passed to the toolchain.</summary>
    public string CppStandard { get; set; } = "c++latest";

    /// <summary>Instruction set baseline. AVX2 crashes on CPUs without it; see the engine target.</summary>
    public string VectorExtensions { get; set; } = "AVX";

    public bool bEnableExceptions { get; set; } = true;

    public bool bEnableRtti { get; set; }

    /// <summary>Use the dynamically linked C runtime.</summary>
    public bool bUseDynamicCrt { get; set; } = true;

    /// <summary>Link the debug C runtime.</summary>
    public bool bUseDebugCrt { get; set; }

    /// <summary>Treat the compiler's own warnings as errors across the target.</summary>
    public bool bWarningsAsErrors { get; set; }

    /// <summary>Compiler warning level. Maps to /W&lt;n&gt; on MSVC.</summary>
    public int WarningLevel { get; set; } = 3;

    /// <summary>Suffix appended to output binaries, for example "-Debug".</summary>
    public string OutputSuffix { get; set; } = string.Empty;

    /// <summary>Overrides the directory binaries are written to. Empty uses Binaries/&lt;Platform&gt;.</summary>
    public string OutputDirectoryOverride { get; set; } = string.Empty;

    /// <summary>Absolute path of the Target.cs file.</summary>
    public string RulesFile { get; }

    /// <summary>Directory containing the Target.cs file.</summary>
    public string RulesDirectory { get; }
}
