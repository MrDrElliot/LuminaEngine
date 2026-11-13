using System.Text.Json;
using System.Text.Json.Serialization;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>One entry in the Reflector's input document.</summary>
public sealed class ReflectionProjectEntry
{
    public string Name { get; set; } = string.Empty;

    /// <summary>Module base directory. The Reflector prefix-matches headers against it.</summary>
    public string Path { get; set; } = string.Empty;

    public List<string> IncludeDirs { get; set; } = new();

    /// <summary>The module's real compile definitions, so the Reflector sees the same code the compiler does.</summary>
    public List<string> Definitions { get; set; } = new();

    /// <summary>Headers the compiler force-includes ahead of every source file in this module.</summary>
    public List<string> ForceIncludes { get; set; } = new();

    public List<string> Files { get; set; } = new();

    public string CSharpBindingsDir { get; set; } = string.Empty;

    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingDefault)]
    public bool RouteTypeBindings { get; set; }

    /// <summary>Where the generator writes this module's generated C++.</summary>
    public string GeneratedDir { get; set; } = string.Empty;

    /// <summary>Precompiled header the generated sources open with, or empty when the module has none.</summary>
    public string PrecompiledHeader { get; set; } = string.Empty;

    /// <summary>Parsed for type discovery but never generated for.</summary>
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingDefault)]
    public bool ReferenceOnly { get; set; }
}

// A Reflection module's library the Reflector loads, with the hash that reruns the step when it is rebuilt.
public sealed record ReflectionGeneratorEntry(string Library, string Fingerprint, string RulesFile, IReadOnlyDictionary<string, string> Options);

public sealed class ReflectionInputDocument
{
    public int CodeGenerationVersion { get; set; }

    public string ReflectorFingerprint { get; set; } = string.Empty;

    public string CoreReflectionFingerprint { get; set; } = string.Empty;

    public string WorkspaceName { get; set; } = string.Empty;

    /// <summary>Root the Reflector writes Intermediates/Reflection and CSharpBindings under.</summary>
    public string WorkspacePath { get; set; } = string.Empty;

    public List<ReflectionGeneratorEntry> Generators { get; set; } = new();

    public List<ReflectionProjectEntry> Projects { get; set; } = new();
}

/// <summary>Manifest of the engine's reflected modules, written on engine builds, read by projects.</summary>
public sealed class EngineReflectionManifest
{
    public List<ReflectionProjectEntry> Projects { get; set; } = new();
}

/// <summary>Wires the reflection code generator into the action graph.</summary>
public static class ReflectionStep
{
    private const int CodeGenerationVersion = 3;

    /// <summary>Must match kUnityShardCount in Reflector/CodeGeneration/CppReflectionGenerator.cpp.</summary>
    public const int UnityShardCount = 8;

    public static string GetUnityShardPath(string GeneratedCodeDirectory, int Shard)
    {
        return Path.Combine(GeneratedCodeDirectory, $"ReflectionUnity_{Shard}.gen.cpp");
    }

    public static IEnumerable<string> EnumerateUnityShardPaths(string GeneratedCodeDirectory)
    {
        for (int Shard = 0; Shard < UnityShardCount; Shard++)
        {
            yield return GetUnityShardPath(GeneratedCodeDirectory, Shard);
        }
    }

    /// <summary>Planned reflection step, with the generator, its input actions, and the downstream manifest.</summary>
    public sealed class ReflectionActions
    {
        public required BuildAction Generate { get; init; }

        public required List<BuildAction> Inputs { get; init; }
    }

