using System.Diagnostics;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Execution;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.ProjectFiles;
using LuminaBuildTool.Rules;
using LuminaBuildTool.Toolchain;

namespace LuminaBuildTool.Modes;

/// <summary>Compiles and links a target, after building whatever targets it declares as prerequisites.</summary>
public static class BuildMode
{
    public static async Task<int> RunAsync(CommandLine Arguments, BuildDirectories Directories, CancellationToken Cancellation)
    {
        string? TargetName = Arguments.GetPositional(1);

        if (string.IsNullOrEmpty(TargetName))
        {
            throw new BuildException("Build requires a target name. Usage: LuminaBuildTool Build <Target> [options]");
        }

        BuildPlatform PlatformValue = Arguments.GetEnum("Platform", BuildPlatformRegistry.HostPlatform);
        BuildConfiguration ConfigurationValue = Arguments.GetEnum("Configuration", BuildConfiguration.Development);
        TargetType TypeValue = Arguments.GetEnum("TargetType", TargetType.Editor);

        Stopwatch Timer = Stopwatch.StartNew();

        RulesAssembly Assembly = RulesAssembly.Create(Directories, Arguments.HasFlag("RecompileRules"));

        int Result = await BuildTargetsAsync(
            new[] { TargetName },
            Arguments,
            Directories,
            Assembly,
            TypeValue,
            PlatformValue,
            ConfigurationValue,
            Cancellation).ConfigureAwait(false);

        if (Result == 0)
        {
            RefreshProjectFilesIfStale(Arguments, Directories, Assembly);

            Log.Info("Total build time: {0:F2}s.", Timer.Elapsed.TotalSeconds);
        }

        return Result;
    }

    // Builds several targets through one session, so shared prerequisites are built once.
    public static async Task<int> BuildTargetsAsync(
        IReadOnlyList<string> TargetNames,
        CommandLine Arguments,
        BuildDirectories Directories,
        RulesAssembly Assembly,
        TargetType TypeValue,
        BuildPlatform PlatformValue,
        BuildConfiguration ConfigurationValue,
        CancellationToken Cancellation)
    {
        IBuildPlatform PlatformSupport = BuildPlatformRegistry.Get(PlatformValue);
        BuildOptions Options = BuildOptions.Load(Directories, Arguments);

        IToolchain Toolchain = PlatformSupport.CreateToolchain(
            new TargetInfo(TargetNames[0], TypeValue, PlatformValue, ConfigurationValue, Directories, Options));

        Log.Info("Using {0}", Toolchain.Description);
        Log.Verbose("Build features: {0}", Options);

        BuildSession Session = new(Arguments, Directories, Assembly, PlatformSupport, Toolchain, Options);

        foreach (string TargetName in TargetNames)
        {
            int Result = await Session
                .BuildAsync(TargetName, TypeValue, PlatformValue, ConfigurationValue, Cancellation, bIsPrimary: true)
                .ConfigureAwait(false);

            if (Result != 0)
            {
                return Result;
            }
        }

        return 0;
    }

    /// <summary>Default parallelism: core count less one, capped to what the machine's memory holds.</summary>
    private static int DefaultParallelism()
    {
        int FromCores = Math.Max(1, Environment.ProcessorCount - 1);

        // TotalAvailableMemoryBytes is the cgroup limit where one applies and physical RAM
        // otherwise, so this reads a container's real allowance rather than the host's.
        long TotalBytes = GC.GetGCMemoryInfo().TotalAvailableMemoryBytes;

        if (TotalBytes <= 0)
        {
            return FromCores;
        }

        const long ReservedBytes = 3L * 1024 * 1024 * 1024;
        const long BytesPerAction = 1536L * 1024 * 1024;

        long BudgetBytes = TotalBytes - ReservedBytes;
        int FromMemory = (int)Math.Max(1, BudgetBytes / BytesPerAction);

        if (FromMemory >= FromCores)
        {
            return FromCores;
        }

        // Worth saying out loud. A build running at half the width of the machine looks like a bug
        // in the scheduler unless the reason is on screen, and the flag to override it is right here.
        Log.Info(
            "Limiting to {0} parallel actions for {1:F1} GiB of usable memory ({2} cores available). Override with -MaxParallel=<n>.",
            FromMemory,
            TotalBytes / (double)(1024 * 1024 * 1024),
            Environment.ProcessorCount);

        return FromMemory;
    }

