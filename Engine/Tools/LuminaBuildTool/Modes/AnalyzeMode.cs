using System.Text;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Execution;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.Modes;

/// <summary>
/// Reports on the include graph the last build recorded.
/// </summary>
/// <remarks>
/// The compiler already tells us the exact header closure of every translation unit, and
/// <see cref="DependencyCache"/> already keeps it so that editing a header rebuilds precisely the
/// objects that read it. That same data answers questions nobody was asking it: which header costs
/// the most to touch, and whether a module's declared dependencies match the ones it actually
/// reaches into. Both are measurements rather than opinions, and both were previously guesses.
/// </remarks>
public static class AnalyzeMode
{
    /// <summary>A module's compiled outputs and the include closures recorded against them.</summary>
    private sealed class ModuleClosures
    {
        public required BuildModule Module { get; init; }

        /// <summary>One entry per translation unit that has a recorded closure.</summary>
        public List<string[]> TranslationUnits { get; } = new();
    }

    private sealed class AnalysisContext
    {
        public required BuildTarget Target { get; init; }

        public required Dictionary<BuildModule, ModuleClosures> ByModule { get; init; }

        /// <summary>Header path to the module whose tree it lives in, for headers we own.</summary>
        public required Dictionary<string, BuildModule> HeaderOwners { get; init; }

        public int TranslationUnitCount => ByModule.Values.Sum(C => C.TranslationUnits.Count);
    }

    public static int RunIncludes(CommandLine Arguments, BuildDirectories Directories)
    {
        AnalysisContext? Context = Prepare(Arguments, Directories);

        if (Context is null)
        {
            return 1;
        }

        string? ModuleFilter = Arguments.GetString("Module");
        int Top = Arguments.GetInt("Top", 40);
        bool bAll = Arguments.HasFlag("All");

        IEnumerable<ModuleClosures> Scope = Context.ByModule.Values;

        if (!string.IsNullOrEmpty(ModuleFilter))
        {
            Scope = Scope.Where(C => string.Equals(C.Module.Name, ModuleFilter, StringComparison.OrdinalIgnoreCase));

            if (!Scope.Any())
            {
                Log.Error("Module '{0}' has no recorded translation units in this target.", ModuleFilter);
                return 1;
            }
        }

        List<ModuleClosures> Closures = Scope.ToList();
        int Total = Closures.Sum(C => C.TranslationUnits.Count);

        // Counted once per translation unit, not once per appearance: a header pulled in through
        // five different paths still only costs one recompile of that object.
        Dictionary<string, int> FanIn = new(StringComparer.OrdinalIgnoreCase);

        foreach (ModuleClosures Closure in Closures)
        {
            foreach (string[] Includes in Closure.TranslationUnits)
            {
                foreach (string Header in Includes.Distinct(StringComparer.OrdinalIgnoreCase))
                {
                    // Toolchain and SDK headers dominate the ranking and cannot be changed, so
                    // they are off by default; -All puts them back for the curious.
                    if (!bAll && !Context.HeaderOwners.ContainsKey(Header))
                    {
                        continue;
                    }

                    FanIn.TryGetValue(Header, out int Count);
                    FanIn[Header] = Count + 1;
                }
            }
        }

        StringBuilder Report = new();
        Report.AppendLine($"Include fan-in for {Context.Target.Name} ({Context.Target.Info.Type}-{Context.Target.Info.Configuration})");
        Report.AppendLine(string.IsNullOrEmpty(ModuleFilter)
            ? $"  {Total} translation units across {Closures.Count} modules, {FanIn.Count} headers"
            : $"  {Total} translation units in {ModuleFilter}, {FanIn.Count} headers");

        AppendModuleRollup(Report, Context, Closures, Total, bAll);

        Report.AppendLine();
        Report.AppendLine("Individual headers:");
        Report.AppendLine();
        Report.AppendLine("  Reached by  Header");

        foreach ((string Header, int Count) in FanIn.OrderByDescending(P => P.Value).ThenBy(P => P.Key, StringComparer.OrdinalIgnoreCase).Take(Top))
        {
            double Percent = Total == 0 ? 0.0 : 100.0 * Count / Total;
            string Owner = Context.HeaderOwners.TryGetValue(Header, out BuildModule? Module) ? Module.Name : "(external)";

            Report.AppendLine($"  {Percent,5:F1}% {Count,5}  [{Owner}] {Describe(Context, Header)}");
        }

        if (FanIn.Count > Top)
        {
            Report.AppendLine($"  ... {FanIn.Count - Top} more; -Top=<n> to see further, -Module=<name> to scope to one module.");
        }

        Report.AppendLine();
        Report.AppendLine("  A header near the top of this list and outside the precompiled header is the");
        Report.AppendLine("  cheapest clean-build win available. One near the bottom that sits inside a PCH is");
        Report.AppendLine("  weight every translation unit pays and almost none of them use.");

        Log.Raw(Report.ToString());

        return 0;
    }

