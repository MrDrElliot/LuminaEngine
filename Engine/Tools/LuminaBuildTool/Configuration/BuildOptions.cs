using System.Text.Json;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Configuration;

/// <summary>
/// How an optional feature resolves. Auto lets the rules decide per configuration.
/// </summary>
public enum FeatureMode
{
    Auto,
    On,
    Off,
}

/// <summary>
/// Optional-feature switches, resolved from Engine/Build/BuildConfiguration.json and then from
/// the command line, which wins.
/// </summary>
/// <remarks>
/// Only the mode lives here. What "auto" means for a given feature is engine policy and belongs
/// in the shared rules file, so the tool stays free of engine-specific knowledge.
/// </remarks>
public sealed class BuildOptions
{
    private readonly Dictionary<string, FeatureMode> Modes = new(StringComparer.OrdinalIgnoreCase);

    public static BuildOptions Empty { get; } = new();

    /// <summary>
    /// Compile every source as its own translation unit regardless of what the rules say. Set by
    /// -NoUnity, so ruling unity out as the cause of a failure never means editing a Build.cs.
    /// </summary>
    public bool bDisableUnityBuild { get; set; }

    public FeatureMode GetMode(string Feature)
    {
        return Modes.TryGetValue(Feature, out FeatureMode Mode) ? Mode : FeatureMode.Auto;
    }

    public IReadOnlyDictionary<string, FeatureMode> All => Modes;

    /// <summary>
    /// Loads the config file, then applies matching command line switches such as -Tracy=off.
    /// An unreadable or missing file leaves every feature on Auto.
    /// </summary>
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
            }
        }
        catch (Exception Ex) when (Ex is IOException or JsonException)
        {
            Log.Warning("Could not read '{0}': {1}. Using automatic feature defaults.", ConfigFile, Ex.Message);
        }

        if (Arguments is not null)
        {
            Options.bDisableUnityBuild = Arguments.HasFlag("NoUnity");

            // A feature is overridable by name, so -Tracy=off works without the tool knowing what
            // Tracy is. Only names already present in the config file, plus any explicitly passed.
            foreach (string Feature in Options.Modes.Keys.ToList())
            {
                if (TryParseMode(Arguments.GetString(Feature), out FeatureMode Mode))
                {
                    Options.Modes[Feature] = Mode;
                }
            }
        }

        return Options;
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