    /// <summary>Refreshes the IDE workspace after a successful build when a rules file has changed.</summary>
    private static void RefreshProjectFilesIfStale(
        CommandLine Arguments,
        BuildDirectories Directories,
        RulesAssembly Assembly)
    {
        if (Arguments.HasFlag("NoProjectFileUpdate") || Arguments.HasFlag("DryRun"))
        {
            return;
        }

        if (!ProjectFileStamp.IsStale(Directories, Assembly))
        {
            return;
        }

        Log.Info("Build rules changed; updating project files...");

        try
        {
            ProjectFilesMode.Generate(Arguments, Directories, Assembly);
        }
        catch (Exception Ex)
        {
            // Never fail a build that already produced binaries; the manual command still exists.
            Log.Warning(
                "Could not update project files: {0}. Run GenerateProjectFiles to refresh the IDE workspace.",
                Ex.Message);
        }
    }

    /// <summary>Shared state for one invocation, so a prerequisite target is never built twice.</summary>
    private sealed class BuildSession
    {
        private readonly CommandLine Arguments;

        private readonly BuildDirectories Directories;

        private readonly RulesAssembly Assembly;

        private readonly IBuildPlatform PlatformSupport;

        private readonly IToolchain Toolchain;

        private readonly BuildOptions Options;

        private readonly HashSet<string> Completed = new(StringComparer.OrdinalIgnoreCase);

        private readonly List<string> InProgress = new();

        public BuildSession(
            CommandLine Arguments,
            BuildDirectories Directories,
            RulesAssembly Assembly,
            IBuildPlatform PlatformSupport,
            IToolchain Toolchain,
            BuildOptions Options)
        {
            this.Arguments = Arguments;
            this.Directories = Directories;
            this.Assembly = Assembly;
            this.PlatformSupport = PlatformSupport;
            this.Toolchain = Toolchain;
            this.Options = Options;
        }

        public async Task<int> BuildAsync(
            string TargetName,
            TargetType TypeValue,
            BuildPlatform PlatformValue,
            BuildConfiguration ConfigurationValue,
            CancellationToken Cancellation,
            bool bIsPrimary = false)
        {
            string Key = $"{TargetName}|{TypeValue}|{PlatformValue}|{ConfigurationValue}";

            if (!Completed.Add(Key))
            {
                return 0;
            }

            if (InProgress.Contains(Key, StringComparer.OrdinalIgnoreCase))
            {
                throw new BuildException(
                    "Circular prerequisite targets: " + string.Join(" -> ", InProgress.Append(Key)));
            }

            InProgress.Add(Key);

            try
            {
                // A reflection generator or other host tool is built to produce this target, not shipped
                // with it, so the profile the caller asked for is not its to collect.
                TargetInfo Info = new(
                    TargetName, TypeValue, PlatformValue, ConfigurationValue, Directories,
                    bIsPrimary ? Options : Options.WithoutPgo());
                BuildTarget Target = new TargetAssembler(Assembly, Directories, PlatformSupport).Assemble(TargetName, Info);

                // Prerequisite tools, such as the reflection generator, must exist before this
                // target's graph is built, because the graph references their output.
                foreach (string Prerequisite in Target.Rules.PreBuildTargetNames)
                {
                    Log.Verbose("Target '{0}' requires '{1}'", TargetName, Prerequisite);

                    int PrerequisiteResult = await BuildAsync(
                        Prerequisite,
                        TargetType.Program,
                        PlatformValue,
                        ConfigurationValue,
                        Cancellation).ConfigureAwait(false);

                    if (PrerequisiteResult != 0)
                    {
                        Log.Error("Prerequisite target '{0}' failed; not building '{1}'.", Prerequisite, TargetName);
                        return PrerequisiteResult;
                    }
                }

                // Built at this target's own type, and as the engine's own build: an engine target with a project
                // attached would put engine intermediates and generated bindings under that project.
                if (Target.Rules.RequiredTargetNames.Count > 0)
                {
                    BuildDirectories EngineOnly = Directories.ProjectRoot is null
                        ? Directories
                        : BuildDirectories.Discover(Directories.EngineRoot, null);

                    BuildSession EngineBuild = ReferenceEquals(EngineOnly, Directories)
                        ? this
                        : new BuildSession(Arguments, EngineOnly, Assembly, PlatformSupport, Toolchain, Options);

                    foreach (string Required in Target.Rules.RequiredTargetNames)
                    {
                        Log.Verbose("Target '{0}' also requires '{1}'", TargetName, Required);

                        int RequiredResult = await EngineBuild.BuildAsync(
                            Required,
                            TypeValue,
                            PlatformValue,
                            ConfigurationValue,
                            Cancellation).ConfigureAwait(false);

                        if (RequiredResult != 0)
                        {
                            Log.Error("Required target '{0}' failed; not building '{1}'.", Required, TargetName);
                            return RequiredResult;
                        }
                    }
                }

                return await BuildResolvedAsync(Target, Cancellation).ConfigureAwait(false);
            }
            finally
            {
                InProgress.Remove(Key);
            }
        }