    /// <summary>
    /// Rolls the ranking up to the module that owns each header, because a library is what you
    /// decide about. Twenty equally-included headers from one vendored library are one fact, and
    /// listing them individually buries the rest of the report under it.
    /// </summary>
    private static void AppendModuleRollup(
        StringBuilder Report,
        AnalysisContext Context,
        IReadOnlyList<ModuleClosures> Closures,
        int Total,
        bool bAll)
    {
        Dictionary<string, int> ReachCount = new(StringComparer.OrdinalIgnoreCase);
        Dictionary<string, HashSet<string>> HeaderCount = new(StringComparer.OrdinalIgnoreCase);

        foreach (ModuleClosures Closure in Closures)
        {
            foreach (string[] Includes in Closure.TranslationUnits)
            {
                HashSet<string> Owners = new(StringComparer.OrdinalIgnoreCase);

                foreach (string Header in Includes)
                {
                    string Owner = Context.HeaderOwners.TryGetValue(Header, out BuildModule? Module)
                        ? Module.Name
                        : "(external)";

                    if (!bAll && Owner == "(external)")
                    {
                        continue;
                    }

                    // Reaching into a module counts once for the translation unit, however many
                    // of its headers that unit opened.
                    Owners.Add(Owner);

                    if (!HeaderCount.TryGetValue(Owner, out HashSet<string>? Headers))
                    {
                        Headers = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                        HeaderCount[Owner] = Headers;
                    }

                    Headers.Add(Header);
                }

                foreach (string Owner in Owners)
                {
                    ReachCount.TryGetValue(Owner, out int Count);
                    ReachCount[Owner] = Count + 1;
                }
            }
        }

        Report.AppendLine();
        Report.AppendLine("Reach by owning module:");
        Report.AppendLine();

        foreach ((string Owner, int Count) in ReachCount.OrderByDescending(P => P.Value).ThenBy(P => P.Key, StringComparer.OrdinalIgnoreCase))
        {
            double Percent = Total == 0 ? 0.0 : 100.0 * Count / Total;

            Report.AppendLine($"  {Percent,5:F1}% {Count,5} TUs  {Owner} ({HeaderCount[Owner].Count} headers)");
        }
    }

