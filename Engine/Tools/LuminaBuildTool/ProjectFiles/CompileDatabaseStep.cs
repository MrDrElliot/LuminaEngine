using System.Text.Json;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Toolchain;

namespace LuminaBuildTool.ProjectFiles;

/// <summary>
/// Writes a compile_commands.json describing how every source in the workspace is compiled.
/// </summary>
/// <remarks>
/// The format is what clangd, clang-tidy and every editor that is not Visual Studio read to
/// understand a C++ project. Without one they guess, and a guess here means no include paths, no
/// definitions and no precompiled header, which reduces a large codebase to a wall of
/// false errors about headers that are not missing.
///
/// The commands come from the same toolchain call the build uses, so what a tool sees is what the
/// compiler sees, rather than a second description that has to be kept in step.
/// </remarks>
public static class CompileDatabaseStep
{
    private sealed class CompileCommand
    {
        public required string Directory { get; init; }

        public required string File { get; init; }

        public required string Command { get; init; }

        public required string Output { get; init; }
    }

    /// <summary>
    /// Writes the database for the given targets at the workspace root, where clangd looks for it.
    /// </summary>
    /// <remarks>
    /// Targets share modules, so the same source can be compiled several ways in one workspace.
    /// The first description of a file wins and the rest are dropped: a tool can only hold one
    /// answer per file, and every consumer of this format resolves the ambiguity that way anyway.
    /// </remarks>
    public static bool Write(
        BuildDirectories Directories,
        IReadOnlyList<ProjectTargetInfo> Targets,
        IReadOnlyList<ProjectConfiguration> Configurations,
        IToolchain Toolchain)
    {
        // The configuration developers actually read code in. Editor rather than Game because it is
        // the superset: a Game target cannot describe the editor-only modules at all.
        ProjectConfiguration? Preferred = Configurations
            .FirstOrDefault(C => C.Type == TargetType.Editor && C.Configuration == BuildConfiguration.Development);

        Dictionary<string, CompileCommand> ByFile = new(StringComparer.OrdinalIgnoreCase);

        foreach (ProjectTargetInfo Target in Targets)
        {
            BuildTarget? Variant = Preferred is not null && Target.Variants.TryGetValue(Preferred, out BuildTarget? Match)
                ? Match
                : Target.PrimaryVariant;

            if (Variant is null)
            {
                continue;
            }

            foreach (BuildModule Module in Variant.Modules)
            {
                AddModule(Directories, Variant, Module, Toolchain, ByFile);
            }
        }

        string DatabasePath = Path.Combine(Directories.OutputRoot, "compile_commands.json");

        string Json = JsonSerializer.Serialize(
            ByFile.Values.OrderBy(C => C.File, StringComparer.OrdinalIgnoreCase).ToList(),
            new JsonSerializerOptions
            {
                WriteIndented = true,
                PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            });

        bool bChanged = PathUtils.WriteFileIfChanged(DatabasePath, Json, bByteOrderMark: false);

        Log.Info("Compile database: {0} ({1} files{2})", DatabasePath, ByFile.Count, bChanged ? string.Empty : ", unchanged");

        return bChanged;
    }

    private static void AddModule(
        BuildDirectories Directories,
        BuildTarget Target,
        BuildModule Module,
        IToolchain Toolchain,
        Dictionary<string, CompileCommand> ByFile)
    {
        // Project generation never runs the unity step, so these are the real per-file commands.
        // A blob's command line does not mention the sources it absorbed, which is exactly what a
        // per-file tool needs and cannot get from a unity build.
        List<BuildAction> Actions;

        try
        {
            Actions = Toolchain.CreateCompileActions(Target, Module);
        }
        catch (BuildException Ex)
        {
            Log.Verbose("No compile commands for '{0}': {1}", Module.Name, Ex.Message);
            return;
        }

        foreach (BuildAction Action in Actions)
        {
            if (Action.Type != ActionType.Compile
                || Action.ResponseFileContents is null
                || Action.PrerequisiteItems.Count == 0)
            {
                continue;
            }

            string SourceFile = Action.PrerequisiteItems[0].Location;

            if (ByFile.ContainsKey(SourceFile))
            {
                continue;
            }

            IEnumerable<string> Arguments = Action.ResponseFileContents
                .Split('\n')
                .Select(Line => Line.Trim('\r', ' '))
                .Where(Line => Line.Length > 0);

            ByFile[SourceFile] = new CompileCommand
            {
                // Absolute throughout, so the directory only has to exist. The module's
                // intermediates may not yet, on a workspace that has never been built.
                Directory = Directories.OutputRoot,
                File = SourceFile,
                Command = PathUtils.Quote(Action.ToolPath) + " " + string.Join(' ', Translate(Arguments)),
                Output = Action.ProducedItems.Count > 0 ? Action.ProducedItems[0].Location : string.Empty,
            };
        }
    }

    /// <summary>
    /// Adjusts the compiler's own arguments for a tool that is not that compiler.
    /// </summary>
    /// <remarks>
    /// Two rewrites, both about precompiled headers. Clang cannot read an MSVC .pch, so /Yu and
    /// /Fp pointing at one are useless at best and a hard error at worst. Dropping them alone would
    /// lose what the PCH was providing, because a source that relies on it never includes those
    /// headers itself, so /Yu becomes a forced include of the same header: the same declarations
    /// arrive, by the slower route that does not need a binary the tool cannot parse.
    ///
    /// /sourceDependencies goes because it asks for a build artifact, and reading code is not a
    /// build.
    /// </remarks>
    private static IEnumerable<string> Translate(IEnumerable<string> Arguments)
    {
        // A module force-includes its own PCH header already, so translating /Yu would name it a
        // second time. Harmless to the compiler, confusing to read.
        HashSet<string> ForcedIncludes = new(StringComparer.OrdinalIgnoreCase);

        foreach (string Argument in Arguments)
        {
            if (Argument.StartsWith("/sourceDependencies", StringComparison.Ordinal)
                || Argument.StartsWith("/Fp", StringComparison.Ordinal))
            {
                continue;
            }

            string Translated = Argument.StartsWith("/Yc", StringComparison.Ordinal)
                || Argument.StartsWith("/Yu", StringComparison.Ordinal)
                    ? "/FI" + Argument[3..]
                    : Argument;

            if (Translated.StartsWith("/FI", StringComparison.Ordinal) && !ForcedIncludes.Add(Translated))
            {
                continue;
            }

            yield return Translated;
        }
    }
}
