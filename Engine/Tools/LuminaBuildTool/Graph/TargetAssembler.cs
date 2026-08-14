using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Platform;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.Graph;

/// <summary>
/// Resolves a target name into a complete build graph: instantiates rules, discovers sources,
/// walks module dependencies, propagates public settings and assigns output paths.
/// </summary>
public sealed class TargetAssembler
{
    private readonly RulesAssembly Assembly;

    private readonly BuildDirectories Directories;

    private readonly IBuildPlatform PlatformSupport;

    private readonly Dictionary<string, BuildModule> ModulesByName = new(StringComparer.OrdinalIgnoreCase);

    private readonly Dictionary<string, PublicExports> ExportCache = new(StringComparer.OrdinalIgnoreCase);

    private TargetInfo Info = null!;

    private TargetRules TargetRules = null!;

    public TargetAssembler(RulesAssembly Assembly, BuildDirectories Directories, IBuildPlatform PlatformSupport)
    {
        this.Assembly = Assembly;
        this.Directories = Directories;
        this.PlatformSupport = PlatformSupport;
    }

    public BuildTarget Assemble(string TargetName, TargetInfo TargetInfo)
    {
        Info = TargetInfo;
        TargetRules = Assembly.CreateTargetRules(TargetName, Info);

        // First pass reads what the target decided about itself. Those answers go back into a second
        // TargetInfo so target and module rules see the settled values, not the requested ones.
        BuildConfiguration EffectiveConfiguration = TargetRules.ConfigurationOverride ?? Info.Configuration;

        if (EffectiveConfiguration != Info.Configuration)
        {
            Log.Verbose(
                "Target '{0}' pins configuration {1} in place of {2}",
                TargetName,
                EffectiveConfiguration,
                Info.Configuration);
        }

        Info = new TargetInfo(
            TargetName,
            TargetRules.Type,
            TargetInfo.Platform,
            EffectiveConfiguration,
            TargetInfo.Directories,
            TargetInfo.Options,
            TargetRules.bMonolithic);

        TargetRules = Assembly.CreateTargetRules(TargetName, Info);

        // Computed once so every module in the target lands on the same answer.
        SharedEngineEnvironmentKey = new Lazy<string>(ComputeSharedEngineEnvironmentKey);

        BuildTarget Target = new(TargetRules, Info, Directories);
        Target.RulesFiles.Add(TargetRules.RulesFile);

        ResolvePlugins(Target);

        foreach (string ModuleName in EnumerateSeedModules(Target))
        {
            ResolveModule(ModuleName, new List<string>());
        }

        Target.LaunchModule = ModulesByName.TryGetValue(TargetRules.LaunchModuleName, out BuildModule? Launch) ? Launch : null;

        if (Target.LaunchModule is null && TargetRules.Type != TargetType.Program)
        {
            Log.Verbose("Target '{0}' has no launch module named '{1}'", TargetName, TargetRules.LaunchModuleName);
        }

        Target.Modules.AddRange(TopologicallySort(ModulesByName.Values));

        // Before any environment is built, because a layering violation makes the environments
        // meaningless and the error should be about the graph rather than about what came of it.
        ModuleLayerCheck.Verify(Target);

        foreach (BuildModule Module in Target.Modules)
        {
            Target.RulesFiles.Add(Module.Rules.RulesFile);
            AssignOutputPaths(Target, Module);
        }

        foreach (BuildModule Module in Target.Modules)
        {
            BuildCompileEnvironment(Target, Module);
            BuildLinkEnvironment(Target, Module);
        }

        CollectRuntimeDependencies(Target);

        Log.Verbose("Target '{0}' resolved to {1} modules", TargetName, Target.Modules.Count);

        return Target;
    }

    // Plugin selection.

