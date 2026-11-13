using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

// The .modules manifest beside each set of shared libraries, which FModuleManager reads to refuse a module built against other engine headers.
public static class ModuleManifest
{
    private const string BuildIdScheme = "LuminaBuildId/1";

    private static readonly string[] HeaderExtensions = { ".h", ".hpp", ".inl" };

    private static readonly string[] SkippedDirectoryNames = { "Intermediates", "Binaries" };

    private static readonly ConcurrentDictionary<string, string> BuildIdByEngineRoot = new(StringComparer.OrdinalIgnoreCase);

    private sealed class BuildIdRecord
    {
        public string Signature { get; set; } = string.Empty;
        public string BuildId { get; set; } = string.Empty;
    }

    public static string GetManifestPath(string BinariesDirectory, string OutputSuffix)
    {
        return Path.Combine(BinariesDirectory, $"Lumina{OutputSuffix}.modules");
    }

    // Changes exactly when a header a module can compile against does, which is when a module built earlier may disagree on a layout.
    public static string ComputeEngineBuildId(BuildDirectories Directories)
    {
        return BuildIdByEngineRoot.GetOrAdd(Directories.EngineRoot, _ => LoadOrHashEngineHeaders(Directories));
    }

    // Header bytes are read again only when a size or timestamp moved since the recorded BuildId, as DependencyCache treats staleness.
    internal static string LoadOrHashEngineHeaders(BuildDirectories Directories)
    {
        FileInfo[] EngineHeaders = new[] { Directories.EngineRuntimeDirectory, Directories.EngineEditorDirectory, Directories.EnginePluginsDirectory }
            .SelectMany(Root => EnumerateHeaders(Directories.EngineRoot, Root))
            .ToArray();
        FileInfo[] VendoredHeaders = EnumerateHeaders(Directories.EngineRoot, Directories.ThirdPartyDirectory).ToArray();

        string Signature = ContentHash.OfStrings(EngineHeaders.Concat(VendoredHeaders)
            .Select(Header => $"{RelativeKey(Directories.EngineRoot, Header.FullName)}|{Header.Length}|{Header.LastWriteTimeUtc.Ticks}")
            .Prepend(BuildIdScheme));

        string RecordPath = Path.Combine(Directories.EngineRoot, "Intermediates", "BuildTool", "EngineBuildId.json");
        if (JsonStore.Load<BuildIdRecord>(RecordPath) is { } Record && Record.Signature == Signature && Record.BuildId.Length > 0)
        {
            return Record.BuildId;
        }

        string BuildId = HashEngineHeaders(Directories.EngineRoot, EngineHeaders, VendoredHeaders);
        JsonStore.Save(RecordPath, new BuildIdRecord { Signature = Signature, BuildId = BuildId });
        return BuildId;
    }

    public static void Write(BuildTarget Target, string BuildId)
    {
        if (Target.Rules.bMonolithic)
        {
            return;
        }

        IEnumerable<IGrouping<string, BuildModule>> ByDirectory = Target.Modules
            .Where(Module => Module.BinaryType == ModuleBinaryType.SharedLibrary && Module.OutputFile.Length > 0)
            .Where(Module => Module.Rules.HostType != ModuleHostType.Reflection)
            .GroupBy(Module => Module.OutputDirectory, StringComparer.OrdinalIgnoreCase);

        foreach (IGrouping<string, BuildModule> Directory in ByDirectory)
        {
            string ManifestPath = GetManifestPath(Directory.Key, Target.Rules.OutputSuffix);

            // Other targets share the directory and suffix, and their modules keep the BuildId they were last linked with.
            SortedDictionary<string, string> Modules = ReadModules(ManifestPath);
            foreach (BuildModule Module in Directory)
            {
                Modules[Module.Name] = BuildId;
            }

            JsonObject Manifest = new()
            {
                ["Modules"] = new JsonObject(Modules.Select(Pair => KeyValuePair.Create(Pair.Key, (JsonNode?)JsonValue.Create(Pair.Value)))),
            };

            PathUtils.WriteFileIfChanged(
                ManifestPath,
                Manifest.ToJsonString(new JsonSerializerOptions { WriteIndented = true }),
                bByteOrderMark: false);
        }
    }

    private static SortedDictionary<string, string> ReadModules(string ManifestPath)
    {
        SortedDictionary<string, string> Modules = new(StringComparer.Ordinal);

        try
        {
            if (File.Exists(ManifestPath) && JsonNode.Parse(File.ReadAllText(ManifestPath))?["Modules"] is JsonObject Existing)
            {
                foreach ((string Name, JsonNode? Value) in Existing)
                {
                    if (Value is JsonValue Id && Id.TryGetValue(out string? Text))
                    {
                        Modules[Name] = Text;
                    }
                }
            }
        }
        catch (Exception Ex) when (Ex is IOException or JsonException or UnauthorizedAccessException)
        {
            Log.Warning("Replacing the unreadable module manifest {0}: {1}", ManifestPath, Ex.Message);
        }

        return Modules;
    }

    private static string HashEngineHeaders(string EngineRoot, FileInfo[] EngineHeaders, FileInfo[] VendoredHeaders)
    {
        using IncrementalHash Hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        AppendText(Hash, BuildIdScheme);

        foreach (FileInfo Header in EngineHeaders)
        {
            AppendText(Hash, RelativeKey(EngineRoot, Header.FullName));

            // Line endings come from the checkout, not the interface.
            byte[] Contents = File.ReadAllBytes(Header.FullName);
            Hash.AppendData(Contents.Where(Byte => Byte != (byte)'\r').ToArray());
        }

        // Vendored headers are large and change only with an upgrade, which a size change is enough to catch.
        foreach (FileInfo Header in VendoredHeaders)
        {
            AppendText(Hash, $"{RelativeKey(EngineRoot, Header.FullName)}|{Header.Length}");
        }

        return Convert.ToHexString(Hash.GetHashAndReset(), 0, 12).ToLowerInvariant();
    }

    // Enumerated as FileInfo so the size and timestamp come with the directory listing instead of a stat per header.
    private static IEnumerable<FileInfo> EnumerateHeaders(string EngineRoot, string Root)
    {
        if (!Directory.Exists(Root))
        {
            return Enumerable.Empty<FileInfo>();
        }

        return new DirectoryInfo(Root).EnumerateFiles("*", SearchOption.AllDirectories)
            .Where(File => HeaderExtensions.Contains(File.Extension, StringComparer.OrdinalIgnoreCase))
            .Where(File => !RelativeKey(Root, File.FullName).Split('/').Any(Segment => SkippedDirectoryNames.Contains(Segment, StringComparer.OrdinalIgnoreCase)))
            .OrderBy(File => RelativeKey(EngineRoot, File.FullName), StringComparer.Ordinal);
    }

    private static string RelativeKey(string Root, string FilePath)
    {
        return Path.GetRelativePath(Root, FilePath).Replace('\\', '/');
    }

    private static void AppendText(IncrementalHash Hash, string Text)
    {
        byte[] Bytes = Encoding.UTF8.GetBytes(Text);
        Hash.AppendData(BitConverter.GetBytes(Bytes.Length));
        Hash.AppendData(Bytes);
    }
}
