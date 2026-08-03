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

/// <summary>
/// One external process invocation with declared inputs and outputs. The executor decides
/// whether to run it by comparing those inputs against those outputs.
/// </summary>
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

    /// <summary>
    /// Name the tool echoes back before its own diagnostics, stripped from captured output so a
    /// successful compile prints nothing.
    /// </summary>
    /// <remarks>
    /// Deliberately not StatusText. That is a display string free to carry extra detail -- a unity
    /// file adds how many sources it stands for -- and the moment it does, matching against it
    /// stops suppressing anything.
    /// </remarks>
    public string EchoedInputName { get; set; } = string.Empty;

    public string ToolPath { get; set; } = string.Empty;

    public string Arguments { get; set; } = string.Empty;

    public string WorkingDirectory { get; set; } = string.Empty;

    /// <summary>Files that must exist and be older than the outputs for the action to be skipped.</summary>
    public List<FileItem> PrerequisiteItems { get; } = new();

    /// <summary>
    /// Files the action always writes. The first is the primary output: its timestamp is the
    /// reference every input is compared against.
    /// </summary>
    public List<FileItem> ProducedItems { get; } = new();

    /// <summary>
    /// Files the action may or may not write. An import library is the motivating case: the
    /// linker omits it entirely for a binary with no exports, and leaves it untouched when the
    /// exports have not changed. They still register as this action's outputs so dependents
    /// order correctly, but they are never required to exist and never gate freshness.
    /// </summary>
    public List<FileItem> OptionalProducedItems { get; } = new();

    public IEnumerable<FileItem> AllProducedItems => ProducedItems.Concat(OptionalProducedItems);

    /// <summary>Explicit ordering edges for actions that do not communicate through files.</summary>
    public List<BuildAction> OrderDependencies { get; } = new();

    /// <summary>Response file written before the process starts, or null.</summary>
    public string? ResponseFilePath { get; set; }

    public string? ResponseFileContents { get; set; }

    /// <summary>Extra environment for the process, merged over the inherited environment.</summary>
    public Dictionary<string, string>? EnvironmentOverrides { get; set; }

    /// <summary>
    /// File the tool writes its discovered header dependencies to. Read back after a successful
    /// run and folded into the incremental cache.
    /// </summary>
    public string? DependencyListFile { get; set; }

    /// <summary>Actions that must not run concurrently with anything else, such as link steps under LTO.</summary>
    public bool bCanExecuteInParallel { get; set; } = true;

    /// <summary>
    /// Set when the action writes files it does not declare, which makes every cached stat in the
    /// process suspect once it has run.
    /// </summary>
    /// <remarks>
    /// The code generator is the case this exists for: it emits a .generated.h and .generated.cpp
    /// per reflected type plus the C# bindings, and which files those are is only known after it
    /// has run. Everything downstream reads those through the interned stat cache, which was filled
    /// while the build was being planned, so without dropping it the recheck that decides whether a
    /// compile is still necessary compares against the file as it looked before the generator
    /// rewrote it. Must stay paired with bCanExecuteInParallel = false: the invalidation is only
    /// safe while no other action is mid-flight.
    /// </remarks>
    public bool bWritesUndeclaredOutputs { get; set; }

    /// <summary>Treat a failure as a warning rather than a build failure.</summary>
    public bool bIgnoreExitCode { get; set; }

    /// <summary>
    /// Work performed in process rather than by launching a tool. Mutually exclusive with
    /// ToolPath: an action either runs a program or runs an operation.
    /// </summary>
    public BuildOperation? Operation { get; set; }

    /// <summary>
    /// Identity of the toolchain that produced this command. Folded into the command key so that
    /// upgrading the compiler or the platform SDK invalidates the outputs, which a command line
    /// alone would miss because the SDK reaches the compiler through the environment.
    /// </summary>
    public string ToolchainIdentity { get; set; } = string.Empty;

    /// <summary>
    /// Identity of the command being run. Any change forces a rerun even when timestamps say
    /// the outputs are current, which is what catches edits to Build.cs and Target.cs.
    /// </summary>
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