    private void ResolvePlugins(BuildTarget Target)
    {
        // Standalone tools do not host engine plugins.
        if (TargetRules.Type == TargetType.Program)
        {
            return;
        }

        foreach (PluginDescriptor Plugin in Assembly.Plugins)
        {
            if (TargetRules.DisabledPlugins.Contains(Plugin.Name, StringComparer.OrdinalIgnoreCase))
            {
                continue;
            }

            bool bEnabled = Plugin.EnabledByDefault
                || TargetRules.EnabledPlugins.Contains(Plugin.Name, StringComparer.OrdinalIgnoreCase);

            if (!bEnabled)
            {
                continue;
            }

            if (!Plugin.SupportsPlatform(Info.Platform))
            {
                Log.Verbose("Plugin '{0}' does not support {1}", Plugin.Name, Info.PlatformName);
                continue;
            }

            if (Plugin.EditorOnly && Info.Type != TargetType.Editor)
            {
                Log.Verbose("Plugin '{0}' is editor only, skipping for {1} target", Plugin.Name, Info.Type);
                continue;
            }

            Target.EnabledPlugins.Add(Plugin);
        }
    }

    private IEnumerable<string> EnumerateSeedModules(BuildTarget Target)
    {
        if (!string.IsNullOrEmpty(TargetRules.LaunchModuleName) && Assembly.HasModule(TargetRules.LaunchModuleName))
        {
            yield return TargetRules.LaunchModuleName;
        }

        foreach (string Extra in TargetRules.ExtraModuleNames)
        {
            yield return Extra;
        }

        foreach (PluginDescriptor Plugin in Target.EnabledPlugins)
        {
            foreach (PluginModuleEntry Entry in Plugin.Modules)
            {
                if (!Assembly.HasModule(Entry.Name))
                {
                    throw new BuildException(
                        $"Plugin '{Plugin.Name}' declares module '{Entry.Name}' in its .lplugin, "
                        + $"but no {Entry.Name}.Build.cs was found under '{Plugin.RootDirectory}'.");
                }

                if (Entry.GetHostType().IsAvailableIn(Info.Type))
                {
                    yield return Entry.Name;
                }
            }
        }
    }

    // Module resolution.

    private BuildModule ResolveModule(string ModuleName, List<string> ResolutionStack)
    {
        if (ModulesByName.TryGetValue(ModuleName, out BuildModule? Existing))
        {
            return Existing;
        }

        int CycleAt = ResolutionStack.FindIndex(N => string.Equals(N, ModuleName, StringComparison.OrdinalIgnoreCase));

        if (CycleAt >= 0)
        {
            throw new BuildException(
                "Circular module dependency: " + string.Join(" -> ", ResolutionStack.Skip(CycleAt).Append(ModuleName)));
        }

        ModuleRules Rules = Assembly.CreateModuleRules(ModuleName, Info);

        if (!Rules.HostType.IsAvailableIn(Info.Type) || Rules.ExcludedTargetTypes.Contains(Info.Type))
        {
            throw new BuildException(
                $"Module '{ModuleName}' is not available in a {Info.Type} target "
                + $"(host type {Rules.HostType}). Guard the dependency with a Target.Type check in the referencing Build.cs."
                + (ResolutionStack.Count > 0 ? $" Referenced from: {string.Join(" -> ", ResolutionStack)}" : string.Empty));
        }

        // Resolved before scanning: being its own loadable image decides which sources compile.
        ModuleBinaryType EffectiveBinaryType = ResolveEffectiveBinaryType(Rules.BinaryType);

        ModuleSourceSet Sources = SourceFileScanner.Scan(Rules, EffectiveBinaryType.IsLoadableImage());

        BuildModule Module = new(Rules, Sources)
        {
            BinaryType = EffectiveBinaryType,

            // Module registration runs from static constructors nothing references, so a folded
            // module must be linked whole or it vanishes.
            bRequiresWholeArchive = EffectiveBinaryType != Rules.BinaryType,
        };

        // Register before recursing so a diamond resolves to one instance.
        ModulesByName[ModuleName] = Module;

        ResolutionStack.Add(ModuleName);

        foreach (string DependencyName in Rules.PublicDependencyModuleNames)
        {
            Module.PublicDependencies.Add(ResolveModule(DependencyName, ResolutionStack));
        }

        foreach (string DependencyName in Rules.PrivateDependencyModuleNames)
        {
            Module.PrivateDependencies.Add(ResolveModule(DependencyName, ResolutionStack));
        }

        foreach (string DependencyName in Rules.BuildOrderDependencyModuleNames)
        {
            Module.BuildOrderDependencies.Add(ResolveModule(DependencyName, ResolutionStack));
        }

        ResolutionStack.RemoveAt(ResolutionStack.Count - 1);

        return Module;
    }