    /// <summary>Plans the reflection step, or returns null when the target has no reflected modules.</summary>
    public static ReflectionActions? CreateActions(BuildTarget Target)
    {
        List<BuildModule> Reflected = Target.Modules.Where(M => M.Rules.bEnableReflection).ToList();

        if (Reflected.Count == 0)
        {
            return null;
        }

        string ReflectorPath = ResolveReflectorPath(Target);

        ReflectionInputDocument Document = new()
        {
            CodeGenerationVersion = CodeGenerationVersion,
            ReflectorFingerprint = ContentHash.OfFileContents(ReflectorPath),
            CoreReflectionFingerprint = ComputeCoreReflectionFingerprint(Target),
            WorkspaceName = Target.Name,
            WorkspacePath = Target.Directories.OutputRoot,
            Generators = Target.ReflectionGenerators.Select(Generator => new ReflectionGeneratorEntry(
                Generator.LibraryPath,
                ContentHash.OfFileContents(RequireGeneratorLibrary(Generator)),
                Generator.Rules.RulesFile,
                Generator.Rules.ReflectionGeneratorOptions)).ToList(),
        };

        foreach (BuildModule Module in Reflected)
        {
            Document.Projects.Add(new ReflectionProjectEntry
            {
                Name = Module.Name,
                Path = Module.Rules.ModuleDirectory,
                IncludeDirs = Module.AllIncludePaths.ToList(),
                Definitions = Module.CompileDefinitions.ToList(),
                ForceIncludes = Module.ForceIncludeFiles.ToList(),
                Files = Module.Sources.HeaderFiles.Where(IsParseableHeader).Select(H => H.Location).ToList(),
                CSharpBindingsDir = Module.Rules.CSharpBindingsDirectory,
                RouteTypeBindings = Module.Rules.bRouteCSharpTypeBindings,
                GeneratedDir = Module.GeneratedCodeDirectory,
                PrecompiledHeader = Module.Rules.PrecompiledHeader?.Header ?? string.Empty,
            });
        }

        List<BuildAction> InputActions = new();

        // Decided by where the Target.cs lives rather than by -Project, since a game solution builds the engine target too.
        bool bIsEngineTarget = Target.Directories.IsEngineOwned(Target.Rules.RulesDirectory);

        if (bIsEngineTarget)
        {
            if (Target.Rules.bPublishesEngineReflectionManifest)
            {
                InputActions.Add(CreateEngineManifestAction(Target, Document.Projects));
            }
        }
        else
        {
            AppendEngineReferenceProjects(Target, Document);
        }

        // Streamed over stdin rather than written to a file, so its content is part of the command key and a change reruns the step.
        BuildAction Action = new(ActionType.Generate, "Reflection")
        {
            StatusText = $"{Reflected.Count} modules",
            ToolPath = ReflectorPath,
            // A clang error in a reflected header silently generates wrong data, so it fails the build.
            Arguments = "-strict-parse",
            StandardInput = ReflectorInput.Write(Document),
            WorkingDirectory = Target.Directories.OutputRoot,

            // The generator holds every header in memory and is already internally parallel, so a second instance fights it.
            bCanExecuteInParallel = false,

            // The per-type headers, sources and C# bindings are written too but not declared.
            bWritesUndeclaredOutputs = true,
        };

        Action.PrerequisiteItems.Add(FileItem.Get(ReflectorPath));

        foreach (BuildModule Module in Reflected)
        {
            foreach (FileItem Header in Module.Sources.HeaderFiles)
            {
                Action.PrerequisiteItems.Add(Header);
            }

            // The bindings are shared across configurations, so a stamp another configuration wrote reruns the reflector to re-emit them.
            string LayoutStamp = GetLayoutStampPath(Target, Module);
            if (File.Exists(LayoutStamp))
            {
                Action.PrerequisiteItems.Add(FileItem.Get(LayoutStamp));
            }

            // Only the owner declares the shared engine files; two claimants would each regenerate the other's.
            bool bOwnsOutput = Target.Directories.IsEngineOwned(Module.Rules.ModuleDirectory) == bIsEngineTarget;

            foreach (string Shard in EnumerateUnityShardPaths(Module.GeneratedCodeDirectory))
            {
                if (bOwnsOutput)
                {
                    Action.ProducedItems.Add(FileItem.Get(Shard));
                }
                else
                {
                    Action.OptionalProducedItems.Add(FileItem.Get(Shard));
                }
            }
        }

        foreach (ReflectionProjectEntry Project in Document.Projects.Where(Project => Project.ReferenceOnly))
        {
            foreach (string Header in Project.Files)
            {
                Action.PrerequisiteItems.Add(FileItem.Get(Header));
            }
        }

        // Every target of one type shares the history file and the engine shards, so each needs its own record.
        Action.HistoryKey = $"Reflection/{Target.Name}";

        return new ReflectionActions { Generate = Action, Inputs = InputActions };
    }

    // Must match GeneratedFiles::LayoutStampPath in Reflector/CodeGeneration/GeneratedFiles.h.
    internal static string GetLayoutStampPath(BuildTarget Target, BuildModule Module)
    {
        string Directory = Module.Rules.bRouteCSharpTypeBindings && !string.IsNullOrEmpty(Module.Rules.CSharpBindingsDirectory)
            ? Module.Rules.CSharpBindingsDirectory
            : Path.Combine(Target.Directories.OutputRoot, "Intermediates", "CSharpBindings", Module.Name);
        return Path.Combine(Directory, Module.Name + ".Layout.generated.cs");
    }

    /// <summary>An .inl/.ipp is an include fragment, so parsing it as its own file misreads X-macro bodies.</summary>
    internal static bool IsParseableHeader(FileItem Header)
    {
        string Extension = Path.GetExtension(Header.Location);

        return !Extension.Equals(".inl", StringComparison.OrdinalIgnoreCase)
            && !Extension.Equals(".ipp", StringComparison.OrdinalIgnoreCase);
    }

