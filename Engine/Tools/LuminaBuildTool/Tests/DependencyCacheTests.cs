using LuminaBuildTool.Core;
using LuminaBuildTool.Execution;
using LuminaBuildTool.Graph;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class DependencyCacheTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("DependencyCacheTests").FullName;

    private readonly string CacheFile;

    public DependencyCacheTests()
    {
        CacheFile = Path.Combine(TempDirectory, "DependencyCache.json");
    }

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    private string WriteOutput(string Name)
    {
        string OutputPath = Path.Combine(TempDirectory, Name);
        File.WriteAllText(OutputPath, string.Empty);
        return OutputPath;
    }

    [Fact]
    public void SaveLoad_RoundTripsSharedHeaderAcrossOutputs()
    {
        string FirstOutput = WriteOutput("First.obj");
        string SecondOutput = WriteOutput("Second.obj");
        string SharedHeader = Path.Combine(TempDirectory, "Shared.h");
        string OnlyHeader = Path.Combine(TempDirectory, "Only.h");

        DependencyCache Cache = DependencyCache.Load(CacheFile);
        Cache.RecordFromCompilerOutput(FirstOutput, WriteSourceDependencies(FirstOutput, [SharedHeader, OnlyHeader]), DependencyListFormat.MsvcSourceDependencies);
        Cache.RecordFromCompilerOutput(SecondOutput, WriteSourceDependencies(SecondOutput, [SharedHeader]), DependencyListFormat.MsvcSourceDependencies);
        Cache.Save();

        DependencyCache Reloaded = DependencyCache.Load(CacheFile);

        Assert.Equal([SharedHeader, OnlyHeader], Reloaded.GetDependencies(FirstOutput) ?? Array.Empty<string>());
        Assert.Equal([SharedHeader], Reloaded.GetDependencies(SecondOutput) ?? Array.Empty<string>());
    }

    [Fact]
    public void Load_DropsEntryForMissingOutputAndMarksDirtyForNextSave()
    {
        string KeptOutput = WriteOutput("Kept.obj");
        string MissingOutput = Path.Combine(TempDirectory, "Missing.obj");
        string Header = Path.Combine(TempDirectory, "Shared.h");

        DependencyCache Original = DependencyCache.Load(CacheFile);
        Original.RecordFromCompilerOutput(KeptOutput, WriteSourceDependencies(KeptOutput, [Header]), DependencyListFormat.MsvcSourceDependencies);
        Original.RecordFromCompilerOutput(MissingOutput, WriteSourceDependencies(MissingOutput, [Header]), DependencyListFormat.MsvcSourceDependencies);
        Original.Save();

        File.Delete(MissingOutput);

        DependencyCache Reloaded = DependencyCache.Load(CacheFile);
        Assert.Null(Reloaded.GetDependencies(MissingOutput));
        Assert.NotNull(Reloaded.GetDependencies(KeptOutput));

        Reloaded.Save();

        DependencyCacheData? Saved = JsonStore.Load<DependencyCacheData>(CacheFile);
        Assert.NotNull(Saved);
        Assert.DoesNotContain(MissingOutput, Saved.Dependencies.Keys);
        Assert.Contains(KeptOutput, Saved.Dependencies.Keys);
    }

    private string WriteSourceDependencies(string OutputFile, string[] Headers)
    {
        string ListFile = OutputFile + ".deps.json";
        string Includes = string.Join(", ", Headers.Select(Header => $"\"{Header.Replace("\\", "\\\\")}\""));
        File.WriteAllText(ListFile, $"{{\"Data\":{{\"Includes\":[{Includes}]}}}}");
        return ListFile;
    }
}
