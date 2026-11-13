using System.Text.Json;
using System.Text.Json.Serialization;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Configuration;

/// <summary>One module entry inside a .lplugin descriptor.</summary>
public sealed class PluginModuleEntry
{
    public string Name { get; set; } = string.Empty;

    // Runtime, Editor, Developer or Reflection, the module types the engine's plugin loader knows.
    public string Type { get; set; } = "Runtime";

    public string LoadingPhase { get; set; } = "PreEngineInit";

    /// <summary>System names the module is built for; empty means every platform the plugin supports.</summary>
    public List<string> SupportedPlatforms { get; set; } = new();

    public ModuleHostType GetHostType()
    {
        return Type switch
        {
            "Editor" => ModuleHostType.Editor,
            "Developer" => ModuleHostType.Developer,
            "Reflection" => ModuleHostType.Reflection,
            _ => ModuleHostType.Runtime,
        };
    }

    public bool SupportsPlatform(BuildPlatform Platform)
    {
        return SupportedPlatforms.Count == 0 || SupportedPlatforms.Contains(Platform.GetSystemName(), StringComparer.Ordinal);
    }
}

/// <summary>A plugin another one needs, written as its bare name or as an object that can mark it optional or ask for a minimum Version.</summary>
[JsonConverter(typeof(PluginDependencyConverter))]
public sealed class PluginDependency
{
    public string Name { get; set; } = string.Empty;

    public bool Optional { get; set; }

    /// <summary>The lowest Version of the dependency that satisfies it; 0 accepts any.</summary>
    public int Version { get; set; }
}

/// <summary>Reads both dependency spellings, failing on a field of the wrong type exactly as the engine's loader does.</summary>
internal sealed class PluginDependencyConverter : JsonConverter<PluginDependency>
{
    public override PluginDependency Read(ref Utf8JsonReader Reader, Type TypeToConvert, JsonSerializerOptions Options)
    {
        if (Reader.TokenType == JsonTokenType.String)
        {
            return new PluginDependency { Name = Reader.GetString()! };
        }

        if (Reader.TokenType != JsonTokenType.StartObject)
        {
            throw new JsonException("A dependency must be a plugin name or an object with a Name.");
        }

        PluginDependency Result = new();
        while (Reader.Read() && Reader.TokenType != JsonTokenType.EndObject)
        {
            string Property = Reader.GetString()!;
            Reader.Read();
            switch (Property)
            {
                case "Name":
                    Result.Name = Reader.TokenType == JsonTokenType.String
                        ? Reader.GetString()!
                        : throw new JsonException("Dependency field 'Name' must be a string.");
                    break;
                case "Optional":
                    Result.Optional = Reader.TokenType is JsonTokenType.True or JsonTokenType.False
                        ? Reader.GetBoolean()
                        : throw new JsonException("Dependency field 'Optional' must be a boolean.");
                    break;
                case "Version":
                    Result.Version = Reader.TokenType == JsonTokenType.Number && Reader.TryGetInt32(out int Version)
                        ? Version
                        : throw new JsonException("Dependency field 'Version' must be an integer.");
                    break;
                default:
                    Reader.Skip();
                    break;
            }
        }

        return Result;
    }

    public override void Write(Utf8JsonWriter Writer, PluginDependency Value, JsonSerializerOptions Options)
    {
        Writer.WriteStartObject();
        Writer.WriteString("Name", Value.Name);
        Writer.WriteBoolean("Optional", Value.Optional);
        Writer.WriteNumber("Version", Value.Version);
        Writer.WriteEndObject();
    }
}

/// <summary>A .lplugin file. The schema, defaults and errors match FPluginDescriptor in the engine, so both read every descriptor alike.</summary>
public sealed class PluginDescriptor
{
    public static readonly string[] ModuleTypes = { "Runtime", "Editor", "Developer", "Reflection" };

    public static readonly string[] LoadingPhases =
        { "Earliest", "Core", "PreEngineInit", "EngineInit", "PostEngineInit", "PostProjectLoad", "EditorInit" };

    public int FormatVersion { get; set; } = 1;

    public string Name { get; set; } = string.Empty;

    public int Version { get; set; } = 1;

    public string VersionName { get; set; } = string.Empty;

    public string Author { get; set; } = string.Empty;

    public string Description { get; set; } = string.Empty;

    public string Category { get; set; } = string.Empty;

    public bool EnabledByDefault { get; set; } = true;

    public bool EditorOnly { get; set; }

    public bool ContainsContent { get; set; } = true;

    public List<string> SupportedPlatforms { get; set; } = new();

    public List<PluginDependency> Dependencies { get; set; } = new();

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

    /// <summary>SupportedPlatforms holds system names without architecture, matching LUMINA_SYSTEM_NAME exactly.</summary>
    public bool SupportsPlatform(BuildPlatform Platform)
    {
        return SupportedPlatforms.Count == 0 || SupportedPlatforms.Contains(Platform.GetSystemName(), StringComparer.Ordinal);
    }

    // Strict JSON with exact key names, since that is all the engine's parser accepts.
    private static readonly JsonSerializerOptions ParseOptions = new()
    {
        RespectNullableAnnotations = true,
    };

    public static PluginDescriptor Load(string DescriptorPath)
    {
        PluginDescriptor? Descriptor;

        try
        {
            Descriptor = JsonSerializer.Deserialize<PluginDescriptor>(File.ReadAllText(DescriptorPath), ParseOptions);
        }
        catch (JsonException Ex)
        {
            throw new BuildException($"Failed to parse plugin descriptor '{DescriptorPath}': {Ex.Message}");
        }

        if (Descriptor is null)
        {
            throw new BuildException($"Plugin descriptor '{DescriptorPath}' is empty.");
        }

        string? Problem = Descriptor.Validate();
        if (Problem is not null)
        {
            throw new BuildException($"Plugin descriptor '{DescriptorPath}': {Problem}");
        }

        Descriptor.DescriptorFile = PathUtils.Normalize(DescriptorPath);
        Descriptor.RootDirectory = Path.GetDirectoryName(Descriptor.DescriptorFile)!;

        return Descriptor;
    }

    private string? Validate()
    {
        if (string.IsNullOrEmpty(Name))
        {
            return "missing required field 'Name'.";
        }

        if (SupportedPlatforms.Any(Platform => Platform is null))
        {
            return "'SupportedPlatforms' must hold only strings.";
        }

        foreach (PluginDependency Dependency in Dependencies)
        {
            if (Dependency is null || string.IsNullOrEmpty(Dependency.Name))
            {
                return "a dependency has no Name.";
            }
        }

        foreach (PluginModuleEntry Module in Modules)
        {
            if (Module is null || string.IsNullOrEmpty(Module.Name))
            {
                return "a module has no Name.";
            }

            if (!ModuleTypes.Contains(Module.Type, StringComparer.Ordinal))
            {
                return $"module '{Module.Name}' has unknown Type '{Module.Type}'; expected one of {string.Join(", ", ModuleTypes)}.";
            }

            if (!LoadingPhases.Contains(Module.LoadingPhase, StringComparer.Ordinal))
            {
                return $"module '{Module.Name}' has unknown LoadingPhase '{Module.LoadingPhase}'.";
            }

            if (Module.SupportedPlatforms.Any(Platform => Platform is null))
            {
                return $"module '{Module.Name}' 'SupportedPlatforms' must hold only strings.";
            }
        }

        return null;
    }
}
