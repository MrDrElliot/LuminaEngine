using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Configuration;

// Ties a project to an engine checkout rather than a version, so pulling a newer engine never strands a project.
public static class EngineAssociation
{
    public const string ProjectKey = "EngineAssociation";

    // Set on a forwarded run, so two engines naming each other cannot bounce a command forever.
    private const string ForwardedVariable = "LUMINA_FORWARDED_FROM";

    private static string StorePath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Lumina", "Engines.json");

    private static Dictionary<string, string> LoadStored() =>
        JsonStore.Load<Dictionary<string, string>>(StorePath) ?? new();

    private static string? FindId(Dictionary<string, string> Engines, string EngineRoot) =>
        Engines.FirstOrDefault(Pair => PathUtils.AreSame(Pair.Value, EngineRoot)).Key;

    // Engine ids on this machine mapped to their roots, leaving out checkouts that are missing right now.
    public static Dictionary<string, string> LoadEngines()
    {
        return LoadStored()
            .Where(Pair => Directory.Exists(Pair.Value))
            .ToDictionary(Pair => Pair.Key, Pair => Pair.Value, StringComparer.OrdinalIgnoreCase);
    }

    // Gives this checkout an id the first time it runs and returns it, so a project can name it from then on.
    public static string Register(string EngineRoot)
    {
        if (FindId(LoadStored(), EngineRoot) is string KnownId)
        {
            return KnownId;
        }

        // Missing roots stay stored, since a briefly unmounted drive must not lose its id.
        using BuildLock StoreLock = BuildLock.AcquireFile(
            StorePath + ".lock", "another build to finish registering its engine", TimeSpan.FromMinutes(1));
        Dictionary<string, string> Engines = LoadStored();
        if (FindId(Engines, EngineRoot) is string RacedId)
        {
            return RacedId;
        }

        string NewId = Guid.NewGuid().ToString("B").ToUpperInvariant();
        Engines[NewId] = EngineRoot;
        JsonStore.Save(StorePath, Engines);
        return NewId;
    }

    public static string? FindProjectFile(string ProjectRoot)
    {
        return Directory.Exists(ProjectRoot)
            ? Directory.EnumerateFiles(ProjectRoot, "*.lproject", SearchOption.TopDirectoryOnly).FirstOrDefault()
            : null;
    }

    public static string? ReadAssociation(string ProjectFile)
    {
        try
        {
            return JsonNode.Parse(File.ReadAllText(ProjectFile))?[ProjectKey]?.GetValue<string>();
        }
        catch (Exception Ex) when (Ex is IOException or JsonException or InvalidOperationException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    // An empty association removes the field, which leaves the project to whichever engine opens it.
    public static void WriteAssociation(string ProjectFile, string Association)
    {
        JsonObject Root;
        try
        {
            Root = JsonNode.Parse(File.ReadAllText(ProjectFile)) as JsonObject
                ?? throw new BuildException($"'{ProjectFile}' does not hold a JSON object.");
        }
        catch (JsonException Ex)
        {
            throw new BuildException($"'{ProjectFile}' is not valid JSON: {Ex.Message}");
        }

        if (Association.Length == 0)
        {
            Root.Remove(ProjectKey);
        }
        else
        {
            Root[ProjectKey] = Association;
        }

        PathUtils.WriteFileAtomic(
            ProjectFile,
            Root.ToJsonString(new JsonSerializerOptions { WriteIndented = true, IndentSize = 4 }),
            new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }

    // An engine id known on this machine, or an engine root given absolute or relative to the project.
    public static string? Resolve(string Association, string ProjectRoot)
    {
        if (LoadEngines().TryGetValue(Association, out string? Registered))
        {
            return Registered;
        }

        string Candidate = Path.GetFullPath(Path.Combine(ProjectRoot, Association));
        return Directory.Exists(Path.Combine(Candidate, "Engine", "Source", "Runtime")) ? PathUtils.Normalize(Candidate) : null;
    }

    // Reruns the whole command on the project's own engine. False when this engine is the one to use.
    public static bool TryForward(string ProjectRoot, string EngineRoot, IEnumerable<string> Args, out int ExitCode)
    {
        ExitCode = 0;
        if (Environment.GetEnvironmentVariable(ForwardedVariable) is not null)
        {
            return false;
        }

        string? ProjectFile = FindProjectFile(ProjectRoot);
        if (ProjectFile is null)
        {
            return false;
        }

        string? Association = ReadAssociation(ProjectFile);
        if (string.IsNullOrEmpty(Association))
        {
            return false;
        }

        string? TargetRoot = Resolve(Association, ProjectRoot);
        if (TargetRoot is null)
        {
            // Never a failure, since a teammate's engine id means nothing on this machine.
            Log.Warning(
                "{0} names engine '{1}', which is not on this machine, so the engine at {2} is used. Right-click the project and pick Switch Engine to choose another.",
                Path.GetFileName(ProjectFile), Association, EngineRoot);
            return false;
        }

        if (PathUtils.AreSame(TargetRoot, EngineRoot))
        {
            return false;
        }

        string Script = Path.Combine(TargetRoot, OperatingSystem.IsWindows() ? "LuminaBuild.bat" : "LuminaBuild.sh");
        if (!File.Exists(Script))
        {
            Log.Warning("{0} names the engine at {1}, which has no {2}, so this engine is used.", Path.GetFileName(ProjectFile), TargetRoot, Path.GetFileName(Script));
            return false;
        }

        Log.Info("{0} uses the engine at {1}.", Path.GetFileName(ProjectFile), TargetRoot);

        // That engine's script appends its own root, so this one's is dropped.
        List<string> Forwarded = Args.Where(Arg => !Arg.TrimStart('-', '/').StartsWith("EngineRoot", StringComparison.OrdinalIgnoreCase)).ToList();

        ProcessStartInfo StartInfo;
        if (OperatingSystem.IsWindows())
        {
            StartInfo = new ProcessStartInfo("cmd.exe")
            {
                Arguments = $"/c \"{PathUtils.Quote(Script)} {string.Join(' ', Forwarded.Select(PathUtils.Quote))}\"",
            };
        }
        else
        {
            StartInfo = new ProcessStartInfo(Script);
            Forwarded.ForEach(StartInfo.ArgumentList.Add);
        }

        StartInfo.UseShellExecute = false;
        StartInfo.Environment[ForwardedVariable] = EngineRoot;

        using Process Child = Process.Start(StartInfo) ?? throw new BuildException($"Could not start {Script}.");
        Child.WaitForExit();
        ExitCode = Child.ExitCode;
        return true;
    }
}