    public static int RunDependencies(CommandLine Arguments, BuildDirectories Directories)
    {
        AnalysisContext? Context = Prepare(Arguments, Directories);

        if (Context is null)
        {
            return 1;
        }

        StringBuilder Report = new();
        Report.AppendLine($"Declared versus reached dependencies for {Context.Target.Name} ({Context.Target.Info.Type}-{Context.Target.Info.Configuration})");
        Report.AppendLine();

        List<string> Unreached = new();
        List<string> Undeclared = new();

        foreach (BuildModule Module in Context.Target.Modules)
        {
            if (!Context.ByModule.TryGetValue(Module, out ModuleClosures? Closure) || Closure.TranslationUnits.Count == 0)
            {
                continue;
            }

            HashSet<BuildModule> Reached = new();

            foreach (string[] Includes in Closure.TranslationUnits)
            {
                foreach (string Header in Includes)
                {
                    if (Context.HeaderOwners.TryGetValue(Header, out BuildModule? Owner) && Owner != Module)
                    {
                        Reached.Add(Owner);
                    }
                }
            }

            // Build-order dependencies are neither linked nor included, so they belong in neither
            // column; naming one is a statement about ordering, not about code.
            List<BuildModule> Declared = Module.AllDependencies.Distinct().ToList();

            foreach (BuildModule Dependency in Declared.Where(D => !Reached.Contains(D)).OrderBy(D => D.Name, StringComparer.OrdinalIgnoreCase))
            {
                string Note = Dependency.BinaryType == ModuleBinaryType.HeaderOnly
                    ? " (header only, so nothing to link either)"
                    : string.Empty;

                Unreached.Add($"  {Module.Name} -> {Dependency.Name}{Note}");
            }

            foreach (BuildModule Dependency in Reached.Where(R => !Declared.Contains(R)).OrderBy(D => D.Name, StringComparer.OrdinalIgnoreCase))
            {
                // Name the direct dependency that re-exports it, because that is the edge whose
                // removal would break this module.
                BuildModule? Through = Declared.FirstOrDefault(D => D.EnumerateDependencyClosure().Contains(Dependency));

                Undeclared.Add($"  {Module.Name} -> {Dependency.Name}"
                    + (Through is not null ? $" (visible only through {Through.Name})" : string.Empty));
            }
        }

        Report.AppendLine($"Declared but no headers included ({Unreached.Count}):");
        Report.AppendLine();

        if (Unreached.Count == 0)
        {
            Report.AppendLine("  (none)");
        }
        else
        {
            Unreached.ForEach(Line => Report.AppendLine(Line));
            Report.AppendLine();
            Report.AppendLine("  Candidates for deletion, not confirmed ones. A dependency can be needed purely");
            Report.AppendLine("  to link: an allocator, a static library whose symbols are referenced through a");
            Report.AppendLine("  header this module never opens itself. Check what breaks before pruning.");
        }

        Report.AppendLine();
        Report.AppendLine($"Headers included from an undeclared module ({Undeclared.Count}):");
        Report.AppendLine();

        if (Undeclared.Count == 0)
        {
            Report.AppendLine("  (none)");
        }
        else
        {
            Undeclared.ForEach(Line => Report.AppendLine(Line));
            Report.AppendLine();
            Report.AppendLine("  These compile today because something else re-exports them. Declaring the");
            Report.AppendLine("  dependency makes the reliance explicit and survives the day that re-export is");
            Report.AppendLine("  tidied away.");
            Report.AppendLine();
            Report.AppendLine("  One caveat on attribution: a per-image source such as EASTLImpl.cpp is compiled");
            Report.AppendLine("  into every module, and its include closure is charged to whichever module");
            Report.AppendLine("  compiled it. An entry with no re-exporting module named is usually that, rather");
            Report.AppendLine("  than the module's own code reaching somewhere it should not.");
        }

        Log.Raw(Report.ToString());

        return 0;
    }

