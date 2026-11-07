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

    public static string GetManifestPath(string BinariesDirectory, string OutputSuffix)
    {
        return Path.Combine(BinariesDirectory, $"Lumina{OutputSuffix}.modules");
    }

    // Changes exactly when a header a module can compile against does, which is when a module built earlier may disagree on a layout.
    public static string ComputeEngineBuildId(BuildDirectories Directories)
    {
        return BuildIdByEngineRoot.GetOrAdd(Directories.EngineRoot, _ => HashEngineHeaders(Directories));
    }

    public static void Write(BuildTarget Target, string BuildId)
    {
        if (Target.Rules.bMonolithic)
        {
            return;
        }

        IEnumerable<IGrouping<string, BuildModule>> ByDirectory = Target.Modules
            .Where(Module => Module.BinaryType == ModuleBinaryType.SharedLibrary && Module.OutputFile.Length > 0)
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

    private static string HashEngineHeaders(BuildDirectories Directories)
    {
        using IncrementalHash Hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        AppendText(Hash, BuildIdScheme);

        string[] EngineRoots = { Directories.EngineRuntimeDirectory, Directories.EngineEditorDirectory, Directories.EnginePluginsDirectory };
        foreach (string Header in EngineRoots.SelectMany(Root => EnumerateHeaders(Directories.EngineRoot, Root)))
        {
            AppendText(Hash, RelativeKey(Directories.EngineRoot, Header));

            // Line endings come from the checkout, not the interface.
            byte[] Contents = File.ReadAllBytes(Header);
            Hash.AppendData(Contents.Where(Byte => Byte != (byte)'\r').ToArray());
        }

        // Vendored headers are large and change only with an upgrade, which a size change is enough to catch.
        foreach (string Header in EnumerateHeaders(Directories.EngineRoot, Directories.ThirdPartyDirectory))
        {
            AppendText(Hash, $"{RelativeKey(Directories.EngineRoot, Header)}|{new FileInfo(Header).Length}");
        }

        return Convert.ToHexString(Hash.GetHashAndReset(), 0, 12).ToLowerInvariant();
    }

    private static IEnumerable<string> EnumerateHeaders(string EngineRoot, string Root)
    {
        if (!Directory.Exists(Root))
        {
            return Enumerable.Empty<string>();
        }

        return Directory.EnumerateFiles(Root, "*", SearchOption.AllDirectories)
            .Where(File => HeaderExtensions.Contains(Path.GetExtension(File), StringComparer.OrdinalIgnoreCase))
            .Where(File => !RelativeKey(Root, File).Split('/').Any(Segment => SkippedDirectoryNames.Contains(Segment, StringComparer.OrdinalIgnoreCase)))
            .OrderBy(File => RelativeKey(EngineRoot, File), StringComparer.Ordinal);
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
