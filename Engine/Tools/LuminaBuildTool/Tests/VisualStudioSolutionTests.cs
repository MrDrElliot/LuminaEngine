using System.Xml.Linq;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.ProjectFiles;
using LuminaBuildTool.ProjectFiles.VisualStudio;
using Xunit;
using SolutionProject = LuminaBuildTool.ProjectFiles.VisualStudio.VisualStudioGenerator.SolutionProject;

namespace LuminaBuildTool.Tests;

public sealed class VisualStudioSolutionTests
{
    private const string SolutionDirectory = "C:/Root";

    private static readonly ProjectConfiguration[] Configurations =
    {
        new(BuildConfiguration.Debug, TargetType.Editor),
        new(BuildConfiguration.Development, TargetType.Game),
    };

    private readonly ProjectTargetInfo Target = new()
    {
        TargetName = "SolutionTest",
        Variants = new(),
        PrimaryVariant = new TestTarget("SolutionTest").Target,
    };

    private VisualStudioGenerator.GeneratedProject MakeProject(
        string Name, string Folder, bool bBuildable = false, bool bBuildByDefault = false, bool bIsStartup = false)
    {
        return new VisualStudioGenerator.GeneratedProject
        {
            ProjectName = Name,
            OwningTargetName = Target.TargetName,
            Module = null,
            Target = Target,
            FilePath = $"{SolutionDirectory}/Intermediates/ProjectFiles/{Name}.vcxproj",
            Guid = Guid.NewGuid(),
            SolutionFolder = Folder,
            bBuildable = bBuildable,
            bBuildByDefault = bBuildByDefault,
            bIsStartup = bIsStartup,
        };
    }

    private List<SolutionProject> Collect()
    {
        VisualStudioGenerator.GeneratedProject[] Projects =
        {
            MakeProject("AModule", "Engine"),
            MakeProject("Tests", "Engine/Tests", bBuildable: true),
            MakeProject("Lumina", "Targets", bBuildable: true, bBuildByDefault: true),
            MakeProject("Zed", "Games/Foo/Source", bBuildable: true, bBuildByDefault: true, bIsStartup: true),
        };

        ScriptProject Scripts = new()
        {
            Name = "Game",
            ScriptsDirectory = $"{SolutionDirectory}/Game/Scripts",
            ProjectPath = $"{SolutionDirectory}/Game/Scripts/Game{ScriptProjectGenerator.ProjectSuffix}",
            SolutionFolder = "Games/Foo/Scripts",
            OutputPath = "C:/Out/",
            IntermediateOutputPath = "C:/Obj/",
            Dependencies = Array.Empty<string>(),
        };

        return VisualStudioGenerator.CollectSolutionProjects(
            Projects, new[] { Scripts }, $"{SolutionDirectory}/Intermediates/ProjectFiles/LuminaRules.csproj", SolutionDirectory);
    }

    private static XElement ProjectEntry(XDocument Slnx, string FileName)
    {
        return Assert.Single(Slnx.Descendants("Project"), P => Path.GetFileName((string)P.Attribute("Path")!) == FileName);
    }

    [Fact]
    public void ProjectsListStartupFirstThenBuildableThenScriptsAndRules()
    {
        List<SolutionProject> Projects = Collect();

        Assert.Equal(new[] { "Zed", "Lumina", "Tests", "AModule", "Game.Scripts", "LuminaRules" }, Projects.Select(P => P.Name));
        Assert.Equal(new[] { true, true, false, false, false, false }, Projects.Select(P => P.bBuildByDefault));
        Assert.Equal(new[] { false, false, false, false, true, true }, Projects.Select(P => P.bManaged));
    }

    [Fact]
    public void SlnxDeclaresEveryConfigurationAndThePlatform()
    {
        XDocument Slnx = XDocument.Parse(VisualStudioGenerator.BuildSolution(SolutionFormat.Slnx, Collect(), Configurations));
        XElement Section = Slnx.Root!.Element("Configurations")!;

        Assert.Equal(new[] { "Debug Editor", "Development Game" }, Section.Elements("BuildType").Select(E => (string)E.Attribute("Name")!));
        Assert.Equal("x64", (string)Assert.Single(Section.Elements("Platform")).Attribute("Name")!);
    }

