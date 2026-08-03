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

/// <summary>
/// Compiles and links a target, after building whatever targets it declares as prerequisites.
/// </summary>
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
        IBuildPlatform PlatformSupport = BuildPlatformRegistry.Get(PlatformValue);
        BuildOptions Options = BuildOptions.Load(Directories, Arguments);

        IToolchain Toolchain = PlatformSupport.CreateToolchain(
            new TargetInfo(TargetName, TypeValue, PlatformValue, ConfigurationValue, Directories, Options));

        Log.Info("Using {0}", Toolchain.Description);
        Log.Verbose("Build features: {0}", Options);

        BuildSession Session = new(Arguments, Directories, Assembly, PlatformSupport, Toolchain, Options);
        int Result = await Session.BuildAsync(TargetName, TypeValue, PlatformValue, ConfigurationValue, Cancellation)
            .ConfigureAwait(false);

        if (Result == 0)
        {
            RefreshProjectFilesIfStale(Arguments, Directories, Assembly);

            Log.Info("Total build time: {0:F2}s.", Timer.Elapsed.TotalSeconds);
        }

        return Result;
    }

    /// <summary>
    /// Brings the IDE workspace back in line with the rules after a successful build, when a
    /// rules file has changed since it was written.
    /// </summary>
    /// <remarks>
    /// Editing a Build.cs used to mean the build saw the change and the IDE did not, until someone
    /// remembered to regenerate; the symptom was an include that compiles and still shows as
    /// unresolved. The gap closes itself now.
    ///
    /// Cheap because it is gated on the rules fingerprint, so an ordinary rebuild does no extra
    /// work at all, and because generation rewrites only the files whose content actually changes.
    /// The IDE therefore sees a modified project exactly when one is, and not on every build.
    ///
    /// Runs after the build rather than before it: MSBuild has finished evaluating the project by
    /// then, so rewriting it cannot affect the build that is invoking us.
    /// </remarks>
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
            // Never fail a build that already produced its binaries. The workspace being out of
            // date costs completion and error checking, not the build, and the manual command
            // still exists.
            Log.Warning(
                "Could not update project files: {0}. Run GenerateProjectFiles to refresh the IDE workspace.",
                Ex.Message);
        }
    }

    /// <summary>
    /// Shared state for one invocation, so a prerequisite target reuses the rules assembly and is
    /// never built twice.
    /// </summary>
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
            CancellationToken Cancellation)
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
                TargetInfo Info = new(TargetName, TypeValue, PlatformValue, ConfigurationValue, Directories, Options);
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

            // Held for the whole build. An IDE drives several projects at once, and two builds
            // sharing an output set would otherwise write the same object files concurrently.
            // Keyed on what is actually shared: platform, target type and configuration. Two
            // configurations build at once because they write different files; two targets in one
            // configuration cannot, because they share the engine's binaries and its objects.
            //
            // Rooted at the engine rather than at whatever was being built, because what they
            // share lives in the engine tree. Two different game projects building the same
            // configuration are as much a collision as two engine targets are, and keying this on
            // the invocation's own output root would have let them past each other.
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
                // Nothing to build still leaves something to write: loading the cache prunes
                // closures for outputs that no longer exist, and without this the pruning would be
                // redone on every up-to-date build and never kept.
                HeaderDependencies.Save();

                Log.Info("{0} is up to date ({1:F2}s).", Target.Name, Timer.Elapsed.TotalSeconds);
                return 0;
            }

            int MaxParallelism = Arguments.GetInt("MaxParallel", Math.Max(1, Environment.ProcessorCount - 1));
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
