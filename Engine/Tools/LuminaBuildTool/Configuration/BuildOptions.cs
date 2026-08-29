using System.Text.Json;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Configuration;

/// <summary>How an optional feature resolves. Auto lets the rules decide per configuration.</summary>
public enum FeatureMode
{
    Auto,
    On,
    Off,
}

/// <summary>Feature switches from BuildConfiguration.json, then the command line, which wins.</summary>
public sealed class BuildOptions
{
    private readonly Dictionary<string, FeatureMode> Modes = new(StringComparer.OrdinalIgnoreCase);

    private readonly Dictionary<string, bool> PluginOverrides = new(StringComparer.OrdinalIgnoreCase);

    public static BuildOptions Empty { get; } = new();

    /// <summary>A copy with the PGO request dropped, for a target being built only as a prerequisite.</summary>
    public BuildOptions WithoutPgo()
    {
        BuildOptions Copy = new() { bDisableUnityBuild = bDisableUnityBuild };

        foreach (KeyValuePair<string, FeatureMode> Mode in Modes)
        {
            Copy.Modes[Mode.Key] = Mode.Value;
        }

        foreach (KeyValuePair<string, bool> Plugin in PluginOverrides)
        {
            Copy.PluginOverrides[Plugin.Key] = Plugin.Value;
        }

        return Copy;
    }

    /// <summary>Compile every source as its own translation unit regardless of what the rules say.</summary>
    public bool bDisableUnityBuild { get; set; }

    /// <summary>Which half of a PGO cycle was asked for, or null to leave the target rules alone.</summary>
    public PgoMode? Pgo { get; set; }

    public FeatureMode GetMode(string Feature)
    {
        return Modes.TryGetValue(Feature, out FeatureMode Mode) ? Mode : FeatureMode.Auto;
    }

    public IReadOnlyDictionary<string, FeatureMode> All => Modes;

    /// <summary>Plugins someone asked for by name, which is what lets an opt-in plugin compile at all.</summary>
    public IReadOnlyDictionary<string, bool> PluginSettings => PluginOverrides;

    /// <summary>Null when nothing named this plugin, leaving its own EnabledByDefault to decide.</summary>
    public bool? GetPluginOverride(string Name)
    {
        return PluginOverrides.TryGetValue(Name, out bool bEnabled) ? bEnabled : null;
    }

    /// <summary>Loads the config file, then applies matching command line switches such as -Tracy=off.</summary>
    public static BuildOptions Load(BuildDirectories Directories, CommandLine? Arguments)
    {
        BuildOptions Options = new();
        string ConfigFile = Path.Combine(Directories.EngineRoot, "Engine", "Build", "BuildConfiguration.json");

        try
        {
            if (File.Exists(ConfigFile))
            {
                using JsonDocument Document = JsonDocument.Parse(File.ReadAllText(ConfigFile));

                if (Document.RootElement.TryGetProperty("Features", out JsonElement Features))
                {
                    foreach (JsonProperty Feature in Features.EnumerateObject())
                    {
                        if (TryParseMode(Feature.Value.GetString(), out FeatureMode Mode))
                        {
                            Options.Modes[Feature.Name] = Mode;
                        }
                        else
                        {
                            Log.Warning(
                                "Ignoring feature '{0}' in '{1}': value must be auto, on or off.",
                                Feature.Name,
                                ConfigFile);
                        }
                    }
                }

                if (Document.RootElement.TryGetProperty("Plugins", out JsonElement Plugins)
                    && Plugins.ValueKind == JsonValueKind.Object)
                {
                    foreach (JsonProperty Plugin in Plugins.EnumerateObject())
                    {
                        if (Plugin.Value.ValueKind is JsonValueKind.True or JsonValueKind.False)
                        {
                            Options.PluginOverrides[Plugin.Name] = Plugin.Value.GetBoolean();
                        }
                    }
                }
            }
        }
        catch (Exception Ex) when (Ex is IOException or JsonException)
        {
            Log.Warning("Could not read '{0}': {1}. Using automatic feature defaults.", ConfigFile, Ex.Message);
        }

        // The runtime reads the same list, so a plugin the project enables is also one that gets built.
        Options.LoadProjectPlugins(Directories);

        if (Arguments is not null)
        {
            Options.bDisableUnityBuild = Arguments.HasFlag("NoUnity");

            if (Arguments.GetString("Pgo") is { Length: > 0 } PgoArgument)
            {
                if (!Enum.TryParse(PgoArgument, ignoreCase: true, out PgoMode PgoValue))
                {
                    throw new BuildException($"-Pgo={PgoArgument} is not one of off, instrument or optimize.");
                }

                Options.Pgo = PgoValue;
            }

            // A feature is overridable by name, so -Tracy=off works without the tool knowing what
            // Tracy is. Only names already present in the config file, plus any explicitly passed.
            foreach (string Feature in Options.Modes.Keys.ToList())
            {
                if (TryParseMode(Arguments.GetString(Feature), out FeatureMode Mode))
                {
                    Options.Modes[Feature] = Mode;
                }
            }

            Options.ApplyPluginArguments(Arguments.GetString("EnablePlugin"), true);
            Options.ApplyPluginArguments(Arguments.GetString("DisablePlugin"), false);
        }

        return Options;
    }