    private static List<BuildModule> TopologicallySort(IEnumerable<BuildModule> Modules)
    {
        List<BuildModule> Ordered = new();
        HashSet<BuildModule> Visited = new();
        HashSet<BuildModule> InProgress = new();

        void Visit(BuildModule Module)
        {
            if (Visited.Contains(Module))
            {
                return;
            }

            if (!InProgress.Add(Module))
            {
                throw new BuildException($"Circular module dependency involving '{Module.Name}'.");
            }

            foreach (BuildModule Dependency in Module.AllOrderedDependencies)
            {
                Visit(Dependency);
            }

            InProgress.Remove(Module);
            Visited.Add(Module);
            Ordered.Add(Module);
        }

        foreach (BuildModule Module in Modules.OrderBy(M => M.Name, StringComparer.OrdinalIgnoreCase))
        {
            Visit(Module);
        }

        return Ordered;
    }

    // Output layout.

    /// <summary>
    /// A monolithic target turns every shared library into an archive the executable absorbs, so
    /// there is one image instead of many.
    /// </summary>
    private ModuleBinaryType ResolveEffectiveBinaryType(ModuleBinaryType Declared)
    {
        return TargetRules.bMonolithic && Declared == ModuleBinaryType.SharedLibrary
            ? ModuleBinaryType.StaticLibrary
            : Declared;
    }

    /// <summary>Directory segment separating one target's intermediates from another's.</summary>
    /// <remarks>
    /// Project modules key on target name. Engine modules key on the build environment instead, so
    /// targets that compile the engine identically share one set of objects rather than each
    /// recompiling it; a target that changes that environment hashes differently and gets its own.
    /// </remarks>
    private string ResolveIntermediateKey(BuildModule Module)
    {
        return Directories.IsEngineOwned(Module.Rules.ModuleDirectory)
            ? "Engine-" + SharedEngineEnvironmentKey.Value
            : Info.Name;
    }

    private Lazy<string> SharedEngineEnvironmentKey = null!;

    /// <summary>
    /// Hash of everything a target contributes to how an engine module compiles.
    /// </summary>
    /// <remarks>
    /// Deliberately over-inclusive. Listing too much only costs a target its own copy of the
    /// engine, while listing too little would let two targets share objects built from different
    /// settings. Platform, type and configuration are already directory segments and are not
    /// repeated here.
    /// </remarks>
    private string ComputeSharedEngineEnvironmentKey()
    {
        List<string> Inputs = new()
        {
            TargetRules.CppStandard,
            TargetRules.VectorExtensions,
            TargetRules.bEnableExceptions.ToString(),
            TargetRules.bEnableRtti.ToString(),
            TargetRules.bUseDynamicCrt.ToString(),
            TargetRules.bUseDebugCrt.ToString(),
            TargetRules.bDebugSymbols.ToString(),
            TargetRules.bMonolithic.ToString(),
            TargetRules.bWarningsAsErrors.ToString(),
            TargetRules.WarningLevel.ToString(),
            TargetRules.bLinkTimeCodeGeneration.ToString(),
            TargetRules.bUseUnityBuild.ToString(),
            TargetRules.UnityBuildBytesPerFile.ToString(),
            TargetRules.MinFilesForUnityBuild.ToString(),
        };

        // Order matters for the hash but not for the build, so these are sorted: two targets that
        // declare the same set in a different order still share.
        Inputs.AddRange(TargetRules.GlobalDefinitions.OrderBy(D => D, StringComparer.Ordinal));
        Inputs.Add("|");
        Inputs.AddRange(TargetRules.GlobalCompilerOptions.OrderBy(O => O, StringComparer.Ordinal));
        Inputs.Add("|");
        Inputs.AddRange(TargetRules.GlobalDisabledWarnings.OrderBy(W => W, StringComparer.Ordinal));

        return ContentHash.OfString(string.Join('\n', Inputs))[..12];
    }

