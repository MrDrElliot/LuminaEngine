using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Platform;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.Graph;

/// <summary>Resolves a target name into a build graph: rules, sources, dependencies, settings, outputs.</summary>
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

        // Read after construction, so a Target.cs body can override what its base class chose.
        if (Info.Options.Pgo is PgoMode Requested)
        {
            TargetRules.Pgo = Requested;
        }

        TargetRules.GlobalDefinitions.Add(
            TargetRules.ForceInlineHint == ForceInlineHintPolicy.Hint
                ? "LUMINA_FORCEINLINE_HINTS_FORCED=0"
                : "LUMINA_FORCEINLINE_HINTS_FORCED=1");

        TargetRules.GlobalDefinitions.Add(
            TargetRules.Pgo == PgoMode.Off ? "LUMINA_WITH_PGO=0" : "LUMINA_WITH_PGO=1");

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

            // The engine's own build reuses a project-scanned rules assembly, whose plugins would generate here.
            if (Directories.ProjectRoot is null && !PathUtils.IsUnder(Plugin.RootDirectory, Directories.EngineRoot))
            {
                continue;
            }

            // An explicit ask outranks the descriptor, which is what makes an opt-in plugin buildable.
            bool? Override = Info.Options.GetPluginOverride(Plugin.Name);

            bool bEnabled = Override
                ?? (Plugin.EnabledByDefault
                    || TargetRules.EnabledPlugins.Contains(Plugin.Name, StringComparer.OrdinalIgnoreCase));

            if (!bEnabled)
            {
                if (Override is false)
                {
                    Log.Verbose("Plugin '{0}' was turned off by name", Plugin.Name);
                }

                continue;
            }

            if (Override is true && !Plugin.EnabledByDefault)
            {
                Log.Verbose("Plugin '{0}' is opt in and was turned on by name", Plugin.Name);
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

    /// <summary>A monolithic target turns every shared library into an archive the executable absorbs.</summary>
    private ModuleBinaryType ResolveEffectiveBinaryType(ModuleBinaryType Declared)
    {
        return TargetRules.bMonolithic && Declared == ModuleBinaryType.SharedLibrary
            ? ModuleBinaryType.StaticLibrary
            : Declared;
    }

    /// <summary>Directory segment separating one target's intermediates from another's.</summary>
    private string ResolveIntermediateKey(BuildModule Module)
    {
        return Directories.IsEngineOwned(Module.Rules.ModuleDirectory)
            ? "Engine-" + SharedEngineEnvironmentKey.Value
            : Info.Name;
    }

    private Lazy<string> SharedEngineEnvironmentKey = null!;

    /// <summary>Hash of everything a target contributes to how an engine module compiles.</summary>
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
            TargetRules.Pgo.ToString(),
            TargetRules.ForceInlineHint.ToString(),
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
        Inputs.Add("|");
        Inputs.AddRange(TargetRules.GlobalFatalWarnings.OrderBy(W => W, StringComparer.Ordinal));
        Inputs.Add("|");
        Inputs.AddRange(TargetRules.Warnings.Entries.Select(Pair => $"{Pair.Key}={Pair.Value}"));

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

            // LuminaSharp compiles only the engine tree's bindings, so a project module's travel with its scripts.
            if (!Directories.IsEngineOwned(Module.Rules.ModuleDirectory))
            {
                if (Module.Rules.CSharpBindingsDirectory.Length == 0)
                {
                    Module.Rules.CSharpBindingsDirectory = Path.Combine(Directories.ProjectRoot!, "Game", "Scripts", "Generated");
                }

                Module.Rules.bRouteCSharpTypeBindings = true;
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
                // Static libraries stay in the intermediate tree, keyed like this module's objects: a per-target
                // directory made every target relink the engine's DLLs against its own third-party copies.
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

    /// <summary>Plugin binaries live under the plugin's own Binaries directory, where the loader looks.</summary>
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

    /// <summary>Settings a module hands its dependents, including what its public dependencies re-export.</summary>
    private sealed class PublicExports
    {
        public List<string> IncludePaths { get; } = new();

        /// <summary>Include paths originating in a third-party module, kept apart so they can go out as -isystem.</summary>
        public List<string> SystemIncludePaths { get; } = new();

        public List<string> Definitions { get; } = new();

        public List<string> SystemLibraries { get; } = new();

        public List<string> AdditionalLibraries { get; } = new();

        public List<string> LibraryPaths { get; } = new();
    }

    /// <summary>Root of a module's headers: the module directory if flat, its Source subdirectory otherwise.</summary>
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

        // A vendored library's headers are not ours to fix, so its exports travel as system paths.
        List<string> OwnIncludePaths = Rules.bIsThirdParty ? Exports.SystemIncludePaths : Exports.IncludePaths;

        foreach (string IncludePath in Rules.PublicIncludePaths)
        {
            OwnIncludePaths.Add(Rules.ModulePath(IncludePath));
        }

        foreach (string IncludePath in Rules.PublicSystemIncludePaths)
        {
            Exports.SystemIncludePaths.Add(Rules.ModulePath(IncludePath));
        }

        // An engine module's source root is public surface; third-party modules declare roots explicitly.
        if (!Rules.bIsThirdParty)
        {
            Exports.IncludePaths.Add(GetSourceRoot(Rules));
        }

        // Plugins also export their containing Source dir, so a dependent can qualify "Plugin/Types.h" when
        // two plugins ship the same header name. Plugins only: a game module's directory is the project root.
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
            Exports.SystemIncludePaths.AddRange(Inherited.SystemIncludePaths);
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
        List<string> SystemIncludePaths = new();
        List<string> Definitions = new();

        foreach (string IncludePath in Rules.PrivateIncludePaths)
        {
            IncludePaths.Add(Rules.ModulePath(IncludePath));
        }

        PublicExports Own = GetPublicExports(Module);
        IncludePaths.AddRange(Own.IncludePaths);
        SystemIncludePaths.AddRange(Own.SystemIncludePaths);
        Definitions.AddRange(Own.Definitions);

        // A module always sees its own source root even when it exports nothing.
        IncludePaths.Add(GetSourceRoot(Rules));

        Definitions.AddRange(Rules.PrivateDefinitions);

        // Direct dependencies contribute their full public closure, public or private edge alike.
        foreach (BuildModule Dependency in Module.AllDependencies)
        {
            PublicExports Inherited = GetPublicExports(Dependency);
            IncludePaths.AddRange(Inherited.IncludePaths);
            SystemIncludePaths.AddRange(Inherited.SystemIncludePaths);
            Definitions.AddRange(Inherited.Definitions);
        }

        Definitions.AddRange(PlatformSupport.GetPlatformDefinitions(Info));
        Definitions.AddRange(TargetRules.GlobalDefinitions);
        Definitions.Add($"WITH_EDITOR={(Info.bWithEditor ? 1 : 0)}");

        // Read here rather than in the rules so it still matches after a target overrides the suffix.
        Definitions.Add($"LUMINA_BINARY_SUFFIX=\"{TargetRules.OutputSuffix}\"");

        if (Module.BinaryType == ModuleBinaryType.SharedLibrary && !TargetRules.bMonolithic)
        {
            Definitions.Add($"{Module.Name.ToUpperInvariant()}_EXPORTS");
        }

        AddModuleApiDefinitions(Module, Definitions);

        AddUnique(Module.CompileIncludePaths, IncludePaths);

        // A directory already reachable as first-party surface stays a -I path: the compiler lets
        // -isystem win over -I for the same directory, which would mute warnings we want to see.
        HashSet<string> Regular = new(Module.CompileIncludePaths, StringComparer.OrdinalIgnoreCase);
        AddUnique(Module.SystemIncludePaths, SystemIncludePaths.Where(Candidate => !Regular.Contains(Candidate)));

        AddUnique(Module.CompileDefinitions, Definitions);

        // Third-party code does not get the engine's force includes; it has no ModuleAPI.h.
        if (!Rules.bIsThirdParty)
        {
            AddUnique(Module.ForceIncludeFiles, Rules.ForceIncludeFiles);
        }
    }

    /// <summary>Defines &lt;NAME&gt;_API so a marked class is dllexport in its own module and dllimport elsewhere.</summary>
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

            // A vendored library owns its own <NAME>_API; Tracy declares TRACY_API and this would redefine it.
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

    /// <summary>Only modules that actually link resolve symbols.</summary>
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

    /// <summary>What a dependent puts on its link line: import library, archive, or nothing.</summary>
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

    /// <summary>Unions every module's runtime dependencies onto the target.</summary>
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
