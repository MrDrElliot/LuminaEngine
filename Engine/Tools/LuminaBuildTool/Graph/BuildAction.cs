using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

public enum ActionType
{
    /// <summary>Runs a code generator whose outputs other actions compile.</summary>
    Generate,

    Compile,

    /// <summary>Produces a static library.</summary>
    Archive,

    /// <summary>Produces a shared library or executable.</summary>
    Link,

    Copy,
}

public enum DependencyListFormat
{
    MsvcSourceDependencies,

    Makefile,
}

/// <summary>One external process invocation with declared inputs and outputs.</summary>
public sealed class BuildAction
{
    public BuildAction(ActionType Type, string ModuleName)
    {
        this.Type = Type;
        this.ModuleName = ModuleName;
    }

    public ActionType Type { get; }

    /// <summary>Module this action belongs to, used for grouping and log prefixes.</summary>
    public string ModuleName { get; }

    /// <summary>Short text shown while the action runs, usually a file name.</summary>
    public string StatusText { get; set; } = string.Empty;

    /// <summary>Name the tool echoes before its diagnostics, stripped so a successful compile prints nothing.</summary>
    public string EchoedInputName { get; set; } = string.Empty;

    public string ToolPath { get; set; } = string.Empty;

    public string Arguments { get; set; } = string.Empty;

    public string WorkingDirectory { get; set; } = string.Empty;

    /// <summary>Files that must exist and be older than the outputs for the action to be skipped.</summary>
    public List<FileItem> PrerequisiteItems { get; } = new();

    /// <summary>Files the action always writes.</summary>
    public List<FileItem> ProducedItems { get; } = new();

    /// <summary>Files the action may or may not write.</summary>
    public List<FileItem> OptionalProducedItems { get; } = new();

    public IEnumerable<FileItem> AllProducedItems => ProducedItems.Concat(OptionalProducedItems);

    /// <summary>Explicit ordering edges for actions that do not communicate through files.</summary>
    public List<BuildAction> OrderDependencies { get; } = new();

    /// <summary>Response file written before the process starts, or null.</summary>
    public string? ResponseFilePath { get; set; }

    public string? ResponseFileContents { get; set; }

    /// <summary>Extra environment for the process, merged over the inherited environment.</summary>
    public Dictionary<string, string>? EnvironmentOverrides { get; set; }

    /// <summary>File the tool writes its discovered header dependencies to.</summary>
    public string? DependencyListFile { get; set; }

    public DependencyListFormat DependencyListFormat { get; set; } = DependencyListFormat.MsvcSourceDependencies;

    /// <summary>Actions that must not run concurrently with anything else, such as link steps under LTO.</summary>
    public bool bCanExecuteInParallel { get; set; } = true;

    /// <summary>Set when the action writes undeclared files, making every cached stat suspect once it runs.</summary>
    public bool bWritesUndeclaredOutputs { get; set; }

    /// <summary>Treat a failure as a warning rather than a build failure.</summary>
    public bool bIgnoreExitCode { get; set; }

    public bool bDeleteOutputsBeforeRun { get; set; }

    /// <summary>Work performed in process rather than by launching a tool.</summary>
    public BuildOperation? Operation { get; set; }

    /// <summary>Identity of the toolchain that produced this command.</summary>
    public string ToolchainIdentity { get; set; } = string.Empty;

    /// <summary>Identity of the command being run.</summary>
    public string GetCommandKey()
    {
        return ContentHash.OfStrings(new[]
        {
            ToolPath,
            Arguments,
            ResponseFileContents ?? string.Empty,
            WorkingDirectory,
            ToolchainIdentity,
            Operation?.GetIdentity() ?? string.Empty,
        });
    }

    public override string ToString() => $"{Type} {StatusText}";
}