    private void AssignOutputPaths(BuildTarget Target, BuildModule Module)
    {
        // Engine modules keep their intermediates and generated code in the engine tree, so a
        // project builds the engine once and shares it rather than duplicating it per project.
        string OutputRoot = Directories.GetOutputRootFor(Module.Rules.ModuleDirectory);
        string TargetKey = ResolveIntermediateKey(Module);

        Module.IntermediateDirectory = Path.Combine(
            OutputRoot,
            "Intermediates",
            "Obj",
            Info.PlatformName,
            TargetKey,
            $"{Info.Type}-{Info.Configuration}",
            Module.Name);

        Module.GeneratedCodeDirectory = Path.Combine(
            OutputRoot,
            "Intermediates",
            "Reflection",
            Info.PlatformName,
            TargetKey,
            $"{Info.Type}-{Info.Configuration}",
            Module.Name);

        if (Module.Rules.bEnableReflection)
        {
            // The generator always emits the full shard set, stubbing the ones with no types, so
            // the compile inputs are known before it has run.
            foreach (string Shard in ReflectionStep.EnumerateUnityShardPaths(Module.GeneratedCodeDirectory))
            {
                Module.GeneratedSourceFiles.Add(FileItem.Get(Shard));
            }

            // A plugin's bindings belong to its own script assembly, not to LuminaSharp.
            if (Module.bIsPlugin && Module.Rules.CSharpBindingsDirectory.Length == 0)
            {
                PluginDescriptor? Plugin = Target.EnabledPlugins
                    .FirstOrDefault(P => string.Equals(P.Name, Module.Rules.PluginName, StringComparison.OrdinalIgnoreCase));

                if (Plugin is not null)
                {
                    Module.Rules.CSharpBindingsDirectory = Path.Combine(Plugin.RootDirectory, "Scripts", "Generated");
                }
            }
        }

        // What the toolchain compiles, before any unity merge. UnityBuildStep rewrites this on the
        // build path; project generation never sees blobs because it reads Sources instead.
        Module.CppCompileInputs.AddRange(Module.Sources.CppFiles);
        Module.CppCompileInputs.AddRange(Module.GeneratedSourceFiles);

        string Suffix = TargetRules.OutputSuffix;

        switch (Module.BinaryType)
        {
            case ModuleBinaryType.HeaderOnly:
                Module.OutputDirectory = string.Empty;
                Module.OutputFile = string.Empty;
                break;

            case ModuleBinaryType.StaticLibrary:
                // Static libraries stay in the intermediate tree; only loadable output ships. Keyed
                // the same way this module's objects are, which for an engine module means the
                // environment rather than the target: archiving to a per-target directory left every
                // target linking the engine's DLLs against its own copy of the same third-party libs,
                // so the link command differed between them and alternating an engine build with a
                // project build relinked all of them every time, in both directions.
                Module.OutputDirectory = Path.Combine(
                    OutputRoot,
                    "Intermediates",
                    "Obj",
                    Info.PlatformName,
                    TargetKey,
                    $"{Info.Type}-{Info.Configuration}",
                    "Lib");
                Module.OutputFile = Path.Combine(
                    Module.OutputDirectory,
                    PlatformSupport.StaticLibraryPrefix + Module.Name + Suffix + PlatformSupport.StaticLibraryExtension);
                break;

            case ModuleBinaryType.SharedLibrary:
                Module.OutputDirectory = ResolveBinariesDirectory(Target, Module);
                Module.OutputFile = Path.Combine(
                    Module.OutputDirectory,
                    PlatformSupport.SharedLibraryPrefix + Module.Name + Suffix + PlatformSupport.SharedLibraryExtension);

                if (PlatformSupport.ImportLibraryExtension is not null)
                {
                    Module.ImportLibraryFile = Path.Combine(
                        Module.IntermediateDirectory,
                        Module.Name + Suffix + PlatformSupport.ImportLibraryExtension);
                }

                break;

            case ModuleBinaryType.ConsoleApplication:
            case ModuleBinaryType.WindowedApplication:
                Module.OutputDirectory = ResolveBinariesDirectory(Target, Module);
                Module.OutputFile = Path.Combine(
                    Module.OutputDirectory,
                    Module.Name + Suffix + PlatformSupport.ExecutableExtension);
                break;
        }
    }

