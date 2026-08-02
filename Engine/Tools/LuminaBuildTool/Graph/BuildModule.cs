using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>
/// A module resolved against a specific target: its rules, its sources, its place in the
/// dependency graph, and the compile and link environments derived from both.
/// </summary>
public sealed class BuildModule
{
    public BuildModule(ModuleRules Rules, ModuleSourceSet Sources)
    {
        this.Rules = Rules;
        this.Sources = Sources;
    }

    public ModuleRules Rules { get; }

    public ModuleSourceSet Sources { get; }

    public string Name => Rules.Name;

    /// <summary>
    /// What this module actually produces in this target. A monolithic target turns every shared
    /// library into a static one so the executable can absorb them all.
    /// </summary>
    public ModuleBinaryType BinaryType { get; set; }

    /// <summary>What the module declared, before any monolithic rewrite.</summary>
    public ModuleBinaryType DeclaredBinaryType => Rules.BinaryType;

    /// <summary>
    /// The linker must pull in every object of this archive rather than only the ones resolving
    /// an undefined symbol. Module registration runs from static constructors nothing references,
    /// so without this the module would silently vanish from a monolithic build.
    /// </summary>
    public bool bRequiresWholeArchive { get; set; }

    public bool bIsPlugin => Rules.PluginName.Length > 0;

    /// <summary>Dependencies whose public settings this module re-exports to its own dependents.</summary>
    public List<BuildModule> PublicDependencies { get; } = new();

    /// <summary>Dependencies used only by this module's implementation.</summary>
    public List<BuildModule> PrivateDependencies { get; } = new();

    /// <summary>Modules that must be built first but are not linked.</summary>
    public List<BuildModule> BuildOrderDependencies { get; } = new();

    public IEnumerable<BuildModule> AllDependencies => PublicDependencies.Concat(PrivateDependencies);

    public IEnumerable<BuildModule> AllOrderedDependencies => AllDependencies.Concat(BuildOrderDependencies);

    // Resolved environments, filled in by TargetAssembler.

    /// <summary>Include paths passed to the compiler for this module's own translation units.</summary>
    public List<string> CompileIncludePaths { get; } = new();

    /// <summary>Preprocessor definitions for this module's own translation units.</summary>
    public List<string> CompileDefinitions { get; } = new();

    /// <summary>Headers force-included into every translation unit.</summary>
    public List<string> ForceIncludeFiles { get; } = new();

    /// <summary>Libraries this module links, in dependency-first order.</summary>
    public List<string> LinkLibraries { get; } = new();

    /// <summary>Library search paths for this module's link step.</summary>
    public List<string> LinkLibraryPaths { get; } = new();

    // Output layout, filled in by TargetAssembler.

    /// <summary>Directory holding this module's object files and response files.</summary>
    public string IntermediateDirectory { get; set; } = string.Empty;

    /// <summary>Directory holding this module's linked output.</summary>
    public string OutputDirectory { get; set; } = string.Empty;

    /// <summary>Primary linked output: the .dll, .lib or .exe. Empty for header-only modules.</summary>
    public string OutputFile { get; set; } = string.Empty;

    /// <summary>Import library produced alongside a shared library, or empty.</summary>
    public string ImportLibraryFile { get; set; } = string.Empty;

    /// <summary>Directory the reflection generator writes this module's generated sources to.</summary>
    public string GeneratedCodeDirectory { get; set; } = string.Empty;

    /// <summary>Generated sources compiled into this module alongside its own.</summary>
    public List<FileItem> GeneratedSourceFiles { get; } = new();

    /// <summary>
    /// The C++ translation units the toolchain compiles. Filled with the module's own and
    /// generated sources when the graph is assembled, then rewritten by UnityBuildStep on the
    /// build path when the module compiles as unity blobs.
    /// </summary>
    /// <remarks>
    /// Kept separate from Sources so what gets compiled and what the module is made of can differ.
    /// Generated projects list Sources, so an IDE keeps showing real files no matter how they are
    /// batched into the compiler.
    /// </remarks>
    public List<FileItem> CppCompileInputs { get; } = new();

    /// <summary>
    /// Files a compile input stands in for, keyed by that input's path. A unity blob lists its
    /// members so the compile action can depend on them before any header scan has run.
    /// </summary>
    public Dictionary<string, List<FileItem>> SubsumedSourceFiles { get; } = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// Enumerates this module and everything it depends on, in dependency-first order.
    /// </summary>
    public IEnumerable<BuildModule> EnumerateDependencyClosure()
    {
        HashSet<BuildModule> Visited = new();
        List<BuildModule> Ordered = new();

        void Visit(BuildModule Module)
        {
            if (!Visited.Add(Module))
            {
                return;
            }

            foreach (BuildModule Dependency in Module.AllOrderedDependencies)
            {
                Visit(Dependency);
            }

            Ordered.Add(Module);
        }

        Visit(this);
        return Ordered;
    }

    public override string ToString() => $"{Name} ({BinaryType})";
}
