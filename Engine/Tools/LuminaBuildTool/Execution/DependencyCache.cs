using System.Collections.Concurrent;
using System.Text.Json;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Execution;

/// <summary>
/// Serialized form of the header dependency cache. Paths go through a shared pool because the
/// same engine headers appear in nearly every translation unit's include closure.
/// </summary>
public sealed class DependencyCacheData
{
    public List<string> Paths { get; set; } = new();

    /// <summary>Output file to indices into <see cref="Paths"/>.</summary>
    public Dictionary<string, List<int>> Dependencies { get; set; } = new();
}

/// <summary>
/// Remembers which headers each compiled output actually depended on, so editing a header
/// rebuilds exactly the translation units that included it.
/// </summary>
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

        foreach ((string Output, List<int> Indices) in Data.Dependencies)
        {
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

    public void Forget(string OutputFile)
    {
        if (DependenciesByOutput.TryRemove(OutputFile, out _))
        {
            bDirty = true;
        }
    }

    /// <summary>
    /// Reads the JSON the compiler emitted for one translation unit and records its include
    /// closure against the object file it produced.
    /// </summary>
    public void RecordFromCompilerOutput(string OutputFile, string DependencyListFile)
    {
        string[]? Includes = ParseSourceDependencies(DependencyListFile);

        if (Includes is null)
        {
            return;
        }

        DependenciesByOutput[OutputFile] = Includes;
        bDirty = true;
    }

    /// <summary>
    /// Parses the MSVC /sourceDependencies document. Returns null when the file is absent or
    /// malformed, which downgrades to a timestamp-only decision rather than a wrong skip.
    /// </summary>
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
}