    /// <summary>
    /// Plugin binaries live under the plugin's own Binaries directory so the runtime plugin
    /// loader finds them where it expects.
    /// </summary>
    private string ResolveBinariesDirectory(BuildTarget Target, BuildModule Module)
    {
        if (!Module.bIsPlugin)
        {
            // Engine binaries stay beside the engine, project binaries beside the project.
            string OutputRoot = Directories.GetOutputRootFor(Module.Rules.ModuleDirectory);

            return OutputRoot == Directories.OutputRoot
                ? Target.BinariesDirectory
                : Path.Combine(OutputRoot, "Binaries", Info.PlatformName);
        }

        PluginDescriptor? Plugin = Target.EnabledPlugins
            .FirstOrDefault(P => string.Equals(P.Name, Module.Rules.PluginName, StringComparison.OrdinalIgnoreCase));

        return Plugin is null
            ? Target.BinariesDirectory
            : Path.Combine(Plugin.BinariesDirectory, Info.PlatformName);
    }

    // Environment propagation.

    /// <summary>
    /// Settings a module hands to anything that depends on it, including everything re-exported
    /// through its own public dependencies.
    /// </summary>
    private sealed class PublicExports
    {
        public List<string> IncludePaths { get; } = new();

        public List<string> Definitions { get; } = new();

        public List<string> SystemLibraries { get; } = new();

        public List<string> AdditionalLibraries { get; } = new();

        public List<string> LibraryPaths { get; } = new();
    }

    /// <summary>
    /// Directory a module's own headers are rooted at: the module directory for a flat module,
    /// its Source subdirectory otherwise.
    /// </summary>
    private static string GetSourceRoot(ModuleRules Rules)
    {
        return Rules.ResolveSourceRoot();
    }

    private PublicExports GetPublicExports(BuildModule Module)
    {
        if (ExportCache.TryGetValue(Module.Name, out PublicExports? Cached))
        {
            return Cached;
        }

        PublicExports Exports = new();

        // Insert before recursing so a dependency cycle resolves to the partial set instead of
        // recursing forever. The topological sort has already rejected true cycles.
        ExportCache[Module.Name] = Exports;

        ModuleRules Rules = Module.Rules;

        foreach (string IncludePath in Rules.PublicIncludePaths)
        {
            Exports.IncludePaths.Add(Rules.ModulePath(IncludePath));
        }

        // An engine module's source root is part of its public surface: dependents include its
        // headers by the path they sit at, with no separate Public directory. Third-party modules
        // declare their include roots explicitly instead.
        if (!Rules.bIsThirdParty)
        {
            Exports.IncludePaths.Add(GetSourceRoot(Rules));
        }

        // A plugin module also exports the directory holding it, which is the plugin's Source, so
        // a dependent can write "PointGenerator/Components.h" and name the module it is reaching
        // into. The bare form keeps working; this only adds the qualified one, which is what
        // stops two plugins that both ship a Types.h from resolving to whichever came first.
        //
        // Plugins only. A game module's directory is the project root, and exporting that would
        // put Binaries, Config and Content on every dependent's include path.
        if (Module.bIsPlugin && !Rules.bIsThirdParty)
        {
            string? ContainingDirectory = Path.GetDirectoryName(Rules.ModuleDirectory);

            if (!string.IsNullOrEmpty(ContainingDirectory))
            {
                Exports.IncludePaths.Add(ContainingDirectory);
            }
        }

        if (Rules.bEnableReflection)
        {
            Exports.IncludePaths.Add(Module.GeneratedCodeDirectory);
        }

        Exports.Definitions.AddRange(Rules.PublicDefinitions);
        Exports.SystemLibraries.AddRange(Rules.PublicSystemLibraries);
        Exports.LibraryPaths.AddRange(Rules.PublicLibraryPaths.Select(Rules.ModulePath));
        Exports.AdditionalLibraries.AddRange(Rules.PublicAdditionalLibraries.Select(Rules.ModulePath));

        foreach (BuildModule Dependency in Module.PublicDependencies)
        {
            PublicExports Inherited = GetPublicExports(Dependency);

            Exports.IncludePaths.AddRange(Inherited.IncludePaths);
            Exports.Definitions.AddRange(Inherited.Definitions);
            Exports.SystemLibraries.AddRange(Inherited.SystemLibraries);
            Exports.AdditionalLibraries.AddRange(Inherited.AdditionalLibraries);
            Exports.LibraryPaths.AddRange(Inherited.LibraryPaths);
        }

        return Exports;
    }

