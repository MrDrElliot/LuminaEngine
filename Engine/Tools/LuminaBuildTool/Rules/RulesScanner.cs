using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Rules;

/// <summary>
/// One discovered rules file on disk.
/// </summary>
public sealed class RulesFile
{
    public RulesFile(string Location, RulesFileKind Kind, string? PluginName)
    {
        this.Location = Location;
        this.Kind = Kind;
        this.PluginName = PluginName;
    }

    public string Location { get; }

    public RulesFileKind Kind { get; }

    /// <summary>Owning plugin, or null for engine and project rules.</summary>
    public string? PluginName { get; }

    /// <summary>Declared name, taken from the file name with its suffix removed.</summary>
    public string DeclaredName
    {
        get
        {
            string FileName = Path.GetFileName(Location);
            int Suffix = FileName.IndexOf('.');
            return Suffix > 0 ? FileName.Substring(0, Suffix) : FileName;
        }
    }

    public string Directory => Path.GetDirectoryName(Location)!;

    public override string ToString() => Location;
}

public enum RulesFileKind
{
    Target,
    Module,

    /// <summary>Shared helper code compiled into the rules assembly but never instantiated.</summary>
    Shared,
}

/// <summary>
/// Walks the engine, project and plugin trees collecting rules files.
/// </summary>
public sealed class RulesScanner
{
    private static readonly string[] IgnoredDirectoryNames =
    {
        "Intermediates",
        "Binaries",
        "Saved",
        "obj",
        "bin",
        ".git",
        ".vs",
        ".claude",
        "node_modules",
        "External",
        "CrashDumps",
        "Logs",
    };

    private readonly BuildDirectories Directories;

    public RulesScanner(BuildDirectories Directories)
    {
        this.Directories = Directories;
    }

    public List<PluginDescriptor> DiscoveredPlugins { get; } = new();

    public List<RulesFile> Scan()
    {
        List<RulesFile> Results = new();

        ScanTree(Path.Combine(Directories.EngineRoot, "Engine"), PluginName: null, Results);

        if (Directories.ProjectRoot is not null)
        {
            ScanTree(Directories.ProjectRoot, PluginName: null, Results);
        }

        foreach (PluginDescriptor Plugin in DiscoverPlugins())
        {
            DiscoveredPlugins.Add(Plugin);
            ScanTree(Plugin.RootDirectory, Plugin.Name, Results);
        }

        // Later entries win so a project can override an engine module by name.
        Dictionary<string, RulesFile> Deduplicated = new(StringComparer.OrdinalIgnoreCase);
        List<RulesFile> Ordered = new();

        foreach (RulesFile File in Results)
        {
            string Key = File.Kind == RulesFileKind.Shared ? File.Location : $"{File.Kind}:{File.DeclaredName}";

            if (Deduplicated.TryGetValue(Key, out RulesFile? Existing))
            {
                Log.Verbose("Rules '{0}' overrides '{1}'", File.Location, Existing.Location);
                Ordered.Remove(Existing);
            }

            Deduplicated[Key] = File;
            Ordered.Add(File);
        }

        return Ordered;
    }

    private List<PluginDescriptor> DiscoverPlugins()
    {
        List<PluginDescriptor> Plugins = new();

        foreach (string Root in EnumeratePluginRoots())
        {
            if (!System.IO.Directory.Exists(Root))
            {
                continue;
            }

            foreach (string Descriptor in System.IO.Directory.EnumerateFiles(Root, "*.lplugin", SearchOption.AllDirectories))
            {
                if (IsIgnoredPath(Descriptor, Root))
                {
                    continue;
                }

                Plugins.Add(PluginDescriptor.Load(Descriptor));
            }
        }

        return Plugins;
    }

    private IEnumerable<string> EnumeratePluginRoots()
    {
        yield return Directories.EnginePluginsDirectory;

        if (Directories.ProjectRoot is not null)
        {
            yield return Directories.ProjectPluginsDirectory;
        }
    }

    private void ScanTree(string Root, string? PluginName, List<RulesFile> Results)
    {
        if (!System.IO.Directory.Exists(Root))
        {
            return;
        }

        foreach (string Candidate in System.IO.Directory.EnumerateFiles(Root, "*.cs", SearchOption.AllDirectories))
        {
            if (IsIgnoredPath(Candidate, Root))
            {
                continue;
            }

            RulesFileKind? Kind = ClassifyFile(Candidate);

            if (Kind is not null)
            {
                Results.Add(new RulesFile(PathUtils.Normalize(Candidate), Kind.Value, PluginName));
            }
        }
    }

    private static RulesFileKind? ClassifyFile(string FilePath)
    {
        string FileName = Path.GetFileName(FilePath);

        if (FileName.EndsWith(".Target.cs", StringComparison.OrdinalIgnoreCase))
        {
            return RulesFileKind.Target;
        }

        if (FileName.EndsWith(".Build.cs", StringComparison.OrdinalIgnoreCase))
        {
            return RulesFileKind.Module;
        }

        if (FileName.EndsWith(".BuildRules.cs", StringComparison.OrdinalIgnoreCase))
        {
            return RulesFileKind.Shared;
        }

        return null;
    }

    /// <summary>
    /// Whether a discovered file sits under an ignored directory, judged RELATIVE to the tree being
    /// scanned. The list prunes directories inside that tree; it must never get a say about the path
    /// leading up to it, which only describes where the checkout happens to live.
    /// </summary>
    /// <remarks>
    /// This split the absolute path, so any ignored name among the ancestors disqualified the whole
    /// tree. A git worktree under '.claude/worktrees/' (where Claude Code puts them) therefore had
    /// '.claude' in the absolute path of every rules file, all of them were ignored, and the build
    /// died claiming no Target.cs or Build.cs existed anywhere -- in a tree visibly full of them.
    /// Same relative-first shape ManagedProjectStep.EnumerateProjectSources uses.
    /// </remarks>
    private static bool IsIgnoredPath(string FilePath, string Root)
    {
        foreach (string Segment in PathUtils.MakeRelativeTo(FilePath, Root)
            .Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar))
        {
            foreach (string Ignored in IgnoredDirectoryNames)
            {
                if (Segment.Equals(Ignored, StringComparison.OrdinalIgnoreCase))
                {
                    return true;
                }
            }
        }

        return false;
    }
}
