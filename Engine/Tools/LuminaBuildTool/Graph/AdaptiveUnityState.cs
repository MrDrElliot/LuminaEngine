using System.Text.Json;
using System.Text.Json.Serialization;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>Remembers which sources were edited recently so they compile alone rather than dragging a unity blob.</summary>
/// <remarks>Persisted per module, in the same directory as the blobs it steers.</remarks>
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

    // Keyed with the blobs it steers, not the target, or targets sharing intermediates rewrite each other's blobs.
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

    /// <summary>Holds out the sources git reports as changed, falling back to file timestamps where git cannot answer.</summary>
    public void Observe(IEnumerable<FileItem> Sources, int MaxFiles, IReadOnlySet<string>? Changed)
    {
        if (Changed is not null)
        {
            ObserveWorkingSet(Sources, MaxFiles, Changed);
            return;
        }

        ObserveTimestamps(Sources, MaxFiles);
    }

    // Derived fresh from the commit rather than accumulated, so a clean checkout holds nothing out anywhere.
    private void ObserveWorkingSet(IEnumerable<FileItem> Sources, int MaxFiles, IReadOnlySet<string> Changed)
    {
        List<FileItem> Held = Sources.Where(Source => Changed.Contains(Source.Location)).ToList();

        // Holding out more than the cap costs more than the blobs save, so the least recently touched stay merged.
        if (Held.Count > Math.Max(0, MaxFiles))
        {
            Held = Held.OrderByDescending(Source => Source.Timestamp).Take(Math.Max(0, MaxFiles)).ToList();
        }

        Dictionary<string, long> Next = new(StringComparer.OrdinalIgnoreCase);

        foreach (FileItem Source in Held)
        {
            Next[Source.Location] = Data.Build + 1;
        }

        bDirty |= Next.Count != Data.Files.Count || Next.Keys.Any(Path_ => !Data.Files.ContainsKey(Path_));

        Data.Files = Next;
        Data.Build++;
        Data.SeenUtc = DateTime.UtcNow;
    }

    private void ObserveTimestamps(IEnumerable<FileItem> Sources, int MaxFiles)
    {
        long Build = Data.Build + 1;

        // With no previous build every source reads as edited, so this one becomes the baseline and admits nothing.
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