    private void BuildCompileEnvironment(BuildTarget Target, BuildModule Module)
    {
        ModuleRules Rules = Module.Rules;

        List<string> IncludePaths = new();
        List<string> Definitions = new();

        foreach (string IncludePath in Rules.PrivateIncludePaths)
        {
            IncludePaths.Add(Rules.ModulePath(IncludePath));
        }

        PublicExports Own = GetPublicExports(Module);
        IncludePaths.AddRange(Own.IncludePaths);
        Definitions.AddRange(Own.Definitions);

        // A module always sees its own source root even when it exports nothing.
        IncludePaths.Add(GetSourceRoot(Rules));

        Definitions.AddRange(Rules.PrivateDefinitions);

        // Direct dependencies contribute their full public closure, public or private edge alike.
        foreach (BuildModule Dependency in Module.AllDependencies)
        {
            PublicExports Inherited = GetPublicExports(Dependency);
            IncludePaths.AddRange(Inherited.IncludePaths);
            Definitions.AddRange(Inherited.Definitions);
        }

        Definitions.AddRange(PlatformSupport.GetPlatformDefinitions(Info));
        Definitions.AddRange(TargetRules.GlobalDefinitions);
        Definitions.Add($"WITH_EDITOR={(Info.bWithEditor ? 1 : 0)}");

        if (Module.BinaryType == ModuleBinaryType.SharedLibrary && !TargetRules.bMonolithic)
        {
            Definitions.Add($"{Module.Name.ToUpperInvariant()}_EXPORTS");
        }

        AddModuleApiDefinitions(Module, Definitions);

        AddUnique(Module.CompileIncludePaths, IncludePaths);
        AddUnique(Module.CompileDefinitions, Definitions);

        // Third-party code does not get the engine's force includes; it has no ModuleAPI.h.
        if (!Rules.bIsThirdParty)
        {
            AddUnique(Module.ForceIncludeFiles, Rules.ForceIncludeFiles);
        }
    }

    /// <summary>
    /// Defines the &lt;NAME&gt;_API export macro for this module and for every shared library it can
    /// see, so a class marked with one resolves to dllexport while compiling its own module and to
    /// dllimport everywhere else.
    /// </summary>
    /// <remarks>
    /// Derived from the graph rather than written down. A hand-maintained header can only name the
    /// modules that shipped with the engine, which leaves a game or plugin module with an
    /// undefined macro and no way to fix it short of editing an engine header it does not own.
    /// The names expand lazily, so DLL_EXPORT and DLL_IMPORT need only be defined by the time some
    /// declaration actually uses the macro.
    /// </remarks>
    private void AddModuleApiDefinitions(BuildModule Module, List<string> Definitions)
    {
        foreach (BuildModule Visible in Module.EnumerateDependencyClosure())
        {
            // Only a module that produces its own shared library has anything to export. A
            // monolithic link folds them all into one image, where the macros must vanish.
            if (Visible.DeclaredBinaryType != ModuleBinaryType.SharedLibrary)
            {
                continue;
            }

            // <NAME>_API is an engine convention, and a vendored library is entitled to that name
            // for its own purposes: Tracy declares TRACY_API in TracyApi.h and manages its own
            // TRACY_EXPORTS. Defining one here would redefine theirs.
            if (Visible.Rules.bIsThirdParty)
            {
                continue;
            }

            string Macro = Visible.Name.ToUpperInvariant() + "_API";

            Definitions.Add(TargetRules.bMonolithic
                ? $"{Macro}="
                : $"{Macro}={(Visible == Module ? "DLL_EXPORT" : "DLL_IMPORT")}");
        }
    }

