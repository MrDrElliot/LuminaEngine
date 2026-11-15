using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class ManagedProjectStepTests : IDisposable
{
    private readonly string RootDirectory = Path.Combine(Path.GetTempPath(), $"ManagedProjectStepTests-{Guid.NewGuid():N}");

    public void Dispose()
    {
        Directory.Delete(RootDirectory, recursive: true);
    }

    [Fact]
    public void EnumerateProjectAndReferences_LogsWarningAndKeepsScanningWhenAReferenceIsUnreadable()
    {
        Directory.CreateDirectory(RootDirectory);

        string LockedProject = Path.Combine(RootDirectory, "Locked.csproj");
        File.WriteAllText(LockedProject, "<Project Sdk=\"Microsoft.NET.Sdk\"></Project>");

        string ReadableProject = Path.Combine(RootDirectory, "Readable.csproj");
        File.WriteAllText(ReadableProject, "<Project Sdk=\"Microsoft.NET.Sdk\"></Project>");

        string MainProject = Path.Combine(RootDirectory, "Main.csproj");
        File.WriteAllText(MainProject, $"""
            <Project Sdk="Microsoft.NET.Sdk">
              <ItemGroup>
                <ProjectReference Include="Locked.csproj" />
                <ProjectReference Include="Readable.csproj" />
              </ItemGroup>
            </Project>
            """);

        int WarningCountBefore = Log.WarningCount;

        using (FileStream Exclusive = new(LockedProject, FileMode.Open, FileAccess.Read, FileShare.None))
        {
            FileItem[] Found = ManagedProjectStep.EnumerateProjectAndReferences(MainProject).ToArray();

            Assert.Contains(Found, Item => Item.Location.Equals(ReadableProject, StringComparison.OrdinalIgnoreCase));
            Assert.Contains(Found, Item => Item.Location.Equals(MainProject, StringComparison.OrdinalIgnoreCase));
        }

        Assert.Equal(WarningCountBefore + 1, Log.WarningCount);
    }

    [Fact]
    public void EnumerateProjectAndReferences_IncludesDirectoryBuildPropsAndTargetsBesideTheProject()
    {
        Directory.CreateDirectory(RootDirectory);

        string ProjectFile = Path.Combine(RootDirectory, "Main.csproj");
        File.WriteAllText(ProjectFile, "<Project Sdk=\"Microsoft.NET.Sdk\"></Project>");

        string BuildProps = Path.Combine(RootDirectory, "Directory.Build.props");
        File.WriteAllText(BuildProps, "<Project></Project>");

        string BuildTargets = Path.Combine(RootDirectory, "Directory.Build.targets");
        File.WriteAllText(BuildTargets, "<Project></Project>");

        FileItem[] Found = ManagedProjectStep.EnumerateProjectAndReferences(ProjectFile).ToArray();

        Assert.Contains(Found, Item => Item.Location.Equals(BuildProps, StringComparison.OrdinalIgnoreCase));
        Assert.Contains(Found, Item => Item.Location.Equals(BuildTargets, StringComparison.OrdinalIgnoreCase));
    }
}
