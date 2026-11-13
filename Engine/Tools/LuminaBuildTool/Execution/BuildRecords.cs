using LuminaBuildTool.Configuration;

namespace LuminaBuildTool.Execution;

public sealed class BuildRecords
{
    private const string HistoryFileName = "ActionHistory.json";

    private const string DependenciesFileName = "Dependencies.json";

    private readonly BuildDirectories Directories;

    public BuildRecords(BuildDirectories Directories, BuildPlatform Platform, BuildConfiguration Configuration, TargetType Type)
    {
        this.Directories = Directories;
        LocalDirectory = Directories.BuildRecordDirectory(Platform, Configuration, Type);
        EngineDirectory = Directories.ToEngineOnly().BuildRecordDirectory(Platform, Configuration, Type);
    }

    public string LocalDirectory { get; }

    public string EngineDirectory { get; }

    public bool IsSameLocation(BuildRecords Other)
    {
        return string.Equals(LocalDirectory, Other.LocalDirectory, StringComparison.OrdinalIgnoreCase)
            && string.Equals(EngineDirectory, Other.EngineDirectory, StringComparison.OrdinalIgnoreCase);
    }

    // Engine-owned outputs keep their record in the engine's cache so projects share it.
    public string ResolveFile(string Output, string FileName)
    {
        bool bEngineOwned = Path.IsPathRooted(Output) && Directories.IsEngineOwned(Output);
        return Path.Combine(bEngineOwned ? EngineDirectory : LocalDirectory, FileName);
    }

    public ActionHistory LoadHistory()
    {
        return ActionHistory.Load(FilesNamed(HistoryFileName), Output => ResolveFile(Output, HistoryFileName));
    }

    public DependencyCache LoadDependencies()
    {
        return DependencyCache.Load(FilesNamed(DependenciesFileName), Output => ResolveFile(Output, DependenciesFileName));
    }

    private string[] FilesNamed(string FileName)
    {
        return new[] { Path.Combine(LocalDirectory, FileName), Path.Combine(EngineDirectory, FileName) };
    }
}
