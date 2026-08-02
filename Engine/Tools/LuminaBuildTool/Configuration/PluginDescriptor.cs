using System.Text.Json;
using System.Text.Json.Serialization;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Configuration;

/// <summary>
/// One module entry inside a .lplugin descriptor.
/// </summary>
public sealed class PluginModuleEntry
{
    public string Name { get; set; } = string.Empty;

    /// <summary>Runtime, Editor, Developer or Program.</summary>
    public string Type { get; set; } = "Runtime";

    public string LoadingPhase { get; set; } = "Default";

    public ModuleHostType GetHostType()
    {
        return Type switch
        {
            "Editor" => ModuleHostType.Editor,
            "Developer" => ModuleHostType.Developer,
            "Program" => ModuleHostType.Program,
            _ => ModuleHostType.Runtime,
        };
    }
}

/// <summary>
/// A .lplugin file. This is the single source of truth for a plugin's identity and module list;
/// each listed module must have a matching Build.cs under the plugin's Source directory.
/// </summary>
public sealed class PluginDescriptor
{
    public int FormatVersion { get; set; } = 1;

    public string Name { get; set; } = string.Empty;

    public int Version { get; set; } = 1;

    public string VersionName { get; set; } = string.Empty;

    public string Author { get; set; } = string.Empty;

    public string Description { get; set; } = string.Empty;

    public string Category { get; set; } = string.Empty;

    public bool EnabledByDefault { get; set; }

    public bool EditorOnly { get; set; }

    public bool ContainsContent { get; set; }

    public List<string> SupportedPlatforms { get; set; } = new();

    public List<string> Dependencies { get; set; } = new();

    public List<PluginModuleEntry> Modules { get; set; } = new();

    /// <summary>Absolute path of the .lplugin file.</summary>
    [JsonIgnore]
    public string DescriptorFile { get; set; } = string.Empty;

    /// <summary>Plugin root directory, the one containing the .lplugin.</summary>
    [JsonIgnore]
    public string RootDirectory { get; set; } = string.Empty;

    [JsonIgnore]
    public string SourceDirectory => Path.Combine(RootDirectory, "Source");

    [JsonIgnore]
    public string BinariesDirectory => Path.Combine(RootDirectory, "Binaries");

    /// <summary>
    /// SupportedPlatforms entries are system names without architecture, matching how the runtime
    /// compares them against LUMINA_SYSTEM_NAME.
    /// </summary>
    public bool SupportsPlatform(BuildPlatform Platform)
    {
        return SupportedPlatforms.Count == 0
            || SupportedPlatforms.Contains(Platform.GetSystemName(), StringComparer.OrdinalIgnoreCase);
    }

    public static PluginDescriptor Load(string DescriptorPath)
    {
        JsonSerializerOptions Options = new()
        {
            PropertyNameCaseInsensitive = true,
            ReadCommentHandling = JsonCommentHandling.Skip,
            AllowTrailingCommas = true,
        };

        PluginDescriptor? Descriptor;

        try
        {
            Descriptor = JsonSerializer.Deserialize<PluginDescriptor>(File.ReadAllText(DescriptorPath), Options);
        }
        catch (JsonException Ex)
        {
            throw new BuildException($"Failed to parse plugin descriptor '{DescriptorPath}': {Ex.Message}");
        }

        if (Descriptor is null)
        {
            throw new BuildException($"Plugin descriptor '{DescriptorPath}' is empty.");
        }

        Descriptor.DescriptorFile = PathUtils.Normalize(DescriptorPath);
        Descriptor.RootDirectory = Path.GetDirectoryName(Descriptor.DescriptorFile)!;

        if (string.IsNullOrEmpty(Descriptor.Name))
        {
            Descriptor.Name = Path.GetFileNameWithoutExtension(DescriptorPath);
        }

        return Descriptor;
    }
}
