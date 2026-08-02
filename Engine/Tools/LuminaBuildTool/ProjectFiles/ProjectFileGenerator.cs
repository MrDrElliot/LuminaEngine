using LuminaBuildTool.Configuration;
using LuminaBuildTool.Graph;

namespace LuminaBuildTool.ProjectFiles;

/// <summary>
/// One (configuration, target type) pair the generated projects expose to the IDE.
/// </summary>
public sealed record ProjectConfiguration(BuildConfiguration Configuration, TargetType Type)
{
    /// <summary>Name shown in the IDE's configuration dropdown, for example "Development Editor".</summary>
    public string DisplayName => $"{Configuration} {Type}";
}

/// <summary>
/// A target resolved for every configuration the IDE should offer.
/// </summary>
public sealed class ProjectTargetInfo
{
    public required string TargetName { get; init; }

    /// <summary>Resolved graph per configuration, used for source lists and IntelliSense settings.</summary>
    public required Dictionary<ProjectConfiguration, BuildTarget> Variants { get; init; }

    /// <summary>The variant used for source listing and IntelliSense.</summary>
    public required BuildTarget PrimaryVariant { get; init; }
}

/// <summary>
/// Writes IDE project files from resolved build graphs. Implementations decide the file format;
/// the driver decides what gets generated.
/// </summary>
public interface IProjectFileGenerator
{
    string Name { get; }

    /// <summary>
    /// Writes projects and a solution. Returns the number of files that actually changed on disk.
    /// </summary>
    /// <param name="RulesProjectPath">
    /// C# project holding the rules files, included in the solution so the IDE can resolve them.
    /// Empty when none was generated.
    /// </param>
    int Generate(
        BuildDirectories Directories,
        IReadOnlyList<ProjectTargetInfo> Targets,
        IReadOnlyList<ProjectConfiguration> Configurations,
        string RulesProjectPath);
}
