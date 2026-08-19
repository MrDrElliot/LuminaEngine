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

public sealed class ReflectionInputDocument
{
    public string WorkspaceName { get; set; } = string.Empty;

    /// <summary>Root the Reflector writes Intermediates/Reflection and CSharpBindings under.</summary>
    public string WorkspacePath { get; set; } = string.Empty;

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
    /// <summary>Must match kUnityShardCount in Reflector/CodeGeneration/CodeGenerator.cpp.</summary>
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

    /// <summary>Planned reflection step: the generator, its input actions, and the downstream manifest.</summary>
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
            WorkspaceName = Target.Name,
            WorkspacePath = Target.Directories.OutputRoot,
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

        // Decided by where the Target.cs lives, not by -Project: a game solution builds the engine target
        // with -Project set, which used to make the engine's own build publish no manifest.
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

        // Per target: an Editor and a Game build reflect different module sets, and a shared file
        // would make each one look like a changed input to the other.
        string InputFile = Path.Combine(Target.IntermediateDirectory, "Reflection_Files.json");

        BuildAction WriteInput = new(ActionType.Generate, "Reflection")
        {
            StatusText = Path.GetFileName(InputFile),
            Operation = new WriteFileOperation(InputFile, Serialize(Document)),
        };

        WriteInput.ProducedItems.Add(FileItem.Get(InputFile));
        InputActions.Add(WriteInput);

        BuildAction Action = new(ActionType.Generate, "Reflection")
        {
            StatusText = $"{Reflected.Count} modules",
            ToolPath = ReflectorPath,
            // A clang error in a reflected header silently generates wrong data, so it fails the build.
            Arguments = PathUtils.Quote(InputFile) + " -strict-parse",
            WorkingDirectory = Target.Directories.OutputRoot,

            // The generator holds every header in memory at once; running one is already
            // internally parallel and a second instance would fight it for cores.
            bCanExecuteInParallel = false,

            // The per-type headers, sources and C# bindings are written too but not declared.
            bWritesUndeclaredOutputs = true,
        };

        Action.PrerequisiteItems.Add(FileItem.Get(InputFile));
        Action.PrerequisiteItems.Add(FileItem.Get(ReflectorPath));

        foreach (BuildModule Module in Reflected)
        {
            foreach (FileItem Header in Module.Sources.HeaderFiles)
            {
                Action.PrerequisiteItems.Add(Header);
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

        return new ReflectionActions { Generate = Action, Inputs = InputActions };
    }

    /// <summary>An .inl/.ipp is an include fragment, so parsing it as its own file misreads X-macro bodies.</summary>
    private static bool IsParseableHeader(FileItem Header)
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
    private static string EngineManifestPath(BuildTarget Target)
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
    private static void AppendEngineReferenceProjects(BuildTarget Target, ReflectionInputDocument Document)
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
