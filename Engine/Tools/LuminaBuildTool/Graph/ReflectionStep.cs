using System.Text.Json;
using System.Text.Json.Serialization;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>
/// One entry in the Reflector's input document. Property names are the wire format the Reflector
/// parses and cannot be renamed independently of it.
/// </summary>
public sealed class ReflectionProjectEntry
{
    public string Name { get; set; } = string.Empty;

    /// <summary>Module base directory. The Reflector prefix-matches headers against it.</summary>
    public string Path { get; set; } = string.Empty;

    public List<string> IncludeDirs { get; set; } = new();

    public List<string> Files { get; set; } = new();

    public string CSharpBindingsDir { get; set; } = string.Empty;

    /// <summary>Where the generator writes this module's generated C++.</summary>
    public string GeneratedDir { get; set; } = string.Empty;

    /// <summary>
    /// Precompiled header the generated sources open with, or empty when the module has none.
    /// A PCH is named after the module that owns it, so the generator is told rather than
    /// guessing a name that only resolves while some other module is on the include path.
    /// </summary>
    public string PrecompiledHeader { get; set; } = string.Empty;

    /// <summary>
    /// Parsed for type discovery but never generated for. Set on the engine's modules when a game
    /// or plugin workspace pulls them in, so an engine base class is known without being emitted.
    /// </summary>
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

/// <summary>
/// Manifest of the engine's reflected modules, written when the engine builds and read by game
/// and plugin workspaces. Without it a downstream module cannot tell that an engine base class is
/// reflected, and silently emits a broken type with a null super struct.
/// </summary>
public sealed class EngineReflectionManifest
{
    public List<ReflectionProjectEntry> Projects { get; set; } = new();
}

/// <summary>
/// Wires the reflection code generator into the action graph. Produces one Generate action per
/// target whose outputs are the generated unity shards the reflected modules compile.
/// </summary>
public static class ReflectionStep
{
    /// <summary>
    /// Must match kUnityShardCount in Reflector/CodeGeneration/CodeGenerator.cpp. The Reflector
    /// emits exactly this many shards per reflected module, stubbing the empty ones.
    /// </summary>
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

    /// <summary>
    /// Result of planning the reflection step: the generator plus the actions that materialize
    /// the files it reads and the manifest downstream projects read.
    /// </summary>
    public sealed class ReflectionActions
    {
        public required BuildAction Generate { get; init; }

        public required List<BuildAction> Inputs { get; init; }
    }

    /// <summary>
    /// Plans the reflection step, or returns null when the target has no reflected modules.
    /// </summary>
    /// <remarks>
    /// The generator's input document and the engine manifest are produced by actions rather than
    /// written while the graph is being built. Planning a build must not touch the filesystem, or
    /// a dry run would mutate the tree it is only meant to describe, and the input document's
    /// timestamp would be decided by the very pass that reasons about it.
    /// </remarks>
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
                IncludeDirs = Module.CompileIncludePaths.ToList(),
                Files = Module.Sources.HeaderFiles.Select(H => H.Location).ToList(),
                CSharpBindingsDir = Module.Rules.CSharpBindingsDirectory,
                GeneratedDir = Module.GeneratedCodeDirectory,
                PrecompiledHeader = Module.Rules.PrecompiledHeader?.Header ?? string.Empty,
            });
        }

        List<BuildAction> InputActions = new();
        bool bIsEngineWorkspace = Target.Directories.ProjectRoot is null;

        if (bIsEngineWorkspace)
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
            Arguments = PathUtils.Quote(InputFile),
            WorkingDirectory = Target.Directories.OutputRoot,

            // The generator holds every header in memory at once; running one is already
            // internally parallel and a second instance would fight it for cores.
            bCanExecuteInParallel = false,
        };

        Action.PrerequisiteItems.Add(FileItem.Get(InputFile));
        Action.PrerequisiteItems.Add(FileItem.Get(ReflectorPath));

        foreach (BuildModule Module in Reflected)
        {
            foreach (FileItem Header in Module.Sources.HeaderFiles)
            {
                Action.PrerequisiteItems.Add(Header);
            }

            // A project's build does not own the engine's generated code. Engine modules write
            // into the shared engine tree, so if both an engine target and a project target
            // claimed those files, each would record its own command against them and every build
            // would find the other's record and regenerate. Only the owner declares them, which
            // leaves the shared copy stable for everyone reading it.
            bool bOwnsOutput = Target.Directories.IsEngineOwned(Module.Rules.ModuleDirectory)
                == (Target.Directories.ProjectRoot is null);

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

    /// <summary>
    /// Publishes the engine's reflected module set for downstream projects. Its identity is its
    /// content, so a build that changes nothing leaves the file untouched and does not make every
    /// game project look stale.
    /// </summary>
    private static BuildAction CreateEngineManifestAction(BuildTarget Target, IEnumerable<ReflectionProjectEntry> Projects)
    {
        EngineReflectionManifest Manifest = new()
        {
            Projects = Projects.Select(P => new ReflectionProjectEntry
            {
                Name = P.Name,
                Path = P.Path,
                IncludeDirs = P.IncludeDirs,
                Files = P.Files,

                // A downstream build never emits the engine's bindings.
                CSharpBindingsDir = string.Empty,
                ReferenceOnly = true,
            }).ToList(),
        };

        string ManifestPath = Path.Combine(Target.Directories.ReflectionDirectory, "EngineModules.json");

        BuildAction Action = new(ActionType.Generate, "Reflection")
        {
            StatusText = Path.GetFileName(ManifestPath),
            Operation = new WriteFileOperation(ManifestPath, Serialize(Manifest)),
        };

        Action.ProducedItems.Add(FileItem.Get(ManifestPath));

        return Action;
    }

    private static void AppendEngineReferenceProjects(BuildTarget Target, ReflectionInputDocument Document)
    {
        string ManifestPath = Path.Combine(
            Target.Directories.EngineRoot, "Intermediates", "Reflection", "EngineModules.json");

        EngineReflectionManifest? Manifest = JsonStore.Load<EngineReflectionManifest>(ManifestPath);

        if (Manifest is null)
        {
            throw new BuildException(
                $"The engine reflection manifest at '{ManifestPath}' is missing or unreadable. "
                + "Build the engine once before building a project against it; the manifest is what lets "
                + "a project module derive from and reference engine reflected types.");
        }

        foreach (ReflectionProjectEntry Project in Manifest.Projects)
        {
            Project.ReferenceOnly = true;
            Project.CSharpBindingsDir = string.Empty;
            Document.Projects.Add(Project);
        }
    }
}
