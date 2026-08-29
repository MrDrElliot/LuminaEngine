using LuminaBuildTool.Graph;

namespace LuminaBuildTool.Toolchain;

/// <summary>Turns resolved modules into the process invocations that compile and link them.</summary>
public interface IToolchain
{
    /// <summary>Short identifier, for example "MSVC".</summary>
    string Name { get; }

    /// <summary>Human readable version summary shown once per build.</summary>
    string Description { get; }

    /// <summary>Stable identity of the toolchain installation, stamped onto every action it produces.</summary>
    string VersionKey { get; }

    /// <summary>Toolset name generated IDE projects declare.</summary>
    string ProjectToolsetName { get; }

    /// <summary>Produces the compile actions for one module.</summary>
    List<BuildAction> CreateCompileActions(BuildTarget Target, BuildModule Module);

    /// <summary>Archive or link action for one module, or null when it has no linked output.</summary>
    BuildAction? CreateLinkAction(BuildTarget Target, BuildModule Module, IReadOnlyList<BuildAction> CompileActions);

    /// <summary>Files an instrumented PGO binary needs beside it to start. Empty when Pgo is off.</summary>
    IEnumerable<string> GetProfileRuntimeFiles(BuildTarget Target) => Array.Empty<string>();
}