    /// <summary>
    /// Only modules that actually link resolve symbols. A static library is archived from its
    /// own objects and leaves dependency resolution to whatever consumes it.
    /// </summary>
    private void BuildLinkEnvironment(BuildTarget Target, BuildModule Module)
    {
        if (Module.BinaryType is ModuleBinaryType.HeaderOnly or ModuleBinaryType.StaticLibrary)
        {
            return;
        }

        List<string> Libraries = new();
        List<string> LibraryPaths = new();

        // Dependency-first order reversed, so dependents precede what they depend on.
        foreach (BuildModule Dependency in Module.EnumerateDependencyClosure().Where(M => M != Module).Reverse())
        {
            string? LinkInput = GetLinkInput(Dependency);

            if (LinkInput is not null)
            {
                Libraries.Add(LinkInput);
            }
        }

        // Prebuilt and system libraries from the whole closure, this module's own included.
        foreach (BuildModule Contributor in Module.EnumerateDependencyClosure())
        {
            PublicExports Exports = GetPublicExports(Contributor);

            Libraries.AddRange(Exports.AdditionalLibraries);
            Libraries.AddRange(Exports.SystemLibraries);
            LibraryPaths.AddRange(Exports.LibraryPaths);
        }

        AddUnique(Module.LinkLibraries, Libraries);
        AddUnique(Module.LinkLibraryPaths, LibraryPaths);
    }

    /// <summary>
    /// The file a dependent actually puts on its link line: the import library for a shared
    /// library, the archive itself for a static library, nothing for anything else.
    /// </summary>
    /// <remarks>
    /// ELF platforms have no import library, so a shared library is linked against directly. The
    /// SONAME recorded at link time is what the loader resolves later, not this build path.
    /// </remarks>
    private static string? GetLinkInput(BuildModule Module)
    {
        return Module.BinaryType switch
        {
            ModuleBinaryType.SharedLibrary => Module.ImportLibraryFile.Length > 0
                ? Module.ImportLibraryFile
                : (Module.OutputFile.Length > 0 ? Module.OutputFile : null),
            ModuleBinaryType.StaticLibrary => Module.OutputFile.Length > 0 ? Module.OutputFile : null,
            _ => null,
        };
    }

    /// <summary>
    /// Unions every module's runtime dependencies onto the target. They land beside the target's
    /// binaries, which is where the executable's loader looks.
    /// </summary>
    private static void CollectRuntimeDependencies(BuildTarget Target)
    {
        HashSet<string> Seen = new(StringComparer.OrdinalIgnoreCase);

        foreach (BuildModule Module in Target.Modules)
        {
            foreach (RuntimeDependency Dependency in Module.Rules.RuntimeDependencies)
            {
                if (Seen.Add(Dependency.SourcePath))
                {
                    Target.RuntimeDependencies.Add(Dependency);
                }
            }
        }
    }

    private static void AddUnique(List<string> Destination, IEnumerable<string> Values)
    {
        HashSet<string> Seen = new(Destination, StringComparer.OrdinalIgnoreCase);

        foreach (string Value in Values)
        {
            if (!string.IsNullOrEmpty(Value) && Seen.Add(Value))
            {
                Destination.Add(Value);
            }
        }
    }
}
