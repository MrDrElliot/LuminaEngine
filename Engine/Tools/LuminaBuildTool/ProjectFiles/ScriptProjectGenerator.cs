using System.Text;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.ProjectFiles;

public sealed class ScriptProject
{
    public required string Name { get; init; }

    public required string ScriptsDirectory { get; init; }

    public required string ProjectPath { get; init; }

    public required string SolutionFolder { get; init; }

    public required string OutputPath { get; init; }

    public required string IntermediateOutputPath { get; init; }

    public required IReadOnlyList<string> Dependencies { get; init; }
}

public static class ScriptProjectGenerator
{
    public const string ProjectSuffix = ".Scripts.csproj";

    // Mirrors BuildScriptUnits in DotNetHost.cpp: every enabled plugin, the game, the engine library.
    public static IReadOnlyList<ScriptProject> Discover(
        BuildDirectories Directories,
        IReadOnlyList<ProjectTargetInfo> Targets,
        BuildPlatform Platform)
    {
        List<ScriptProject> Discovered = new();
        List<string> PluginsWithScripts = new();

        IEnumerable<PluginDescriptor> Plugins = Targets
            .SelectMany(Target => Target.PrimaryVariant.EnabledPlugins)
            .DistinctBy(Plugin => Plugin.Name, StringComparer.OrdinalIgnoreCase)
            .OrderBy(Plugin => Plugin.Name, StringComparer.OrdinalIgnoreCase);

        foreach (PluginDescriptor Plugin in Plugins)
        {
            string ScriptsDirectory = Path.Combine(Plugin.RootDirectory, "Scripts");

            if (!Directory.Exists(ScriptsDirectory))
            {
                continue;
            }

            bool bUnderProject = Directories.ProjectRoot is not null
                && PathUtils.IsUnder(Plugin.RootDirectory, Directories.ProjectRoot);

            Discovered.Add(new ScriptProject
            {
                Name = Plugin.Name,
                ScriptsDirectory = PathUtils.Normalize(ScriptsDirectory),
                ProjectPath = MakeProjectPath(ScriptsDirectory, Plugin.Name),
                SolutionFolder = bUnderProject
                    ? $"{ResolveGameFolder(Directories)}/Plugins/{Plugin.Name}"
                    : "Plugins/" + Plugin.Name,
                OutputPath = PathUtils.Combine(Plugin.RootDirectory, "Binaries/DotNet") + "/",
                IntermediateOutputPath = PathUtils.Combine(Plugin.RootDirectory, "Intermediates/DotNet", Plugin.Name) + "/",
                Dependencies = Plugin.Dependencies,
            });

            PluginsWithScripts.Add(Plugin.Name);
        }

        if (Directories.ProjectRoot is not null)
        {
            string ScriptsDirectory = Path.Combine(Directories.ProjectRoot, "Game", "Scripts");

            if (Directory.Exists(ScriptsDirectory))
            {
                Discovered.Add(new ScriptProject
                {
                    Name = "Game",
                    ScriptsDirectory = PathUtils.Normalize(ScriptsDirectory),
                    ProjectPath = MakeProjectPath(ScriptsDirectory, "Game"),
                    SolutionFolder = ResolveGameFolder(Directories) + "/Scripts",
                    OutputPath = PathUtils.Combine(Directories.ProjectRoot, "Binaries/DotNet") + "/",
                    IntermediateOutputPath = PathUtils.Combine(Directories.ProjectRoot, "Intermediates/DotNet/Game") + "/",
                    Dependencies = PluginsWithScripts,
                });
            }
        }

        string EngineScriptsDirectory = Path.Combine(Directories.EngineRoot, "Engine", "Resources", "Scripts");

        if (Directory.Exists(EngineScriptsDirectory))
        {
            // Engine scripts are emitted beside the editor executable, not under an Engine/Binaries/DotNet root.
            string EngineDotNet = PathUtils.Combine(EngineBinariesDirectory(Directories, Platform), "DotNet");

            Discovered.Add(new ScriptProject
            {
                Name = "Engine",
                ScriptsDirectory = PathUtils.Normalize(EngineScriptsDirectory),
                ProjectPath = MakeProjectPath(EngineScriptsDirectory, "Engine"),
                SolutionFolder = "Engine/Scripts",
                OutputPath = EngineDotNet + "/",
                IntermediateOutputPath = PathUtils.Combine(EngineDotNet, "obj/Engine") + "/",
                Dependencies = Array.Empty<string>(),
            });
        }

        return Discovered;
    }

