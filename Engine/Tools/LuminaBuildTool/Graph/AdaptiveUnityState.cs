using System.Text.Json;
using System.Text.Json.Serialization;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>
/// Remembers which sources were edited recently so they compile on their own instead of dragging a
/// whole unity blob with them. Persisted per module, in the same directory as the blobs it steers.
/// </summary>
public sealed class AdaptiveUnityState
{
    private const string FileName = "AdaptiveUnity.json";

    private sealed class Payload
    {
        public long Build { get; set; }
        public DateTime SeenUtc { get; set; }

        [JsonPropertyName("Files")]
        public Dictionary<string, long> Files { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    }

    private readonly string Location;
    private readonly Payload Data;
    private readonly DateTime PreviousSeenUtc;
    private bool bDirty;

    private AdaptiveUnityState(string Location, Payload Data)
    {
        this.Location = Location;
        this.Data = Data;
        PreviousSeenUtc = Data.SeenUtc;
    }

    /// <summary>Files held out of unity blobs for this build.</summary>
    public IReadOnlyCollection<string> WorkingSet => Data.Files.Keys;

    public bool Contains(string NormalizedPath) => Data.Files.ContainsKey(NormalizedPath);

    // Keyed with the blobs it steers, not with the target. Several targets share one module's
    // intermediates, and a per-target working set makes each rewrite the other's blobs forever.
    public static AdaptiveUnityState Load(BuildModule Module)
    {
        string Path_ = Path.Combine(Module.IntermediateDirectory, FileName);

        try
        {
            if (File.Exists(Path_))
            {
                Payload? Loaded = JsonSerializer.Deserialize<Payload>(File.ReadAllText(Path_));

                if (Loaded is not null)
                {
                    // Case-insensitive on the way back in; System.Text.Json rebuilds the default comparer.
                    Loaded.Files = new Dictionary<string, long>(Loaded.Files, StringComparer.OrdinalIgnoreCase);
                    return new AdaptiveUnityState(Path_, Loaded);
                }
            }
        }
        catch (Exception Ex) when (Ex is IOException or JsonException or UnauthorizedAccessException)
        {
            // A losable cache. Starting over costs one unity-shaped build, never a wrong one.
            Log.Verbose("Adaptive unity: could not read '{0}': {1}", Path_, Ex.Message);
        }

        return new AdaptiveUnityState(Path_, new Payload());
    }

    /// <summary>
    /// Admits sources edited since the last build and evicts the least recently edited past the cap.
    /// Returns the paths that entered or left, which is what forces a blob to be rewritten.
    /// </summary>
    public void Observe(IEnumerable<FileItem> Sources, int MaxFiles)
    {
        long Build = Data.Build + 1;

        // With no previous build to compare against, every source reads as edited. Take this one as
        // the baseline and admit nothing, so the first build after a clone is a plain unity build.
        if (PreviousSeenUtc != default)
        {
            foreach (FileItem Source in Sources)
            {
                if (Source.Timestamp > PreviousSeenUtc)
                {
                    bDirty |= !Data.Files.TryGetValue(Source.Location, out long Seen) || Seen != Build;
                    Data.Files[Source.Location] = Build;
                }
            }
        }

        // Eviction is by cap rather than by age, so a stable working set never rewrites a blob.
        int Excess = Data.Files.Count - Math.Max(0, MaxFiles);

        if (Excess > 0)
        {
            foreach (string Stale in Data.Files.OrderBy(Pair => Pair.Value).Take(Excess).Select(Pair => Pair.Key).ToList())
            {
                Data.Files.Remove(Stale);
                bDirty = true;
            }
        }

        Data.Build = Build;
        Data.SeenUtc = DateTime.UtcNow;
    }

    /// <summary>Forgets everything, for when adaptive unity is turned off and blobs must go back to whole.</summary>
    public void Clear()
    {
        bDirty |= Data.Files.Count > 0;
        Data.Files.Clear();
        Data.Build = 0;
        Data.SeenUtc = DateTime.UtcNow;
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Location)!);
            File.WriteAllText(Location, JsonSerializer.Serialize(Data, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception Ex) when (Ex is IOException or UnauthorizedAccessException)
        {
            Log.Verbose("Adaptive unity: could not write '{0}': {1}", Location, Ex.Message);
        }
    }

    public bool HasChanged => bDirty;
}
