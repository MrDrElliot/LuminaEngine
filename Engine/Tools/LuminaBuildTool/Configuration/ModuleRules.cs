namespace LuminaBuildTool.Configuration;

/// <summary>Precompiled header declaration for a module.</summary>
public sealed class PrecompiledHeaderRules
{
    public PrecompiledHeaderRules(string Header, string Source)
    {
        this.Header = Header;
        this.Source = Source;
    }

    /// <summary>Header name as written in #include, for example "pch.h".</summary>
    public string Header { get; }

    /// <summary>Source file that creates the PCH, relative to the module directory.</summary>
    public string Source { get; }
}

/// <summary>A prebuilt file that must sit beside the target's binaries at run time.</summary>
/// <param name="SourcePath">Absolute path of the file to copy.</param>
/// <param name="bOptional">Tolerate a missing or locked file, for DLLs a running editor holds open.</param>
public sealed record RuntimeDependency(string SourcePath, bool bOptional);

/// <summary>Base class for a Build.cs file. One module is one compilation and link unit.</summary>
public abstract class ModuleRules
{
    /// <summary>Identity published pre-construction, so a Build.cs can call ModulePath and read its own name.</summary>
    internal sealed class ConstructionContext
    {
        public required string Name { get; init; }

        public required string RulesFile { get; init; }

        public required string ModuleDirectory { get; init; }

        public required string PluginName { get; init; }
    }

    [ThreadStatic]
    internal static ConstructionContext? PendingConstruction;

    protected ModuleRules(TargetInfo Target)
    {
        this.Target = Target;

        ConstructionContext Context = PendingConstruction
            ?? throw new InvalidOperationException(
                "ModuleRules was constructed outside the rules assembly. Build.cs types are created by LuminaBuildTool.");

        Name = Context.Name;
        RulesFile = Context.RulesFile;
        ModuleDirectory = Context.ModuleDirectory;
        PluginName = Context.PluginName;
    }

    /// <summary>What is being built. Read only; do not mutate from rules.</summary>
    public TargetInfo Target { get; }

    /// <summary>Module name, taken from the Build.cs file's base name.</summary>
    public string Name { get; }

    /// <summary>Absolute path of the Build.cs file.</summary>
    public string RulesFile { get; }

    /// <summary>Directory containing the Build.cs file. All relative paths resolve against it.</summary>
    public string ModuleDirectory { get; }

    /// <summary>Owning plugin name, or empty for engine and project modules.</summary>
    public string PluginName { get; }

    public ModuleBinaryType BinaryType { get; set; } = ModuleBinaryType.SharedLibrary;

    public ModuleHostType HostType { get; set; } = ModuleHostType.Runtime;

    // Include paths.

    /// <summary>Include paths visible to this module and to every dependent.</summary>
    public List<string> PublicIncludePaths { get; } = new();

    /// <summary>Include paths visible only to this module.</summary>
    public List<string> PrivateIncludePaths { get; } = new();

    // Preprocessor.

    /// <summary>Definitions applied to this module and to every dependent.</summary>
    public List<string> PublicDefinitions { get; } = new();

    /// <summary>Definitions applied only to this module.</summary>
    public List<string> PrivateDefinitions { get; } = new();

    // Module dependencies.

    /// <summary>Dependencies exposed by this module's public headers, so they propagate on to dependents.</summary>
    public List<string> PublicDependencyModuleNames { get; } = new();

    /// <summary>Dependencies used only by this module's implementation. Their public settings stop here.</summary>
    public List<string> PrivateDependencyModuleNames { get; } = new();

    /// <summary>Modules built before this one but not linked: code generators, runtime-loaded assemblies.</summary>
    public List<string> BuildOrderDependencyModuleNames { get; } = new();

    // Libraries.

    /// <summary>System libraries by bare name, for example "ws2_32". Propagates to dependents.</summary>
    public List<string> PublicSystemLibraries { get; } = new();

    /// <summary>Prebuilt libraries by absolute path. Propagates to dependents.</summary>
    public List<string> PublicAdditionalLibraries { get; } = new();

    /// <summary>Library search paths. Propagates to dependents.</summary>
    public List<string> PublicLibraryPaths { get; } = new();

    // Sources.

    /// <summary>Directories searched recursively for source files, relative to the module directory.</summary>
    public List<string> SourceDirectories { get; } = new();

    /// <summary>Treat the module directory as the source root, skipping the Source subdirectory check.</summary>
    public bool bRootSourceFiles { get; set; }

