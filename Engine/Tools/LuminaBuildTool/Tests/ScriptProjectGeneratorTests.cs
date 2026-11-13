using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.ProjectFiles;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class ScriptProjectGeneratorTests : IDisposable
{
    private readonly string EngineRoot = Directory.CreateTempSubdirectory("ScriptProjectGeneratorTests").FullName;

    private readonly string ProjectRoot;

    private readonly TestTarget Fixture;

    public ScriptProjectGeneratorTests()
    {
        Directory.CreateDirectory(Path.Combine(EngineRoot, "Engine", "Source", "Runtime"));
        ProjectRoot = Directory.CreateDirectory(Path.Combine(EngineRoot, "Project")).FullName;
        Fixture = new TestTarget("ScriptProjectGeneratorTest", EngineRoot, ProjectRoot);
    }

    public void Dispose()
    {
        Directory.Delete(EngineRoot, recursive: true);
    }

    private BuildDirectories Directories => Fixture.Target.Directories;

    private void AddPlugin(string Name, bool bHasScripts, params string[] Dependencies)
    {
        string Root = Path.Combine(ProjectRoot, "Plugins", Name);
        Directory.CreateDirectory(bHasScripts ? Path.Combine(Root, "Scripts") : Root);

        Fixture.Target.EnabledPlugins.Add(new PluginDescriptor
        {
            Name = Name,
            RootDirectory = Root,
            Dependencies = Dependencies.Select(Dependency => new PluginDependency { Name = Dependency }).ToList(),
        });
    }

    private IReadOnlyList<ScriptProject> Discover()
    {
        ProjectTargetInfo Target = new() { TargetName = "ScriptProjectGeneratorTest", Variants = new(), PrimaryVariant = Fixture.Target };
        return ScriptProjectGenerator.Discover(Directories, new[] { Target }, BuildPlatform.Windows64);
    }

    private static ScriptProject MakeProject(
        string Name, string[]? Dependencies = null, string OutputPath = "C:/Out/", string IntermediateOutputPath = "C:/Obj/")
    {
        return new ScriptProject
        {
            Name = Name,
            ScriptsDirectory = "C:/Scripts",
            ProjectPath = $"C:/Scripts/{Name}{ScriptProjectGenerator.ProjectSuffix}",
            SolutionFolder = "Scripts",
            OutputPath = OutputPath,
            IntermediateOutputPath = IntermediateOutputPath,
            Dependencies = Dependencies ?? Array.Empty<string>(),
        };
    }

    [Fact]
    public void DiscoverFindsNothingWhenNoScriptsDirectoryExists()
    {
        AddPlugin("Plain", bHasScripts: false);

        Assert.Empty(Discover());
    }

    [Fact]
    public void DiscoverFindsPluginGameAndEngineScripts()
    {
        AddPlugin("Scripted", bHasScripts: true, "Core");
        Directory.CreateDirectory(Path.Combine(ProjectRoot, "Game", "Scripts"));
        Directory.CreateDirectory(Path.Combine(EngineRoot, "Engine", "Resources", "Scripts"));

        IReadOnlyList<ScriptProject> Projects = Discover();

        Assert.Equal(new[] { "Scripted", "Game", "Engine" }, Projects.Select(Project => Project.Name));
        Assert.Equal(PathUtils.Normalize(Path.Combine(ProjectRoot, "Plugins", "Scripted", "Scripts")), Projects[0].ScriptsDirectory);
        Assert.Equal(new[] { "Core" }, Projects[0].Dependencies);
        Assert.Equal(PathUtils.Normalize(Path.Combine(ProjectRoot, "Game", "Scripts", "Game.Scripts.csproj")), Projects[1].ProjectPath);
        Assert.Equal(PathUtils.Normalize(Path.Combine(EngineRoot, "Engine", "Resources", "Scripts")), Projects[2].ScriptsDirectory);
        Assert.Empty(Projects[2].Dependencies);
    }

    [Fact]
    public void GameDependsOnlyOnPluginsWithScripts()
    {
        AddPlugin("Scripted", bHasScripts: true);
        AddPlugin("Plain", bHasScripts: false);
        Directory.CreateDirectory(Path.Combine(ProjectRoot, "Game", "Scripts"));

        ScriptProject Game = Assert.Single(Discover(), Project => Project.Name == "Game");

        Assert.Equal(new[] { "Scripted" }, Game.Dependencies);
    }

    [Fact]
    public void CsprojEscapesNameAndOutputPaths()
    {
        ScriptProject Escaped = MakeProject("A&B", OutputPath: "C:/Out<1>/", IntermediateOutputPath: "C:/Obj&\"2\"/");

        string Xml = ScriptProjectGenerator.BuildCsprojXml(Directories, Escaped, new[] { Escaped }, BuildPlatform.Windows64);

        Assert.Contains("<AssemblyName>A&amp;B</AssemblyName>", Xml);
        Assert.Contains("<OutputPath>C:/Out&lt;1&gt;/</OutputPath>", Xml);
        Assert.Contains("<IntermediateOutputPath>C:/Obj&amp;&quot;2&quot;/</IntermediateOutputPath>", Xml);
    }

    [Fact]
    public void CsprojNeverCopiesLuminaSharpIntoTheOutput()
    {
        ScriptProject Game = MakeProject("Game");

        string Xml = ScriptProjectGenerator.BuildCsprojXml(Directories, Game, new[] { Game }, BuildPlatform.Windows64);

        string Reference = Xml.Substring(Xml.IndexOf("""<Reference Include="LuminaSharp">""", StringComparison.Ordinal));
        Reference = Reference.Substring(0, Reference.IndexOf("</Reference>", StringComparison.Ordinal));
        Assert.Contains("<Private>false</Private>", Reference);
    }

    [Fact]
    public void CsprojReferencesOnlyResolvableDependencies()
    {
        ScriptProject Plugin = MakeProject("Plugin");
        ScriptProject Game = MakeProject("Game", new[] { "plugin", "Missing" });

        string Xml = ScriptProjectGenerator.BuildCsprojXml(Directories, Game, new[] { Plugin, Game }, BuildPlatform.Windows64);

        Assert.Single(Xml.Split("<ProjectReference ").Skip(1));
        Assert.Contains($"""<ProjectReference Include="{Plugin.ProjectPath}" />""", Xml);
        Assert.DoesNotContain("<ProjectReference", ScriptProjectGenerator.BuildCsprojXml(Directories, Plugin, new[] { Plugin, Game }, BuildPlatform.Windows64));
    }
}
