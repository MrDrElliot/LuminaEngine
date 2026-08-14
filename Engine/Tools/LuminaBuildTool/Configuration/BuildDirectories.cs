using LuminaBuildTool.Core;

namespace LuminaBuildTool.Configuration;

/// <summary>Resolved directory layout for one build.</summary>
public sealed class BuildDirectories
{
    private BuildDirectories(string EngineRoot, string? ProjectRoot)
    {
        this.EngineRoot = EngineRoot;
        this.ProjectRoot = ProjectRoot;
    }

    /// <summary>Engine installation root, the directory containing Engine/Source/Runtime.</summary>
    public string EngineRoot { get; }

    /// <summary>Game project root, or null when building the engine itself.</summary>
    public string? ProjectRoot { get; }

    /// <summary>Root that owns Binaries and Intermediates for this build.</summary>
    public string OutputRoot => ProjectRoot ?? EngineRoot;

    public string EngineSourceDirectory => Path.Combine(EngineRoot, "Engine", "Source");

    public string EngineRuntimeDirectory => Path.Combine(EngineSourceDirectory, "Runtime");

    public string EngineEditorDirectory => Path.Combine(EngineRoot, "Engine", "Editor");

    public string ThirdPartyDirectory => Path.Combine(EngineSourceDirectory, "ThirdParty");

    public string EngineApplicationsDirectory => Path.Combine(EngineRoot, "Engine", "Applications");

    public string EnginePluginsDirectory => Path.Combine(EngineRoot, "Engine", "Plugins");

    public string ProjectPluginsDirectory => ProjectRoot is null ? string.Empty : Path.Combine(ProjectRoot, "Plugins");

    public string IntermediatesDirectory => Path.Combine(OutputRoot, "Intermediates");

    public string BuildToolIntermediatesDirectory => Path.Combine(IntermediatesDirectory, "BuildTool");

    public string ReflectionDirectory => Path.Combine(IntermediatesDirectory, "Reflection");

    public string ProjectFilesDirectory => Path.Combine(IntermediatesDirectory, "ProjectFiles");

    public string BinariesDirectory(BuildPlatform Platform)
    {
        return Path.Combine(OutputRoot, "Binaries", Platform.GetOutputDirectoryName());
    }

    /// <summary>Directory the reflection generator writes C# bindings to.</summary>
    public string CSharpBindingsDirectory => Path.Combine(IntermediatesDirectory, "CSharpBindings");

    /// <summary>Where one target's intermediates live.</summary>
    public string ObjectDirectory(string TargetName, BuildPlatform Platform, BuildConfiguration Configuration, TargetType Type)
    {
        return Path.Combine(
            IntermediatesDirectory,
            "Obj",
            Platform.GetOutputDirectoryName(),
            TargetName,
            $"{Type}-{Configuration}");
    }

    /// <summary>Where build records live: what each output was built from, and which headers it included.</summary>
    public string BuildRecordDirectory(BuildPlatform Platform, BuildConfiguration Configuration, TargetType Type)
    {
        return Path.Combine(
            EngineRoot,
            "Intermediates",
            "Obj",
            Platform.GetOutputDirectoryName(),
            $"{Type}-{Configuration}",
            ".buildtool");
    }

    /// <summary>Root that owns the output of whatever lives at the given path.</summary>
    public string GetOutputRootFor(string ModuleDirectory)
    {
        return IsEngineOwned(ModuleDirectory) ? EngineRoot : ProjectRoot ?? EngineRoot;
    }

    /// <summary>Whether the code at this path belongs to the engine rather than the game project.</summary>
    public bool IsEngineOwned(string ModuleDirectory)
    {
        if (ProjectRoot is null)
        {
            return true;
        }

        return PathUtils.IsUnder(ModuleDirectory, EngineRoot) && !PathUtils.IsUnder(ModuleDirectory, ProjectRoot);
    }

    public string EnginePath(string RelativePath) => PathUtils.Combine(EngineRoot, RelativePath);

    public string ThirdPartyPath(string RelativePath) => PathUtils.Combine(ThirdPartyDirectory, RelativePath);

    /// <summary>Locates the engine root: explicit override, then LUMINA_DIR, then a walk up from this exe.</summary>
    public static BuildDirectories Discover(string? EngineRootOverride, string? ProjectRootOverride)
    {
        string? Root = EngineRootOverride;

        if (string.IsNullOrEmpty(Root))
        {
            Root = Environment.GetEnvironmentVariable("LUMINA_DIR");
        }

        if (string.IsNullOrEmpty(Root))
        {
            Root = SearchUpwardForEngineRoot(AppContext.BaseDirectory)
                ?? SearchUpwardForEngineRoot(System.IO.Directory.GetCurrentDirectory());
        }

        if (string.IsNullOrEmpty(Root))
        {
            throw new BuildException(
                "Could not locate the Lumina engine root. Pass -EngineRoot=<path>, set LUMINA_DIR, "
                + "or run the tool from inside an engine tree.");
        }

        Root = PathUtils.Normalize(Root);

        if (!LooksLikeEngineRoot(Root))
        {
            throw new BuildException(
                $"'{Root}' does not contain Engine/Source/Runtime, so it is not an engine root. "
                + "Fix LUMINA_DIR or pass -EngineRoot=<path>.");
        }

        string? Project = string.IsNullOrEmpty(ProjectRootOverride) ? null : PathUtils.Normalize(ProjectRootOverride);

        if (Project is not null && !System.IO.Directory.Exists(Project))
        {
            throw new BuildException($"Project root '{Project}' does not exist.");
        }

        return new BuildDirectories(Root, Project);
    }

    private static bool LooksLikeEngineRoot(string Candidate)
    {
        return System.IO.Directory.Exists(Path.Combine(Candidate, "Engine", "Source", "Runtime"));
    }

    private static string? SearchUpwardForEngineRoot(string StartDirectory)
    {
        DirectoryInfo? Current = new(StartDirectory);

        while (Current is not null)
        {
            if (LooksLikeEngineRoot(Current.FullName))
            {
                return Current.FullName;
            }

            Current = Current.Parent;
        }

        return null;
    }
}