    [Fact]
    public void SlnxNestsProjectsInEveryFolderLevel()
    {
        XDocument Slnx = XDocument.Parse(VisualStudioGenerator.BuildSolution(SolutionFormat.Slnx, Collect(), Configurations));

        Assert.Equal(
            new[] { "/Games/", "/Games/Foo/", "/Games/Foo/Source/", "/Targets/", "/Engine/", "/Engine/Tests/", "/Games/Foo/Scripts/" },
            Slnx.Root!.Elements("Folder").Select(F => (string)F.Attribute("Name")!));
        Assert.Equal("/Games/Foo/Source/", (string)ProjectEntry(Slnx, "Zed.vcxproj").Parent!.Attribute("Name")!);
        Assert.Equal("/Games/Foo/Scripts/", (string)ProjectEntry(Slnx, "Game.Scripts.csproj").Parent!.Attribute("Name")!);
        Assert.Equal(Slnx.Root, ProjectEntry(Slnx, "LuminaRules.csproj").Parent);
        Assert.Equal("Intermediates/ProjectFiles/Zed.vcxproj", (string)ProjectEntry(Slnx, "Zed.vcxproj").Attribute("Path")!);
    }

    [Fact]
    public void SlnxListsStartupTargetFirst()
    {
        XDocument Slnx = XDocument.Parse(VisualStudioGenerator.BuildSolution(SolutionFormat.Slnx, Collect(), Configurations));

        Assert.Equal("Zed.vcxproj", Path.GetFileName((string)Slnx.Descendants("Project").First().Attribute("Path")!));
    }

    [Fact]
    public void SlnxBuildsOnlyBuildByDefaultNativeProjects()
    {
        XDocument Slnx = XDocument.Parse(VisualStudioGenerator.BuildSolution(SolutionFormat.Slnx, Collect(), Configurations));

        Assert.Empty(ProjectEntry(Slnx, "Zed.vcxproj").Elements());
        Assert.Empty(ProjectEntry(Slnx, "Lumina.vcxproj").Elements());

        foreach (string Unbuilt in new[] { "Tests.vcxproj", "AModule.vcxproj" })
        {
            XElement Build = Assert.Single(ProjectEntry(Slnx, Unbuilt).Elements());
            Assert.Equal("Build", Build.Name.LocalName);
            Assert.Equal("false", (string)Build.Attribute("Project")!);
        }
    }

    [Fact]
    public void SlnxMapsManagedProjectsToDebugAnyCpuWithoutBuilding()
    {
        XDocument Slnx = XDocument.Parse(VisualStudioGenerator.BuildSolution(SolutionFormat.Slnx, Collect(), Configurations));

        foreach (string Managed in new[] { "Game.Scripts.csproj", "LuminaRules.csproj" })
        {
            XElement Entry = ProjectEntry(Slnx, Managed);
            Assert.Equal("Debug", (string)Entry.Element("BuildType")!.Attribute("Project")!);
            Assert.Equal("Any CPU", (string)Entry.Element("Platform")!.Attribute("Project")!);
            Assert.Equal("false", (string)Entry.Element("Build")!.Attribute("Project")!);
        }
    }

    [Fact]
    public void SlnListsTheSameProjectsAsSlnx()
    {
        List<SolutionProject> Projects = Collect();
        string Sln = VisualStudioGenerator.BuildSolution(SolutionFormat.Sln, Projects, Configurations);

        foreach (SolutionProject Project in Projects)
        {
            Assert.Contains($"\"{Project.Name}\", \"{Project.RelativePath}\", \"{{{Project.Guid.ToString().ToUpperInvariant()}}}\"", Sln);
        }

        Assert.Contains($"{{{Projects[0].Guid.ToString().ToUpperInvariant()}}}.Debug Editor|x64.Build.0 = Debug Editor|x64", Sln);
        Assert.DoesNotContain($"{{{Projects[2].Guid.ToString().ToUpperInvariant()}}}.Debug Editor|x64.Build.0", Sln);
    }

    [Theory]
    [InlineData("Slnx", SolutionFormat.Slnx)]
    [InlineData("sln", SolutionFormat.Sln)]
    public void SolutionFormatParsesNamesCaseInsensitively(string Value, SolutionFormat Expected)
    {
        CommandLine Args = new(new[] { $"-SolutionFormat={Value}" });

        Assert.Equal(Expected, Args.GetEnum("SolutionFormat", SolutionFormat.Slnx));
    }

    [Theory]
    [InlineData("vcxproj")]
    [InlineData("99")]
    public void SolutionFormatRejectsUnknownValues(string Value)
    {
        CommandLine Args = new(new[] { $"-SolutionFormat={Value}" });

        BuildException Error = Assert.Throws<BuildException>(() => Args.GetEnum("SolutionFormat", SolutionFormat.Slnx));
        Assert.Contains("Valid values: Sln, Slnx", Error.Message);
    }
}