        private async Task<int> BuildResolvedAsync(BuildTarget Target, CancellationToken Cancellation)
        {
            Stopwatch Timer = Stopwatch.StartNew();

            // Held for the whole build, keyed on platform/type/configuration and rooted at the engine, because
            // that is where the shared binaries and objects live.
            string OutputKey =
                $"output|{Target.Directories.EngineRoot}|{Target.Info.PlatformName}|{Target.Info.Type}|{Target.Info.Configuration}";

            using BuildLock OutputLock = BuildLock.Acquire(
                Target.Directories.EngineRoot,
                OutputKey,
                $"another build of {Target.Info.PlatformName} {Target.Info.Type} {Target.Info.Configuration} to finish",
                TimeSpan.FromHours(1),
                Cancellation);

            Log.Info("Building {0} - {1} - {2}", Target.Name, Target.Info.PlatformName, Target.Info.Configuration);

            if (Arguments.HasFlag("Clean"))
            {
                CleanMode.CleanTarget(Target);
            }

            ActionGraph Graph = ActionGraph.Build(Target, Toolchain);

            string CacheDirectory = Target.Directories.BuildRecordDirectory(
                Target.Info.Platform, Target.Info.Configuration, Target.Info.Type);
            ActionHistory History = ActionHistory.Load(Path.Combine(CacheDirectory, "ActionHistory.json"));
            DependencyCache HeaderDependencies = DependencyCache.Load(Path.Combine(CacheDirectory, "Dependencies.json"));

            List<BuildAction> Outdated = Graph.DetermineOutdatedActions(History, HeaderDependencies);

            Log.Info("{0} of {1} actions are out of date.", Outdated.Count, Graph.Actions.Count);

            if (Arguments.HasFlag("DryRun"))
            {
                foreach (BuildAction Action in Outdated)
                {
                    Log.Info("  {0} {1} ({2})", Action.Type, Action.StatusText, Action.ModuleName);
                }

                return 0;
            }

            if (Outdated.Count == 0)
            {
                // Loading the cache prunes dead closures; without this the pruning is redone and never kept.
                HeaderDependencies.Save();

                Log.Info("{0} is up to date ({1:F2}s).", Target.Name, Timer.Elapsed.TotalSeconds);
                return 0;
            }

            int MaxParallelism = Arguments.GetInt("MaxParallel", DefaultParallelism());
            bool bStopOnFirstError = !Arguments.GetBool("KeepGoing", false);

            BuildTimeline? Timeline = Arguments.HasFlag("Timeline") ? new BuildTimeline() : null;

            ActionExecutor Executor = new(Graph, History, HeaderDependencies, MaxParallelism, bStopOnFirstError, Timeline);
            ExecutionResult Result;

            try
            {
                Result = await Executor.ExecuteAsync(Outdated, Cancellation).ConfigureAwait(false);
            }
            finally
            {
                History.Save();
                HeaderDependencies.Save();

                // Written even for a failed build: a build that died partway is exactly when you
                // want to see what had run and what was still waiting.
                Timeline?.Write(Path.Combine(
                    Target.Directories.BuildToolIntermediatesDirectory,
                    $"Timeline-{Target.Name}-{Target.Info.Type}-{Target.Info.Configuration}.json"));

                Timeline?.LogSlowest(10);
            }

            if (!Result.bSucceeded)
            {
                Log.Error("{0} failed: {1} of {2} actions failed.", Target.Name, Result.ActionsFailed, Outdated.Count);
                return 1;
            }

            Log.Info(
                "{0} succeeded: {1} actions in {2:F2}s{3}.",
                Target.Name,
                Result.ActionsExecuted,
                Result.Elapsed.TotalSeconds,
                Result.ActionsSkipped > 0 ? $" ({Result.ActionsSkipped} became unnecessary)" : string.Empty);

            if (Target.LaunchModule is not null && Target.LaunchModule.OutputFile.Length > 0)
            {
                Log.Info("Output: {0}", Target.LaunchModule.OutputFile);
            }

            return 0;
        }
    }
}
