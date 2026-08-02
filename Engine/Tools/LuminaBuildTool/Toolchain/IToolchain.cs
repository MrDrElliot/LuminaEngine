using LuminaBuildTool.Graph;

namespace LuminaBuildTool.Toolchain;

/// <summary>
/// Turns resolved modules into the process invocations that compile and link them.
/// Implementations own every platform-specific command-line detail.
/// </summary>
public interface IToolchain
{
    /// <summary>Short identifier, for example "MSVC".</summary>
    string Name { get; }

    /// <summary>Human readable version summary shown once per build.</summary>
    string Description { get; }

    /// <summary>
    /// Stable identity of the toolchain installation, stamped onto every action it produces.
    /// </summary>
    /// <remarks>
    /// Compiler and platform SDK versions do not all appear on a command line. MSVC reaches its
    /// SDK headers and libraries through the environment, so an SDK upgrade would otherwise leave
    /// every output looking current while it was built against different headers.
    /// </remarks>
    string VersionKey { get; }

    /// <summary>
    /// Toolset name generated IDE projects declare. Affects only how the IDE loads the project;
    /// the build itself never goes through it.
    /// </summary>
    string ProjectToolsetName { get; }

    /// <summary>
    /// Produces the compile actions for one module. Returns an empty list for modules that
    /// compile nothing.
    /// </summary>
    List<BuildAction> CreateCompileActions(BuildTarget Target, BuildModule Module);

    /// <summary>
    /// Produces the archive or link action for one module, or null when the module has no
    /// linked output.
    /// </summary>
    BuildAction? CreateLinkAction(BuildTarget Target, BuildModule Module, IReadOnlyList<BuildAction> CompileActions);
}
