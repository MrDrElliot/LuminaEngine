using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.ProjectFiles;
using LuminaBuildTool.ProjectFiles.VisualStudio;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.Modes;

/// <summary>
/// Resolves every target for every configuration the IDE should expose, then hands the graphs
/// to a project file generator.
/// </summary>
public static class ProjectFilesMode
{
    public static int Run(CommandLine Arguments, BuildDirectories Directories)
    {
        BuildPlatform PlatformValue = Arguments.GetEnum("Platform", BuildPlatformRegistry.HostPlatform);
        IBuildPlatform PlatformSupport = BuildPlatformRegistry.Get(PlatformValue);

        RulesAssembly Assembly = RulesAssembly.Create(Directories, Arguments.HasFlag("RecompileRules"));
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

        foreach (string TargetName in Assembly.TargetNames.OrderBy(N => N, StringComparer.OrdinalIgnoreCase))
        {
            Dictionary<ProjectConfiguration, BuildTarget> Variants = new();
            BuildTarget? Primary = null;

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
                }
            }

            if (Primary is null)
            {
                Log.Warning("Target '{0}' could not be resolved in any configuration; skipping.", TargetName);
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

        IProjectFileGenerator Generator = new VisualStudioGenerator(
            PlatformSupport.CreateToolchain(Targets[0].PrimaryVariant.Info));

        int Changed = Generator.Generate(Directories, Targets, Configurations, RulesProjectPath)
            + (bRulesProjectChanged ? 1 : 0);

        Log.Info("{0} project files changed.", Changed);

        return 0;
    }
}
