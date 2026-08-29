using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Toolchain;

namespace LuminaBuildTool.Execution;

/// <summary>Every action for a target, wired into a graph by the files they consume and produce.</summary>
public sealed class ActionGraph
{
    private readonly Dictionary<FileItem, BuildAction> ProducerByFile = new();

    private readonly Dictionary<BuildAction, List<BuildAction>> DependenciesByAction = new();

    private readonly Dictionary<BuildAction, List<BuildAction>> DependentsByAction = new();

    private ActionGraph(List<BuildAction> Actions)
    {
        this.Actions = Actions;
    }

    public List<BuildAction> Actions { get; }

    public IReadOnlyList<BuildAction> GetDependencies(BuildAction Action)
    {
        return DependenciesByAction.TryGetValue(Action, out List<BuildAction>? Found)
            ? Found
            : Array.Empty<BuildAction>();
    }

    public IReadOnlyList<BuildAction> GetDependents(BuildAction Action)
    {
        return DependentsByAction.TryGetValue(Action, out List<BuildAction>? Found)
            ? Found
            : Array.Empty<BuildAction>();
    }

    public static ActionGraph Build(BuildTarget Target, IToolchain Toolchain)
    {
        List<BuildAction> Actions = new();
        Dictionary<string, List<BuildAction>> CompileActionsByModule = new(StringComparer.OrdinalIgnoreCase);

        // Blobs are written before any action is created so a compile input is always a file that
        // exists, with no generation step for the executor to order against.
        UnityBuildStep.Prepare(Target);

        // One generator run covers every reflected module in the target; the shards it produces
        // become compile inputs, so ordering falls out of the file-level edges.
        ReflectionStep.ReflectionActions? Reflection = ReflectionStep.CreateActions(Target);
        BuildAction? GenerateReflection = Reflection?.Generate;

        if (Reflection is not null)
        {
            Actions.AddRange(Reflection.Inputs);
            Actions.Add(Reflection.Generate);
        }

        // Modules are already topologically ordered, so a module's dependencies have their link
        // actions registered before anything that needs to order against them.
        Dictionary<string, BuildAction> LinkActionsByModule = new(StringComparer.OrdinalIgnoreCase);

        foreach (BuildModule Module in Target.Modules)
        {
            List<BuildAction> CompileActions = Toolchain.CreateCompileActions(Target, Module);
            CompileActionsByModule[Module.Name] = CompileActions;
            Actions.AddRange(CompileActions);

            // Generated headers are undeclared generator outputs, so any TU that can reach one waits for it.
            // Not only reflected modules: dependencies' generated headers arrive via their public include paths.
            if (GenerateReflection is not null
                && Module.EnumerateDependencyClosure().Any(M => M.Rules.bEnableReflection))
            {
                foreach (BuildAction Compile in CompileActions)
                {
                    Compile.OrderDependencies.Add(GenerateReflection);
                }
            }

            BuildAction? LinkAction = Toolchain.CreateLinkAction(Target, Module, CompileActions);

            if (LinkAction is not null)
            {
                LinkActionsByModule[Module.Name] = LinkAction;
                Actions.Add(LinkAction);
            }
        }

        // Link granularity only; a TU needs its dependencies' headers, which already exist on disk.
        foreach (BuildModule Module in Target.Modules)
        {
            if (!LinkActionsByModule.TryGetValue(Module.Name, out BuildAction? OwnLink))
            {
                continue;
            }

            foreach (BuildModule Dependency in Module.AllOrderedDependencies)
            {
                if (LinkActionsByModule.TryGetValue(Dependency.Name, out BuildAction? DependencyLink))
                {
                    OwnLink.OrderDependencies.Add(DependencyLink);
                }
            }
        }

        Actions.AddRange(CreateRuntimeDependencyCopies(Target));
        Actions.AddRange(CreateProfileRuntimeCopies(Target, Toolchain));
        Actions.AddRange(ManagedProjectStep.CreateActions(Target, GenerateReflection));

        ActionGraph Graph = new(Actions);
        Graph.Wire();

        return Graph;
    }

