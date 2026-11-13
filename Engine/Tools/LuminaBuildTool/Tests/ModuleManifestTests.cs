using LuminaBuildTool.Configuration;
using LuminaBuildTool.Graph;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class ModuleManifestTests : IDisposable
{
    private readonly string EngineRoot = Path.Combine(Path.GetTempPath(), $"ModuleManifestTests-{Guid.NewGuid():N}");

    private readonly string HeaderPath;

    public ModuleManifestTests()
    {
        HeaderPath = Path.Combine(EngineRoot, "Engine", "Source", "Runtime", "Module.h");
        Directory.CreateDirectory(Path.GetDirectoryName(HeaderPath)!);
        File.WriteAllText(HeaderPath, "struct A { int X; };");
    }

    public void Dispose()
    {
        Directory.Delete(EngineRoot, recursive: true);
    }

    [Fact]
    public void UnchangedSignatureReusesRecordedBuildIdWithoutReadingHeaders()
    {
        BuildDirectories Directories = BuildDirectories.Discover(EngineRoot, null);
        string FirstId = ModuleManifest.LoadOrHashEngineHeaders(Directories);
        DateTime Stamp = File.GetLastWriteTimeUtc(HeaderPath);

        // Same length and timestamp, so only a content read could notice the edit.
        File.WriteAllText(HeaderPath, "struct A { int Y; };");
        File.SetLastWriteTimeUtc(HeaderPath, Stamp);
        Assert.Equal(FirstId, ModuleManifest.LoadOrHashEngineHeaders(Directories));

        File.SetLastWriteTimeUtc(HeaderPath, Stamp.AddSeconds(1));
        Assert.NotEqual(FirstId, ModuleManifest.LoadOrHashEngineHeaders(Directories));
    }
}
