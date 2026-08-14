using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.ProjectFiles;
using LuminaBuildTool.ProjectFiles.VisualStudio;
using LuminaBuildTool.Rules;
using LuminaBuildTool.Toolchain;

namespace LuminaBuildTool.Modes;

/// <summary>Resolves every target in every configuration, then hands the graphs to a generator.</summary>
public static class ProjectFilesMode
{
    public static int Run(CommandLine Arguments, BuildDirectories Directories)
    {
        RulesAssembly Assembly = RulesAssembly.Create(Directories, Arguments.HasFlag("RecompileRules"));

        Generate(Arguments, Directories, Assembly);

        return 0;
    }

    /// <summary>Writes the IDE workspace for every resolvable target.</summary>
    public static int Generate(CommandLine Arguments, BuildDirectories Directories, RulesAssembly Assembly)
    {
        BuildPlatform PlatformValue = Arguments.GetEnum("Platform", BuildPlatformRegistry.HostPlatform);
        IBuildPlatform PlatformSupport = BuildPlatformRegistry.Get(PlatformValue);

        BuildOptions Options = BuildOptions.Load(Directories, Arguments);

        List<ProjectConfiguration> Configurations = new();

        foreach (BuildConfiguration Configuration in Enum.GetValues<BuildConfiguration>())
        {
            foreach (TargetType Type in new[] { TargetType.Editor, TargetType.Game })
            {
                Configurations.Add(new ProjectConfiguration(Configuration, Type));
            }
        }

        List<ProjectTargetInfo> Targets = new();
        List<string> SkippedTargets = new();

        foreach (string TargetName in Assembly.TargetNames.OrderBy(N => N, StringComparer.OrdinalIgnoreCase))
        {
            Dictionary<ProjectConfiguration, BuildTarget> Variants = new();
            BuildTarget? Primary = null;

            // First failure only: total resolution failure usually throws the same diagnostic per configuration.
            string? FirstFailure = null;
            string? FirstFailureConfiguration = null;

            foreach (ProjectConfiguration Configuration in Configurations)
            {
                TargetInfo Info = new(TargetName, Configuration.Type, PlatformValue, Configuration.Configuration, Directories, Options);

                try
                {
                    // A target declaring its own type resolves one graph for every requested type.
                    BuildTarget Resolved = new TargetAssembler(Assembly, Directories, PlatformSupport).Assemble(TargetName, Info);

                    Variants[Configuration] = Resolved;
                    Primary ??= Resolved;
                }
                catch (BuildException Ex)
                {
                    Log.Verbose("Target '{0}' does not support {1}: {2}", TargetName, Configuration.DisplayName, Ex.Message);

                    FirstFailure ??= Ex.Message;
                    FirstFailureConfiguration ??= Configuration.DisplayName;
                }
            }

            if (Primary is null)
            {
                // Warning, not Verbose: the throwing rules file usually reports a fixable setup problem.
                Log.Warning("Target '{0}' could not be resolved in any configuration; skipping. {1} failed: {2}",
                    TargetName, FirstFailureConfiguration ?? "No configuration", FirstFailure ?? "no reason reported.");

                SkippedTargets.Add(TargetName);
                continue;
            }

            Targets.Add(new ProjectTargetInfo
            {
                TargetName = TargetName,
                Variants = Variants,
                PrimaryVariant = Primary,
            });
        }

        if (Targets.Count == 0)
        {
            throw new BuildException("No targets could be resolved, so there is nothing to generate.");
        }

        // Written first so the solution can reference it. It is what gives the IDE syntax
        // highlighting and completion on Build.cs and Target.cs files.
        bool bRulesProjectChanged = RulesProjectGenerator.Generate(Directories, Assembly);
        string RulesProjectPath = RulesProjectGenerator.GetProjectPath(Directories);

        IToolchain Toolchain = PlatformSupport.CreateToolchain(Targets[0].PrimaryVariant.Info);

        int Changed = (bRulesProjectChanged ? 1 : 0)
            + (CompileDatabaseStep.Write(Directories, Targets, Configurations, Toolchain) ? 1 : 0);

        // No .sln off Windows; nothing there builds a .vcxproj. Keyed on the host, not -Platform.
        if (BuildPlatformRegistry.HostPlatform == BuildPlatform.Windows64)
        {
            IProjectFileGenerator Generator = new VisualStudioGenerator(Toolchain);

            Changed += Generator.Generate(Directories, Targets, Configurations, RulesProjectPath);
        }

        // Missing targets is a failure, not partial success. Thrown after the write but before the stamp,
        // so the next build retries instead of trusting an incomplete generation.
        if (SkippedTargets.Count > 0)
        {
            throw new BuildException(
                $"{SkippedTargets.Count} of {SkippedTargets.Count + Targets.Count} targets could not be resolved and are "
                + $"missing from the generated workspace: {string.Join(", ", SkippedTargets)}. "
                + "See the warning logged for each one above.");
        }

        // Written after the files it describes, so an interrupted generation leaves the stamp
        // pointing at the previous rules and the next build tries again.
        ProjectFileStamp.Write(Directories, Assembly);

        Log.Info("{0} project files changed.", Changed);

        // Off Windows the only artefact is a file the user never named, so say where it is.
        if (BuildPlatformRegistry.HostPlatform != BuildPlatform.Windows64)
        {
            Log.Info(
                "Wrote {0}. Point clangd, VS Code (clangd extension) or Rider at this directory to pick it up.",
                Path.Combine(Directories.OutputRoot, "compile_commands.json"));
        }

        return Changed;
    }
}