    /// <summary>Stages the profiling runtime, without which an instrumented binary will not start.</summary>
    private static IEnumerable<BuildAction> CreateProfileRuntimeCopies(BuildTarget Target, IToolchain Toolchain)
    {
        foreach (string SourcePath in Toolchain.GetProfileRuntimeFiles(Target))
        {
            if (!File.Exists(SourcePath))
            {
                Log.Warning("Profiling runtime '{0}' was not found; the instrumented binary will not run.", SourcePath);
                continue;
            }

            string Destination = Path.Combine(Target.BinariesDirectory, Path.GetFileName(SourcePath));

            BuildAction Copy = new(ActionType.Copy, "ProfileRuntime")
            {
                StatusText = Path.GetFileName(SourcePath),
                Operation = new CopyFileOperation(SourcePath, Destination),
            };

            Copy.PrerequisiteItems.Add(FileItem.Get(SourcePath));
            Copy.ProducedItems.Add(FileItem.Get(Destination));

            yield return Copy;
        }
    }

    /// <summary>Stages prebuilt files beside the target's binaries.</summary>
    private static IEnumerable<BuildAction> CreateRuntimeDependencyCopies(BuildTarget Target)
    {
        foreach (RuntimeDependency Dependency in Target.RuntimeDependencies)
        {
            string Destination = Path.Combine(Target.BinariesDirectory, Path.GetFileName(Dependency.SourcePath));

            if (!File.Exists(Dependency.SourcePath))
            {
                if (!Dependency.bOptional)
                {
                    throw new BuildException($"Runtime dependency '{Dependency.SourcePath}' does not exist.");
                }

                // Warning, not Verbose: bOptional covers a DLL held open, not a source that is absent entirely.
                // A missing slang-glslang.dll fails no build and surfaces only as skipped SPIR-V optimisation.
                Log.Warning("Runtime dependency '{0}' does not exist; it will be missing from {1}. " +
                            "Anything that loads it at run time will fail.",
                            Dependency.SourcePath, Target.BinariesDirectory);
                continue;
            }

            BuildAction Copy = new(ActionType.Copy, "RuntimeDependencies")
            {
                StatusText = Path.GetFileName(Dependency.SourcePath),
                Operation = new CopyFileOperation(Dependency.SourcePath, Destination),
                bIgnoreExitCode = Dependency.bOptional,
            };

            Copy.PrerequisiteItems.Add(FileItem.Get(Dependency.SourcePath));
            Copy.ProducedItems.Add(FileItem.Get(Destination));

            yield return Copy;
        }
    }

    private void Wire()
    {
        foreach (BuildAction Action in Actions)
        {
            foreach (FileItem Produced in Action.AllProducedItems)
            {
                if (ProducerByFile.TryGetValue(Produced, out BuildAction? Existing) && Existing != Action)
                {
                    throw new BuildException(
                        $"'{Produced.Location}' is produced by two actions ({Existing.StatusText} and {Action.StatusText}). "
                        + "Two modules most likely claim the same source file.");
                }

                ProducerByFile[Produced] = Action;
            }
        }

        foreach (BuildAction Action in Actions)
        {
            HashSet<BuildAction> Dependencies = new();

            foreach (FileItem Prerequisite in Action.PrerequisiteItems)
            {
                if (ProducerByFile.TryGetValue(Prerequisite, out BuildAction? Producer) && Producer != Action)
                {
                    Dependencies.Add(Producer);
                }
            }

            foreach (BuildAction Ordered in Action.OrderDependencies)
            {
                if (Ordered != Action)
                {
                    Dependencies.Add(Ordered);
                }
            }

            DependenciesByAction[Action] = Dependencies.ToList();

            foreach (BuildAction Dependency in Dependencies)
            {
                if (!DependentsByAction.TryGetValue(Dependency, out List<BuildAction>? Dependents))
                {
                    Dependents = new List<BuildAction>();
                    DependentsByAction[Dependency] = Dependents;
                }

                Dependents.Add(Action);
            }
        }

        DetectCycles();
    }

