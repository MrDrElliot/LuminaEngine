using System.Text.Json;
using System.Text.Json.Serialization;

namespace LuminaBuildTool.Core;

/// <summary>
/// Small JSON persistence helper for build caches. Never throws on a corrupt or missing cache;
/// a bad cache degrades to a full rebuild rather than failing the build.
/// </summary>
public static class JsonStore
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    public static T? Load<T>(string FilePath) where T : class
    {
        try
        {
            if (!File.Exists(FilePath))
            {
                return null;
            }

            return JsonSerializer.Deserialize<T>(File.ReadAllText(FilePath), Options);
        }
        catch (Exception Ex) when (Ex is IOException or JsonException or UnauthorizedAccessException)
        {
            Log.Verbose("Discarding unreadable cache '{0}': {1}", FilePath, Ex.Message);
            return null;
        }
    }

    public static void Save<T>(string FilePath, T Value)
    {
        try
        {
            PathUtils.EnsureDirectoryForFile(FilePath);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(Value, Options));
        }
        catch (Exception Ex) when (Ex is IOException or UnauthorizedAccessException)
        {
            Log.Warning("Failed to write cache '{0}': {1}", FilePath, Ex.Message);
        }
    }
}