    /// <summary>Reads the project's .lproject plugin list, the same one the runtime honors at load.</summary>
    private void LoadProjectPlugins(BuildDirectories Directories)
    {
        if (Directories.ProjectRoot is null)
        {
            return;
        }

        string[] Descriptors;
        try
        {
            Descriptors = Directory.GetFiles(Directories.ProjectRoot, "*.lproject", SearchOption.TopDirectoryOnly);
        }
        catch (Exception Ex) when (Ex is IOException or UnauthorizedAccessException)
        {
            Log.Warning("Could not scan '{0}' for a project descriptor: {1}", Directories.ProjectRoot, Ex.Message);
            return;
        }

        foreach (string Descriptor in Descriptors)
        {
            try
            {
                using JsonDocument Document = JsonDocument.Parse(File.ReadAllText(Descriptor));

                if (!Document.RootElement.TryGetProperty("Plugins", out JsonElement Plugins)
                    || Plugins.ValueKind != JsonValueKind.Array)
                {
                    continue;
                }

                foreach (JsonElement Entry in Plugins.EnumerateArray())
                {
                    if (!Entry.TryGetProperty("Name", out JsonElement Name) || Name.ValueKind != JsonValueKind.String)
                    {
                        continue;
                    }

                    string? PluginName = Name.GetString();
                    if (string.IsNullOrWhiteSpace(PluginName))
                    {
                        continue;
                    }

                    // An entry with no Enabled member is taken as asking for the plugin.
                    bool bEnabled = !Entry.TryGetProperty("Enabled", out JsonElement Enabled)
                        || Enabled.ValueKind != JsonValueKind.False;

                    PluginOverrides[PluginName] = bEnabled;
                }
            }
            catch (Exception Ex) when (Ex is IOException or JsonException)
            {
                Log.Warning("Could not read '{0}': {1}. Its plugin list is ignored.", Descriptor, Ex.Message);
            }
        }
    }

    private void ApplyPluginArguments(string? Names, bool bEnabled)
    {
        if (string.IsNullOrWhiteSpace(Names))
        {
            return;
        }

        foreach (string Name in Names.Split(new[] { ',', ';' }, StringSplitOptions.RemoveEmptyEntries))
        {
            string Trimmed = Name.Trim();
            if (Trimmed.Length > 0)
            {
                PluginOverrides[Trimmed] = bEnabled;
            }
        }
    }

    private static bool TryParseMode(string? Value, out FeatureMode Mode)
    {
        Mode = FeatureMode.Auto;

        if (string.IsNullOrEmpty(Value))
        {
            return false;
        }

        return Enum.TryParse(Value, ignoreCase: true, out Mode);
    }

    public override string ToString()
    {
        return string.Join(", ", Modes.Select(P => $"{P.Key}={P.Value}"));
    }
}