    /// <summary>
    /// Resolves the target, loads the recorded include graph and attributes both objects and
    /// headers back to the modules they belong to.
    /// </summary>
    /// <remarks>
    /// Attribution is by longest matching directory rather than by asking the toolchain to name
    /// object files again. The cache is keyed on output paths, and an output path already says
    /// which module produced it; recomputing the names would be a second implementation of a
    /// mapping that has to agree with the first.
    /// </remarks>
    private static AnalysisContext? Prepare(CommandLine Arguments, BuildDirectories Directories)
    {
        string? TargetName = Arguments.GetPositional(1);

        if (string.IsNullOrEmpty(TargetName))
        {
            Log.Error(
                "A target name is required. Usage: LuminaBuildTool {0} <Target> [options]",
                Arguments.GetPositional(0) ?? "Includes");
            return null;
        }

        RulesAssembly Assembly = RulesAssembly.Create(Directories, Arguments.HasFlag("RecompileRules"));

        BuildPlatform PlatformValue = Arguments.GetEnum("Platform", BuildPlatformRegistry.HostPlatform);
        BuildConfiguration ConfigurationValue = Arguments.GetEnum("Configuration", BuildConfiguration.Development);
        TargetType TypeValue = Arguments.GetEnum("TargetType", TargetType.Editor);

        IBuildPlatform PlatformSupport = BuildPlatformRegistry.Get(PlatformValue);
        BuildOptions Options = BuildOptions.Load(Directories, Arguments);

        TargetInfo Info = new(TargetName, TypeValue, PlatformValue, ConfigurationValue, Directories, Options);
        BuildTarget Target = new TargetAssembler(Assembly, Directories, PlatformSupport).Assemble(TargetName, Info);

        string CacheDirectory = Directories.BuildRecordDirectory(PlatformValue, ConfigurationValue, TypeValue);
        DependencyCache Cache = DependencyCache.Load(Path.Combine(CacheDirectory, "Dependencies.json"));

        Dictionary<BuildModule, ModuleClosures> ByModule = new();
        Dictionary<string, BuildModule> HeaderOwners = new(StringComparer.OrdinalIgnoreCase);

        // Longest first, so a nested module directory wins over the tree that contains it.
        List<(string Directory, BuildModule Module)> Roots = new();

        foreach (BuildModule Module in Target.Modules)
        {
            ByModule[Module] = new ModuleClosures { Module = Module };

            Roots.Add((Module.Rules.ModuleDirectory, Module));

            if (Module.GeneratedCodeDirectory.Length > 0)
            {
                Roots.Add((Module.GeneratedCodeDirectory, Module));
            }
        }

        Roots.Sort((Left, Right) => Right.Directory.Length.CompareTo(Left.Directory.Length));

        BuildModule? FindOwner(string FilePath)
        {
            foreach ((string Directory, BuildModule Module) in Roots)
            {
                if (PathUtils.IsUnder(FilePath, Directory))
                {
                    return Module;
                }
            }

            return null;
        }

        int Recorded = 0;

        foreach ((string Output, string[] Includes) in Cache.EnumerateRecorded())
        {
            BuildModule? Owner = Target.Modules
                .Where(M => M.IntermediateDirectory.Length > 0 && PathUtils.IsUnder(Output, M.IntermediateDirectory))
                .OrderByDescending(M => M.IntermediateDirectory.Length)
                .FirstOrDefault();

            if (Owner is null)
            {
                continue;
            }

            ByModule[Owner].TranslationUnits.Add(Includes);
            Recorded++;

            foreach (string Header in Includes)
            {
                if (!HeaderOwners.ContainsKey(Header))
                {
                    BuildModule? HeaderOwner = FindOwner(Header);

                    if (HeaderOwner is not null)
                    {
                        HeaderOwners[Header] = HeaderOwner;
                    }
                }
            }
        }

        if (Recorded == 0)
        {
            Log.Error(
                "No include graph has been recorded for {0} {1}-{2}. Build it once first; the graph is a by-product of compiling.",
                TargetName,
                TypeValue,
                ConfigurationValue);

            return null;
        }

        Log.Verbose("Analyzing {0} recorded translation units", Recorded);

        return new AnalysisContext
        {
            Target = Target,
            ByModule = ByModule,
            HeaderOwners = HeaderOwners,
        };
    }

    /// <summary>Shortens a path to the part that identifies it, relative to the engine or project.</summary>
    private static string Describe(AnalysisContext Context, string FilePath)
    {
        string?[] Roots = { Context.Target.Directories.ProjectRoot, Context.Target.Directories.EngineRoot };

        foreach (string? Root in Roots)
        {
            if (!string.IsNullOrEmpty(Root) && PathUtils.IsUnder(FilePath, Root))
            {
                return PathUtils.MakeRelativeTo(FilePath, Root);
            }
        }

        return FilePath;
    }
}