    // Bootstrap only: the editor owns these files and rewrites them on load, so an existing one is left alone.
    public static int EnsureProjectFiles(
        BuildDirectories Directories,
        IReadOnlyList<ScriptProject> ScriptProjects,
        BuildPlatform Platform)
    {
        int Written = 0;

        foreach (ScriptProject Project in ScriptProjects)
        {
            if (File.Exists(Project.ProjectPath))
            {
                continue;
            }

            if (PathUtils.WriteFileIfChanged(Project.ProjectPath, BuildCsprojXml(Directories, Project, ScriptProjects, Platform), bByteOrderMark: false))
            {
                Written++;
            }
        }

        return Written;
    }

    private static string BuildCsprojXml(
        BuildDirectories Directories,
        ScriptProject Project,
        IReadOnlyList<ScriptProject> AllProjects,
        BuildPlatform Platform)
    {
        string LuminaSharp = PathUtils.Combine(
            EngineBinariesDirectory(Directories, Platform), "DotNet/Managed/LuminaSharp.dll");

        StringBuilder Xml = new();
        Xml.AppendLine("""<Project Sdk="Microsoft.NET.Sdk">""");
        Xml.AppendLine();
        Xml.AppendLine("  <!-- Generated for IDE IntelliSense only; the engine compiles these scripts at run time. -->");
        Xml.AppendLine();
        Xml.AppendLine("  <PropertyGroup>");
        Xml.AppendLine("    <TargetFramework>net10.0</TargetFramework>");
        Xml.AppendLine("    <Nullable>enable</Nullable>");
        Xml.AppendLine("    <ImplicitUsings>disable</ImplicitUsings>");
        Xml.AppendLine("    <EnableDefaultItems>true</EnableDefaultItems>");
        Xml.AppendLine("    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>");
        Xml.AppendLine($"    <AssemblyName>{Escape(Project.Name)}</AssemblyName>");
        Xml.AppendLine("    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>");
        Xml.AppendLine($"    <OutputPath>{Escape(Project.OutputPath)}</OutputPath>");
        Xml.AppendLine($"    <IntermediateOutputPath>{Escape(Project.IntermediateOutputPath)}</IntermediateOutputPath>");
        Xml.AppendLine("  </PropertyGroup>");
        Xml.AppendLine();
        Xml.AppendLine("  <ItemGroup>");
        Xml.AppendLine("""    <Reference Include="LuminaSharp">""");
        Xml.AppendLine($"      <HintPath>{Escape(LuminaSharp)}</HintPath>");
        Xml.AppendLine("    </Reference>");

        // The generator the runtime compile loads; without it partial NativeCall bodies are missing.
        string Generators = PathUtils.Combine(Path.GetDirectoryName(LuminaSharp)!, "LuminaSharp.Generators.dll");
        Xml.AppendLine($"""    <Analyzer Include="{Escape(Generators)}" />""");
        Xml.AppendLine("  </ItemGroup>");

        List<ScriptProject> References = Project.Dependencies
            .Select(Name => AllProjects.FirstOrDefault(P => string.Equals(P.Name, Name, StringComparison.OrdinalIgnoreCase)))
            .Where(Dependency => Dependency is not null)
            .Select(Dependency => Dependency!)
            .ToList();

        if (References.Count > 0)
        {
            Xml.AppendLine();
            Xml.AppendLine("  <ItemGroup>");

            foreach (ScriptProject Reference in References)
            {
                Xml.AppendLine($"""    <ProjectReference Include="{Escape(Reference.ProjectPath)}" />""");
            }

            Xml.AppendLine("  </ItemGroup>");
        }

        Xml.AppendLine();
        Xml.AppendLine("</Project>");

        return Xml.ToString();
    }

    private static string MakeProjectPath(string ScriptsDirectory, string Name)
    {
        return PathUtils.Normalize(Path.Combine(ScriptsDirectory, Name + ProjectSuffix));
    }

    private static string EngineBinariesDirectory(BuildDirectories Directories, BuildPlatform Platform)
    {
        return PathUtils.Combine(Directories.EngineRoot, "Binaries", Platform.GetOutputDirectoryName());
    }

    private static string ResolveGameFolder(BuildDirectories Directories)
    {
        return "Games/" + Path.GetFileName(PathUtils.Normalize(Directories.ProjectRoot!));
    }

    private static string Escape(string Value)
    {
        return XmlUtils.Escape(Value);
    }
}
