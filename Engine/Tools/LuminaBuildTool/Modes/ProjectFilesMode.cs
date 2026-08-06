using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.ProjectFiles;
using LuminaBuildTool.ProjectFiles.VisualStudio;
using LuminaBuildTool.Rules;
using LuminaBuildTool.Toolchain;

namespace LuminaBuildTool.Modes;

/// <summary>
/// Resolves every target for every configuration the IDE should expose, then hands the graphs
/// to a project file generator.
/// </summary>
public static class ProjectFilesMode
{
    public static int Run(CommandLine Arguments, BuildDirectories Directories)
    {
        RulesAssembly Assembly = RulesAssembly.Create(Directories, Arguments.HasFlag("RecompileRules"));

        Generate(Arguments, Directories, Assembly);

        return 0;
    }

    /// <summary>
    /// Writes the IDE workspace for every resolvable target. Returns the number of files that
    /// changed on disk, which is zero when the rules produce what is already there.
    /// </summary>
    /// <remarks>
    /// Takes the rules assembly rather than creating one so a build can generate with the assembly
    /// it already loaded: compiling the rules twice in one process would put two copies of every
    /// rules type in play.
    /// </remarks>
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

            // First failure only: when a target resolves in NO configuration the reason is usually one
            // rules file throwing the same diagnostic for all of them, so repeating it per configuration
            // buries it. The rest stay at Verbose for the genuinely per-configuration cases.
            string? FirstFailure = null;
            string? FirstFailureConfiguration = null;

            foreach (ProjectConfiguration Configuration in Configurations)
            {
                TargetInfo Info = new(TargetName, Configuration.Type, PlatformValue, Configuration.Configuration, Directories, Options);

                try
                {
                    // A target that declares its own type resolves the same graph for every
                    // requested type, which is what makes a Program appear under every
                    // configuration without needing its own solution configuration.
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
                // The reason used to be Verbose-only, so the default output named the skipped target and
                // said nothing about why -- and the thing that threw is usually a rules file reporting a
                // setup problem it already knows how to fix (an unbuilt vendored library, an uninitialised
                // submodule). That message was being written and then swallowed.
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
        IProjectFileGenerator Generator = new VisualStudioGenerator(Toolchain);

        int Changed = Generator.Generate(Directories, Targets, Configurations, RulesProjectPath)
            + (bRulesProjectChanged ? 1 : 0)
            + (CompileDatabaseStep.Write(Directories, Targets, Configurations, Toolchain) ? 1 : 0);

        // A workspace missing targets is a failure, not a partial success. The Targets.Count == 0 guard
        // above does not catch it: Reflector resolves out of the engine tree whatever state a project is
        // in, so a freshly cloned project whose every real target failed still reached here with a count
        // of one, generated a solution containing nothing buildable, and reported success. That is what
        // made a broken clone look like a working one.
        //
        // Thrown AFTER the files are written but BEFORE the stamp: the partial workspace is worth having,
        // and leaving the stamp on the previous rules is what makes the next build try again rather than
        // treat the incomplete generation as up to date.
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

        return Changed;
    }
}
