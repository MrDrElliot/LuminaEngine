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

    /// <summary>Where build records live, holding what each output was built from and which headers it included.</summary>
    public string BuildRecordDirectory(BuildPlatform Platform, BuildConfiguration Configuration, TargetType Type)
    {
        return Path.Combine(IntermediatesDirectory, "Obj", Platform.GetOutputDirectoryName(), $"{Type}-{Configuration}", ".buildtool");
    }

    public BuildDirectories ToEngineOnly()
    {
        return ProjectRoot is null ? this : Discover(EngineRoot, null);
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

        if (!PathUtils.IsUnder(ModuleDirectory, EngineRoot))
        {
            return false;
        }

        // Inside both roots the nearer one owns it, which keeps an engine submodule's code with the engine.
        return !PathUtils.IsUnder(ModuleDirectory, ProjectRoot) || PathUtils.IsUnder(EngineRoot, ProjectRoot);
    }

    public bool IsProjectOwned(string Location)
    {
        return ProjectRoot is not null && PathUtils.IsUnder(Location, ProjectRoot) && !IsEngineOwned(Location);
    }

    public string EnginePath(string RelativePath) => PathUtils.Combine(EngineRoot, RelativePath);

    public string ThirdPartyPath(string RelativePath) => PathUtils.Combine(ThirdPartyDirectory, RelativePath);

    /// <summary>Locates the engine root, an explicit override, then a walk up from this tool or the working directory.</summary>
    public static BuildDirectories Discover(string? EngineRootOverride, string? ProjectRootOverride)
    {
        string? Root = EngineRootOverride;

        if (string.IsNullOrEmpty(Root))
        {
            Root = SearchUpwardForEngineRoot(AppContext.BaseDirectory)
                ?? SearchUpwardForEngineRoot(System.IO.Directory.GetCurrentDirectory());
        }

        if (string.IsNullOrEmpty(Root))
        {
            throw new BuildException(
                "Could not locate the Lumina engine root. Pass -EngineRoot=<path> or run the tool from inside an engine tree.");
        }

        Root = PathUtils.Normalize(Root);

        if (!LooksLikeEngineRoot(Root))
        {
            throw new BuildException(
                $"'{Root}' does not contain Engine/Source/Runtime, so it is not an engine root. "
                + "Pass -EngineRoot=<path>.");
        }

        string? Project = string.IsNullOrEmpty(ProjectRootOverride) ? null : PathUtils.Normalize(ProjectRootOverride);

        // A project's .lproject names it as well as its directory does, which is what Explorer hands a verb.
        if (Project is not null && File.Exists(Project) && Project.EndsWith(".lproject", StringComparison.OrdinalIgnoreCase))
        {
            Project = PathUtils.Normalize(Path.GetDirectoryName(Project)!);
        }

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
