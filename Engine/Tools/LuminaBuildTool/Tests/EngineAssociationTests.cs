using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class EngineAssociationTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("EngineAssociationTests").FullName;

    private string MakeProjectFile(string Contents = "{}")
    {
        string FilePath = Path.Combine(TempDirectory, "Project.lproject");
        File.WriteAllText(FilePath, Contents);
        return FilePath;
    }

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    [Fact]
    public void WriteThenReadAssociation_RoundTripsValue()
    {
        string ProjectFile = MakeProjectFile();

        EngineAssociation.WriteAssociation(ProjectFile, "{3E1C2B4A-0000-0000-0000-000000000000}");

        Assert.Equal("{3E1C2B4A-0000-0000-0000-000000000000}", EngineAssociation.ReadAssociation(ProjectFile));
    }

    [Fact]
    public void WriteAssociation_EmptyValueRemovesField()
    {
        string ProjectFile = MakeProjectFile();
        EngineAssociation.WriteAssociation(ProjectFile, "{3E1C2B4A-0000-0000-0000-000000000000}");

        EngineAssociation.WriteAssociation(ProjectFile, "");

        Assert.Null(EngineAssociation.ReadAssociation(ProjectFile));
        Assert.DoesNotContain(EngineAssociation.ProjectKey, File.ReadAllText(ProjectFile));
    }

    [Fact]
    public void WriteAssociation_NonObjectFileThrowsWithFileName()
    {
        string ProjectFile = MakeProjectFile("[1, 2, 3]");

        BuildException Ex = Assert.Throws<BuildException>(() => EngineAssociation.WriteAssociation(ProjectFile, "SomeId"));
        Assert.Contains(ProjectFile, Ex.Message);
    }

    [Fact]
    public void Resolve_FallsBackToProjectRelativePathWhenEngineRootExists()
    {
        string EngineDirectoryName = "SiblingEngine";
        Directory.CreateDirectory(Path.Combine(TempDirectory, EngineDirectoryName, "Engine", "Source", "Runtime"));

        string? Resolved = EngineAssociation.Resolve(EngineDirectoryName, TempDirectory);

        Assert.Equal(
            PathUtils.Normalize(Path.Combine(TempDirectory, EngineDirectoryName)),
            Resolved);
    }

    [Fact]
    public void Resolve_ReturnsNullWhenRelativePathHasNoEngineSourceRuntime()
    {
        string? Resolved = EngineAssociation.Resolve("NotAnEngine", TempDirectory);

        Assert.Null(Resolved);
    }
}
