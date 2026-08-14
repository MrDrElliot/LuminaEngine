using System.Collections.Concurrent;
using System.Text.Json;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;

namespace LuminaBuildTool.Execution;

/// <summary>Serialized form of the header dependency cache.</summary>
public sealed class DependencyCacheData
{
    public List<string> Paths { get; set; } = new();

    /// <summary>Output file to indices into <see cref="Paths"/>.</summary>
    public Dictionary<string, List<int>> Dependencies { get; set; } = new();
}

/// <summary>Records which headers each output depended on, so editing one rebuilds exactly its includers.</summary>
public sealed class DependencyCache
{
    private readonly string CacheFile;

    private readonly ConcurrentDictionary<string, string[]> DependenciesByOutput =
        new(StringComparer.OrdinalIgnoreCase);

    private bool bDirty;

    private DependencyCache(string CacheFile)
    {
        this.CacheFile = CacheFile;
    }

    public static DependencyCache Load(string CacheFile)
    {
        DependencyCache Cache = new(CacheFile);
        DependencyCacheData? Data = JsonStore.Load<DependencyCacheData>(CacheFile);

        if (Data is null)
        {
            return Cache;
        }

        int Dropped = 0;

        foreach ((string Output, List<int> Indices) in Data.Dependencies)
        {
            // An output no longer on disk is rebuilt anyway, so this never affected freshness -- but orphaned
            // entries accumulate until they outnumber the live ones.
            if (!File.Exists(Output))
            {
                Dropped++;
                continue;
            }

            string[] Paths = new string[Indices.Count];
            bool bValid = true;

            for (int Index = 0; Index < Indices.Count; Index++)
            {
                int PathIndex = Indices[Index];

                if (PathIndex < 0 || PathIndex >= Data.Paths.Count)
                {
                    bValid = false;
                    break;
                }

                Paths[Index] = Data.Paths[PathIndex];
            }

            if (bValid)
            {
                Cache.DependenciesByOutput[Output] = Paths;
            }
        }

        if (Dropped > 0)
        {
            // Marked dirty so the pruned form is what gets written back, rather than the orphans
            // being reloaded and re-dropped on every build from here on.
            Cache.bDirty = true;
            Log.Verbose("Dropped header dependencies for {0} outputs that no longer exist", Dropped);
        }

        Log.Verbose("Loaded header dependencies for {0} outputs", Cache.DependenciesByOutput.Count);

        return Cache;
    }

    public void Save()
    {
        if (!bDirty)
        {
            return;
        }

        Dictionary<string, int> PathIndices = new(StringComparer.OrdinalIgnoreCase);
        DependencyCacheData Data = new();

        foreach ((string Output, string[] Paths) in DependenciesByOutput.OrderBy(P => P.Key, StringComparer.OrdinalIgnoreCase))
        {
            List<int> Indices = new(Paths.Length);

            foreach (string Path in Paths)
            {
                if (!PathIndices.TryGetValue(Path, out int Index))
                {
                    Index = Data.Paths.Count;
                    Data.Paths.Add(Path);
                    PathIndices[Path] = Index;
                }

                Indices.Add(Index);
            }

            Data.Dependencies[Output] = Indices;
        }

        JsonStore.Save(CacheFile, Data);
        bDirty = false;
    }

    public string[]? GetDependencies(string OutputFile)
    {
        return DependenciesByOutput.TryGetValue(OutputFile, out string[]? Paths) ? Paths : null;
    }

    /// <summary>Every recorded output and the header closure the compiler reported for it.</summary>
    public IEnumerable<KeyValuePair<string, string[]>> EnumerateRecorded()
    {
        return DependenciesByOutput;
    }

    public void Forget(string OutputFile)
    {
        if (DependenciesByOutput.TryRemove(OutputFile, out _))
        {
            bDirty = true;
        }
    }

    /// <summary>Records the header closure the compiler reported for one output file.</summary>
    public void RecordFromCompilerOutput(string OutputFile, string DependencyListFile, DependencyListFormat Format)
    {
        string[]? Includes = Format switch
        {
            DependencyListFormat.Makefile => ParseMakefileDependencies(DependencyListFile),
            _ => ParseSourceDependencies(DependencyListFile),
        };

        if (Includes is null)
        {
            return;
        }

        DependenciesByOutput[OutputFile] = Includes;
        bDirty = true;
    }

    /// <summary>Parses the MSVC /sourceDependencies document.</summary>
    private static string[]? ParseSourceDependencies(string DependencyListFile)
    {
        try
        {
            if (!File.Exists(DependencyListFile))
            {
                return null;
            }

            using JsonDocument Document = JsonDocument.Parse(File.ReadAllText(DependencyListFile));

            if (!Document.RootElement.TryGetProperty("Data", out JsonElement Data)
                || !Data.TryGetProperty("Includes", out JsonElement Includes)
                || Includes.ValueKind != JsonValueKind.Array)
            {
                return null;
            }

            List<string> Paths = new(Includes.GetArrayLength());

            foreach (JsonElement Entry in Includes.EnumerateArray())
            {
                string? Value = Entry.GetString();

                if (!string.IsNullOrEmpty(Value))
                {
                    Paths.Add(PathUtils.Normalize(Value));
                }
            }

            return Paths.ToArray();
        }
        catch (Exception Ex) when (Ex is IOException or JsonException)
        {
            Log.Verbose("Could not read dependency list '{0}': {1}", DependencyListFile, Ex.Message);
            return null;
        }
    }

    private static string[]? ParseMakefileDependencies(string DependencyListFile)
    {
        try
        {
            if (!File.Exists(DependencyListFile))
            {
                return null;
            }

            string Text = File.ReadAllText(DependencyListFile)
                .Replace("\\\r\n", " ")
                .Replace("\\\n", " ");

            string? Rule = Text
                .Split('\n')
                .FirstOrDefault(Line => Line.Trim().Length > 0);

            if (Rule is null)
            {
                return null;
            }

            int Separator = -1;

            for (int Index = 0; Index < Rule.Length; Index++)
            {
                if (Rule[Index] != ':' || (Index > 0 && Rule[Index - 1] == '\\'))
                {
                    continue;
                }

                if (Index == 1 && char.IsLetter(Rule[0]))
                {
                    continue;
                }

                Separator = Index;
                break;
            }

            if (Separator < 0)
            {
                return null;
            }

            List<string> Paths = new();
            System.Text.StringBuilder Current = new();

            for (int Index = Separator + 1; Index < Rule.Length; Index++)
            {
                char Character = Rule[Index];

                if (Character == '\\' && Index + 1 < Rule.Length && Rule[Index + 1] is ' ' or ':' or '#' or '\\')
                {
                    Current.Append(Rule[Index + 1]);
                    Index++;
                    continue;
                }

                if (Character is ' ' or '\t' or '\r')
                {
                    if (Current.Length > 0)
                    {
                        Paths.Add(PathUtils.Normalize(Current.ToString()));
                        Current.Clear();
                    }

                    continue;
                }

                Current.Append(Character);
            }

            if (Current.Length > 0)
            {
                Paths.Add(PathUtils.Normalize(Current.ToString()));
            }

            return Paths.Count > 0 ? Paths.ToArray() : null;
        }
        catch (Exception Ex) when (Ex is IOException)
        {
            Log.Verbose("Could not read dependency list '{0}': {1}", DependencyListFile, Ex.Message);
            return null;
        }
    }
}
