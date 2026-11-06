using System.Text.Json;
using System.Text.Json.Serialization;

namespace LuminaBuildTool.Core;

/// <summary>Small JSON persistence helper for build caches.</summary>
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
        string TempPath = $"{FilePath}.{Environment.ProcessId}.tmp";
        try
        {
            PathUtils.EnsureDirectoryForFile(FilePath);
            File.WriteAllText(TempPath, JsonSerializer.Serialize(Value, Options));
            File.Move(TempPath, FilePath, overwrite: true);
        }
        catch (Exception Ex) when (Ex is IOException or UnauthorizedAccessException)
        {
            Log.Warning("Failed to write cache '{0}': {1}", FilePath, Ex.Message);
            try
            {
                File.Delete(TempPath);
            }
            catch (Exception DeleteEx) when (DeleteEx is IOException or UnauthorizedAccessException)
            {
            }
        }
    }
}
