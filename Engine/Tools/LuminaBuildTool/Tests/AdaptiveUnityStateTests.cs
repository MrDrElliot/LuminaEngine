using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class AdaptiveUnityStateTests : IDisposable
{
    private static readonly DateTime LongAgo = new(2000, 1, 1, 0, 0, 0, DateTimeKind.Utc);

    private readonly string TempDirectory = Directory.CreateTempSubdirectory("AdaptiveUnityStateTests").FullName;

    private readonly BuildTarget Target;

    private readonly BuildModule Module;

    public AdaptiveUnityStateTests()
    {
        TestTarget Fixture = new("AdaptiveUnityTest") { ModuleDirectory = TempDirectory };
        Module = Fixture.AddModule("Test");
        Module.BinaryType = ModuleBinaryType.SharedLibrary;
        Module.IntermediateDirectory = Path.Combine(TempDirectory, "Intermediate");
        Directory.CreateDirectory(Module.IntermediateDirectory);
        Target = Fixture.Target;
    }

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    private FileItem WriteSource(string Name, DateTime TimestampUtc)
    {
        FileItem Item = FileItem.Get(Path.Combine(TempDirectory, Name));
        File.WriteAllText(Item.Location, $"int {Path.GetFileNameWithoutExtension(Name)}Symbol;");
        File.SetLastWriteTimeUtc(Item.Location, TimestampUtc);
        Item.Invalidate();
        return Item;
    }

    // Saved and reloaded between observations, since the timestamp fallback compares against the previous build's time.
    private AdaptiveUnityState ObserveTimestamps(int MaxFiles, params FileItem[] Sources)
    {
        AdaptiveUnityState State = AdaptiveUnityState.Load(Module);
        State.Observe(Sources, MaxFiles, Changed: null);
        State.Save();
        return State;
    }

    private static string[] Locations(params FileItem[] Items) => Items.Select(Item => Item.Location).Order().ToArray();

    [Fact]
    public void WorkingSetHoldsOutChangedSourcesCappedByNewest()
    {
        FileItem Oldest = WriteSource("Oldest.cpp", LongAgo);
        FileItem Middle = WriteSource("Middle.cpp", LongAgo.AddDays(1));
        FileItem Newest = WriteSource("Newest.cpp", LongAgo.AddDays(2));
        FileItem Unchanged = WriteSource("Unchanged.cpp", LongAgo.AddDays(3));
        FileItem[] Sources = [Oldest, Middle, Newest, Unchanged];
        HashSet<string> Changed = new(Locations(Oldest, Middle, Newest), StringComparer.OrdinalIgnoreCase)
        {
            PathUtils.Normalize(Path.Combine(TempDirectory, "NotASource.cpp")),
        };

        AdaptiveUnityState State = AdaptiveUnityState.Load(Module);

        State.Observe(Sources, MaxFiles: 10, Changed);
        Assert.Equal(Locations(Oldest, Middle, Newest), State.WorkingSet.Order());

        State.Observe(Sources, MaxFiles: 2, Changed);
        Assert.Equal(Locations(Middle, Newest), State.WorkingSet.Order());
    }

    [Fact]
    public void TimestampFallbackAdmitsNothingFirstThenFilesTouchedSince()
    {
        FileItem Untouched = WriteSource("Untouched.cpp", LongAgo);
        FileItem Touched = WriteSource("Touched.cpp", DateTime.UtcNow.AddDays(1));

        Assert.Empty(ObserveTimestamps(10, Untouched, Touched).WorkingSet);
        Assert.Equal(Locations(Touched), ObserveTimestamps(10, Untouched, Touched).WorkingSet);
    }

    [Fact]
    public void TimestampFallbackEvictsStalestBuildsPastCap()
    {
        DateTime Future = DateTime.UtcNow.AddDays(1);
        FileItem First = WriteSource("First.cpp", Future);
        FileItem Second = WriteSource("Second.cpp", Future);
        FileItem Third = WriteSource("Third.cpp", Future);

        ObserveTimestamps(2, First, Second, Third);
        ObserveTimestamps(2, First);
        ObserveTimestamps(2, Second);

        Assert.Equal(Locations(Second, Third), ObserveTimestamps(2, Third).WorkingSet.Order());
    }

    [Fact]
    public void UnityStepWithEverySourceHeldOutCompilesOnlyRealSources()
    {
        DateTime Future = DateTime.UtcNow.AddDays(1);
        FileItem[] Sources = [WriteSource("Alpha.cpp", Future), WriteSource("Beta.cpp", Future), WriteSource("Gamma.cpp", Future)];
        Module.Sources.CppFiles.AddRange(Sources);

        UnityBuildStep.Prepare(Target);
        Assert.Empty(AdaptiveUnityState.Load(Module).WorkingSet);

        UnityBuildStep.Prepare(Target);
        Assert.Equal(Locations(Sources), AdaptiveUnityState.Load(Module).WorkingSet.Order());

        // An empty blob would show up here as an extra translation unit that includes nothing.
        Assert.Equal(Locations(Sources), Module.CppCompileInputs.Select(Input => Input.Location).Order());
        Assert.Empty(Directory.EnumerateFiles(Path.Combine(Module.IntermediateDirectory, UnityBuildStep.BlobDirectoryName)));
    }
}
