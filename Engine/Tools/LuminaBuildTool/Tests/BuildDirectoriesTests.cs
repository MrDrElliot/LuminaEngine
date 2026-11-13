using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Execution;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class BuildDirectoriesTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("BuildDirectoriesTests").FullName;

    private string EngineRoot => Path.Combine(TempDirectory, "Engine");

    private string ProjectRoot => Path.Combine(TempDirectory, "Project");

    public BuildDirectoriesTests()
    {
        Directory.CreateDirectory(Path.Combine(EngineRoot, "Engine", "Source", "Runtime"));
        Directory.CreateDirectory(ProjectRoot);
    }

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    private BuildDirectories EngineOnlyDirectories()
    {
        return BuildDirectories.Discover(EngineRoot, null);
    }

    private BuildDirectories ProjectDirectories()
    {
        return BuildDirectories.Discover(EngineRoot, ProjectRoot);
    }

    [Fact]
    public void EngineOnlyBuildPutsIntermediatesObjectAndRecordDirectoriesUnderEngineRoot()
    {
        BuildDirectories Directories = EngineOnlyDirectories();

        string ObjectDirectory = Directories.ObjectDirectory("Lumina", BuildPlatform.Windows64, BuildConfiguration.Development, TargetType.Editor);
        string RecordDirectory = Directories.BuildRecordDirectory(BuildPlatform.Windows64, BuildConfiguration.Development, TargetType.Editor);

        Assert.StartsWith(Directories.EngineRoot, Directories.IntermediatesDirectory);
        Assert.StartsWith(Directories.EngineRoot, ObjectDirectory);
        Assert.StartsWith(Directories.EngineRoot, RecordDirectory);
    }

    [Fact]
    public void ProjectBuildPutsIntermediatesObjectAndRecordDirectoriesUnderProjectRoot()
    {
        BuildDirectories Directories = ProjectDirectories();

        string ObjectDirectory = Directories.ObjectDirectory("MyGame", BuildPlatform.Windows64, BuildConfiguration.Development, TargetType.Game);
        string RecordDirectory = Directories.BuildRecordDirectory(BuildPlatform.Windows64, BuildConfiguration.Development, TargetType.Game);

        Assert.StartsWith(Directories.ProjectRoot!, Directories.IntermediatesDirectory);
        Assert.StartsWith(Directories.ProjectRoot!, ObjectDirectory);
        Assert.StartsWith(Directories.ProjectRoot!, RecordDirectory);
        Assert.DoesNotContain(Directories.EngineRoot, RecordDirectory);
    }

    [Fact]
    public void ProjectBuildSavesEachRecordToTheCacheOfTheRootThatOwnsItsOutput()
    {
        BuildDirectories Directories = ProjectDirectories();
        BuildRecords Records = new(Directories, BuildPlatform.Windows64, BuildConfiguration.Development, TargetType.Game);

        string EngineOwnedOutput = Path.Combine(Directories.EngineRoot, "Intermediates", "Obj", "Windows64", "Engine-abc123", "Game-Development", "Core", "Core.obj");
        string ProjectOwnedOutput = Path.Combine(Directories.ProjectRoot!, "Intermediates", "Obj", "Windows64", "MyGame", "Game-Development", "MyGame", "MyGame.obj");
        const string SyntheticKey = "Reflection/MyGame";

        ActionHistory History = Records.LoadHistory();
        History.RecordCommand(EngineOwnedOutput, "Engine");
        History.RecordCommand(ProjectOwnedOutput, "Project");
        History.RecordCommand(SyntheticKey, "Reflection");
        History.Save();

        ActionHistoryData? EngineSaved = JsonStore.Load<ActionHistoryData>(Path.Combine(Records.EngineDirectory, "ActionHistory.json"));
        ActionHistoryData? ProjectSaved = JsonStore.Load<ActionHistoryData>(Path.Combine(Records.LocalDirectory, "ActionHistory.json"));

        Assert.StartsWith(Directories.EngineRoot, Records.EngineDirectory);
        Assert.StartsWith(Directories.ProjectRoot!, Records.LocalDirectory);
        Assert.NotNull(EngineSaved);
        Assert.NotNull(ProjectSaved);
        Assert.Equal([EngineOwnedOutput], EngineSaved.CommandKeys.Keys);
        Assert.Equal(2, ProjectSaved.CommandKeys.Count);
        Assert.Contains(ProjectOwnedOutput, ProjectSaved.CommandKeys.Keys);
        Assert.Contains(SyntheticKey, ProjectSaved.CommandKeys.Keys);
    }
}