    /// <summary>Directory this module's sources and own-module includes are rooted at.</summary>
    public string ResolveSourceRoot()
    {
        if (bRootSourceFiles)
        {
            return ModuleDirectory;
        }

        string Nested = ModulePath("Source");

        return Directory.Exists(Nested) ? Nested : ModuleDirectory;
    }

    /// <summary>Additional individual source files by absolute or module-relative path.</summary>
    public List<string> ExtraSourceFiles { get; } = new();

    /// <summary>Merge this module's translation units into unity blobs. Null defers to the target.</summary>
    public bool? bUseUnityBuild { get; set; }

    /// <summary>Sources that keep their own translation unit, by file name.</summary>
    public List<string> ExcludeFromUnity { get; } = new();

    /// <summary>Sources belonging to the image, not the module: compiled only when this module is its own binary.</summary>
    public List<string> PerImageSourceFiles { get; } = new();

    /// <summary>Compile only what ExtraSourceFiles lists, skipping the directory walk.</summary>
    public bool bUseExplicitSourceList { get; set; }

    /// <summary>Path fragments that exclude a discovered source file when contained in its path.</summary>
    public List<string> ExcludedSourcePathFragments { get; } = new();

    // Compilation.

    /// <summary>Headers force-included ahead of every translation unit.</summary>
    public List<string> ForceIncludeFiles { get; } = new();

    public PrecompiledHeaderRules? PrecompiledHeader { get; set; }

    /// <summary>Extra compiler options for this module only.</summary>
    public List<string> PrivateCompilerOptions { get; } = new();

    /// <summary>Extra linker options applied when this module is linked.</summary>
    public List<string> PrivateLinkerOptions { get; } = new();

    /// <summary>Warning numbers promoted to errors for this module.</summary>
    public List<string> FatalWarnings { get; } = new();

    /// <summary>Warning numbers suppressed for this module.</summary>
    public List<string> DisabledWarnings { get; } = new();

    /// <summary>Warning levels for this module, layered over the target's own map.</summary>
    public WarningSettings Warnings { get; } = new();

    /// <summary>Per-file compiler options, for units needing a flag the rest of the module must not get.</summary>
    public Dictionary<string, List<string>> PerFileCompilerOptions { get; } = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Run the reflection generator over this module's headers and compile the output into it.</summary>
    public bool bEnableReflection { get; set; }

    /// <summary>Where the generator writes this module's C# bindings.</summary>
    public string CSharpBindingsDirectory { get; set; } = string.Empty;

    /// <summary>Compile this module's sources as C rather than C++.</summary>
    public bool bCompileAsC { get; set; }

    /// <summary>Overrides the target's RTTI setting. Null inherits.</summary>
    public bool? bEnableRtti { get; set; }

    /// <summary>Overrides the target's exception setting. Null inherits.</summary>
    public bool? bEnableExceptions { get; set; }

    /// <summary>Overrides the target's C++ standard.</summary>
    public string? CppStandardOverride { get; set; }

    /// <summary>Prebuilt files copied next to the target's binaries.</summary>
    public List<RuntimeDependency> RuntimeDependencies { get; } = new();

    /// <summary>Declares a file that must sit beside the target's binaries at run time.</summary>
    public void AddRuntimeDependency(string SourcePath, bool bOptional = false)
    {
        RuntimeDependencies.Add(new RuntimeDependency(ModulePath(SourcePath), bOptional));
    }

    /// <summary>Third-party code. Suppresses warnings-as-errors and skips the engine-wide force includes.</summary>
    public bool bIsThirdParty { get; set; }

    /// <summary>Target types this module is excluded from beyond what HostType already implies.</summary>
    public List<TargetType> ExcludedTargetTypes { get; } = new();

    // Helpers for rules files.

    /// <summary>Resolves a module-relative path to an absolute one. Absolute inputs pass through.</summary>
    public string ModulePath(string RelativePath)
    {
        return Path.IsPathRooted(RelativePath)
            ? Path.GetFullPath(RelativePath)
            : Path.GetFullPath(Path.Combine(ModuleDirectory, RelativePath));
    }

    /// <summary>Adds a per-file compiler option keyed by file name.</summary>
    public void AddPerFileOption(string SourceFileName, params string[] Options)
    {
        if (!PerFileCompilerOptions.TryGetValue(SourceFileName, out List<string>? Existing))
        {
            Existing = new List<string>();
            PerFileCompilerOptions[SourceFileName] = Existing;
        }

        Existing.AddRange(Options);
    }
}