    private void DetectCycles()
    {
        HashSet<BuildAction> Visited = new();
        HashSet<BuildAction> InProgress = new();
        List<BuildAction> Stack = new();

        void Visit(BuildAction Action)
        {
            if (Visited.Contains(Action))
            {
                return;
            }

            if (!InProgress.Add(Action))
            {
                throw new BuildException(
                    "Cycle in the action graph: " + string.Join(" -> ", Stack.Select(A => A.StatusText)));
            }

            Stack.Add(Action);

            foreach (BuildAction Dependency in GetDependencies(Action))
            {
                Visit(Dependency);
            }

            Stack.RemoveAt(Stack.Count - 1);
            InProgress.Remove(Action);
            Visited.Add(Action);
        }

        foreach (BuildAction Action in Actions)
        {
            Visit(Action);
        }
    }

    /// <summary>Selects the actions that must run.</summary>
    public List<BuildAction> DetermineOutdatedActions(ActionHistory History, DependencyCache Dependencies)
    {
        HashSet<BuildAction> Outdated = new();

        foreach (BuildAction Action in Actions)
        {
            if (IsOutdated(Action, History, Dependencies, out string Reason))
            {
                Log.Trace("{0} is outdated: {1}", Action.StatusText, Reason);
                MarkOutdated(Action, Outdated);
            }
        }

        // Preserve graph order so the executor sees a stable, dependency-first list.
        return Actions.Where(Outdated.Contains).ToList();
    }

    private void MarkOutdated(BuildAction Action, HashSet<BuildAction> Outdated)
    {
        if (!Outdated.Add(Action))
        {
            return;
        }

        foreach (BuildAction Dependent in GetDependents(Action))
        {
            MarkOutdated(Dependent, Outdated);
        }
    }

    /// <summary>Decides whether one action must run.</summary>
    public static bool IsOutdated(
        BuildAction Action,
        ActionHistory History,
        DependencyCache Dependencies,
        out string Reason)
    {
        if (Action.ProducedItems.Count == 0)
        {
            Reason = "produces no files";
            return true;
        }

        foreach (FileItem Produced in Action.ProducedItems)
        {
            if (!Produced.Exists)
            {
                Reason = $"'{Produced.Name}' does not exist";
                return true;
            }
        }

        // The tool always rewrites the primary output; an untouched import library would else read as stale.
        FileItem PrimaryOutput = Action.ProducedItems[0];

        if (History.HasCommandChanged(PrimaryOutput.Location, Action.GetCommandKey()))
        {
            Reason = "command line changed";
            return true;
        }

        // Without the header list an edit to anything the source includes would go unnoticed.
        if (Action.Type == ActionType.Compile && Dependencies.GetDependencies(PrimaryOutput.Location) is null)
        {
            Reason = "no recorded header dependencies";
            return true;
        }

        if (History.HasInputsChanged(PrimaryOutput.Location, ComputeInputFingerprint(Action, Dependencies)))
        {
            Reason = DescribeInputChange(Action, Dependencies);
            return true;
        }

        Reason = string.Empty;
        return false;
    }

    /// <summary>Identity of everything this action reads.</summary>
    public static string ComputeInputFingerprint(BuildAction Action, DependencyCache Dependencies)
    {
        return ContentHash.OfFiles(EnumerateInputs(Action, Dependencies).Select(Input => Input.Location));
    }

    private static IEnumerable<FileItem> EnumerateInputs(BuildAction Action, DependencyCache Dependencies)
    {
        foreach (FileItem Prerequisite in Action.PrerequisiteItems)
        {
            yield return Prerequisite;
        }

        if (Action.Type != ActionType.Compile || Action.ProducedItems.Count == 0)
        {
            yield break;
        }

        string[]? Headers = Dependencies.GetDependencies(Action.ProducedItems[0].Location);

        if (Headers is not null)
        {
            foreach (string Header in Headers)
            {
                yield return FileItem.Get(Header);
            }
        }
    }

    /// <summary>Names a likely culprit for the log.</summary>
    private static string DescribeInputChange(BuildAction Action, DependencyCache Dependencies)
    {
        DateTime OutputTime = Action.ProducedItems[0].Timestamp;

        foreach (FileItem Input in EnumerateInputs(Action, Dependencies))
        {
            if (!Input.Exists)
            {
                return $"'{Input.Name}' is missing";
            }

            if (Input.Timestamp > OutputTime)
            {
                return $"'{Input.Name}' changed";
            }
        }

        return "inputs differ from the last build";
    }
}