    private static string Serialize<T>(T Value)
    {
        return JsonSerializer.Serialize(Value, new JsonSerializerOptions
        {
            WriteIndented = true,
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingDefault,
        });
    }

    private static string ComputeCoreReflectionFingerprint(BuildTarget Target)
    {
        string Root = Path.Combine(Target.Directories.EngineRuntimeDirectory, "Source", "Core", "Reflection");
        if (!Directory.Exists(Root))
        {
            return string.Empty;
        }

        return ContentHash.OfStrings(Directory.EnumerateFiles(Root, "*", SearchOption.AllDirectories)
            .OrderBy(FilePath => FilePath, StringComparer.OrdinalIgnoreCase)
            .Select(FilePath => Path.GetRelativePath(Root, FilePath) + ContentHash.OfFileContents(FilePath)));
    }

    // Built by the Reflector's target, which runs before this one, so a missing library means that build did not include it.
    private static string RequireGeneratorLibrary(ReflectionGenerator Generator)
    {
        if (!File.Exists(Generator.LibraryPath))
        {
            throw new BuildException(
                $"Reflection module '{Generator.Rules.Name}' has no library at '{Generator.LibraryPath}'. "
                + "The Reflector target builds it, so build that target first, and check the module is not turned off by name.");
        }

        return Generator.LibraryPath;
    }

    private static string ResolveReflectorPath(BuildTarget Target)
    {
        string Extension = Target.Info.Platform.GetExecutableExtension();

        // Always the engine's Reflector, even for a game project building against an install.
        string ReflectorPath = Path.Combine(
            Target.Directories.EngineRoot,
            "Binaries",
            Target.Info.PlatformName,
            "Reflector" + Extension);

        if (!File.Exists(ReflectorPath))
        {
            throw new BuildException(
                $"The reflection generator is missing at '{ReflectorPath}'. "
                + "Build the Reflector target first, or check that the engine's External dependencies are installed.");
        }

        return ReflectorPath;
    }

    /// <summary>Publishes the engine's reflected module set for downstream projects.</summary>
    private static BuildAction CreateEngineManifestAction(BuildTarget Target, IEnumerable<ReflectionProjectEntry> Projects)
    {
        EngineReflectionManifest Manifest = new()
        {
            Projects = Projects.Select(P => new ReflectionProjectEntry
            {
                Name = P.Name,
                Path = P.Path,
                IncludeDirs = P.IncludeDirs,
                Definitions = P.Definitions,
                ForceIncludes = P.ForceIncludes,
                Files = P.Files,

                // A downstream build never emits the engine's bindings.
                CSharpBindingsDir = string.Empty,
                ReferenceOnly = true,
            }).ToList(),
        };

        string ManifestPath = EngineManifestPath(Target);

        BuildAction Action = new(ActionType.Generate, "Reflection")
        {
            StatusText = Path.GetFileName(ManifestPath),
            Operation = new WriteFileOperation(ManifestPath, Serialize(Manifest)),
        };

        Action.ProducedItems.Add(FileItem.Get(ManifestPath));

        return Action;
    }

    /// <summary>Where the engine publishes its reflected module set, always under the engine tree.</summary>
    internal static string EngineManifestPath(BuildTarget Target)
    {
        return Path.Combine(
            Target.Directories.EngineRoot,
            "Intermediates",
            "Reflection",
            Target.Info.PlatformName,
            $"{Target.Info.Type}-{Target.Info.Configuration}",
            "EngineModules.json");
    }

    /// <summary>Adds engine reflected modules this target does not build, so project types can derive from them.</summary>
    internal static void AppendEngineReferenceProjects(BuildTarget Target, ReflectionInputDocument Document)
    {
        string ManifestPath = EngineManifestPath(Target);
        EngineReflectionManifest? Manifest = JsonStore.Load<EngineReflectionManifest>(ManifestPath);

        if (Manifest is null)
        {
            Log.Verbose(
                "No engine reflection manifest at '{0}'; this target's own modules cover the engine types it references.",
                ManifestPath);

            return;
        }

        HashSet<string> Present = new(Document.Projects.Select(P => P.Name), StringComparer.OrdinalIgnoreCase);

        foreach (ReflectionProjectEntry Project in Manifest.Projects)
        {
            // Passing the generator one module twice leaves which entry wins up to parse order.
            if (!Present.Add(Project.Name))
            {
                continue;
            }

            Project.ReferenceOnly = true;
            Project.CSharpBindingsDir = string.Empty;
            Document.Projects.Add(Project);
        }
    }
}
