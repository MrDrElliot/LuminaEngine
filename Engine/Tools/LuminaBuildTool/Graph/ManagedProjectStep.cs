using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>Builds the target's .NET projects through the dotnet SDK; the engine loads them at run time.</summary>
public static class ManagedProjectStep
{
    private static readonly string[] IgnoredDirectories = { "obj", "bin", ".vs", ".idea" };

    public static IEnumerable<BuildAction> CreateActions(BuildTarget Target, BuildAction? GenerateReflection)
    {
        foreach (ManagedProject Project in Target.Rules.ManagedProjects)
        {
            if (!File.Exists(Project.ProjectFile))
            {
                throw new BuildException($"Managed project '{Project.ProjectFile}' does not exist.");
            }

            BuildAction Action = new(ActionType.Generate, "Managed")
            {
                StatusText = Path.GetFileNameWithoutExtension(Project.ProjectFile),
                ToolPath = ResolveDotNet(),
                WorkingDirectory = Path.GetDirectoryName(Project.ProjectFile)!,
                Arguments = string.Join(' ', new[]
                {
                    "build",
                    PathUtils.Quote(Project.ProjectFile),
                    "--configuration",
                    Target.Info.Configuration.ToString(),
                    "--nologo",
                    "--verbosity",
                    "quiet",

                    // The build owns the output location; the project file repeats it only so it opens standalone.
                    PathUtils.Quote($"-p:OutputPath={Path.GetDirectoryName(Project.OutputAssembly)}{Path.DirectorySeparatorChar}"),
                }),

                // MSBuild already parallelizes internally and holds a NuGet lock; two concurrent
                // restores of the same project deadlock more often than they help.
                bCanExecuteInParallel = false,

                // The C# compiler reads LIB and warns per bogus entry (CS1668), and a Developer Command Prompt seeds
                // it with ATL/MFC paths that may not exist. Dropped so this build inherits no opinion from the shell.
                EnvironmentOverrides = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
                {
                    ["LIB"] = string.Empty,
                    ["INCLUDE"] = string.Empty,
                },
            };

            foreach (FileItem Source in EnumerateProjectAndReferences(Project.ProjectFile))
            {
                Action.PrerequisiteItems.Add(Source);
            }

            // The Reflector emits this assembly's bindings, so they are real compile inputs.
            if (GenerateReflection is not null)
            {
                Action.OrderDependencies.Add(GenerateReflection);

                foreach (FileItem Binding in EnumerateGeneratedBindings(Target, Project))
                {
                    Action.PrerequisiteItems.Add(Binding);
                }
            }

            Action.ProducedItems.Add(FileItem.Get(Project.OutputAssembly));

            yield return Action;
        }
    }

    /// <summary>A project's own file and sources, plus those of every project it references, transitively.</summary>
    private static IEnumerable<FileItem> EnumerateProjectAndReferences(string ProjectFile)
    {
        HashSet<string> Visited = new(StringComparer.OrdinalIgnoreCase);
        Queue<string> Pending = new();
        Pending.Enqueue(Path.GetFullPath(ProjectFile));

        while (Pending.Count > 0)
        {
            string Current = Pending.Dequeue();
            if (!Visited.Add(Current) || !File.Exists(Current))
            {
                continue;
            }

            yield return FileItem.Get(Current);

            string Directory = Path.GetDirectoryName(Current)!;
            foreach (FileItem Source in EnumerateProjectSources(Directory))
            {
                yield return Source;
            }

            foreach (string Reference in EnumerateProjectReferences(Current, Directory))
            {
                Pending.Enqueue(Reference);
            }
        }
    }

    /// <summary>The ProjectReference paths a .csproj declares, resolved against it.</summary>
    private static IEnumerable<string> EnumerateProjectReferences(string ProjectFile, string ProjectDirectory)
    {
        string Text;
        try
        {
            Text = File.ReadAllText(ProjectFile);
        }
        catch (IOException)
        {
            yield break;
        }

        const string Marker = "<ProjectReference";
        int Cursor = 0;
        while ((Cursor = Text.IndexOf(Marker, Cursor, StringComparison.OrdinalIgnoreCase)) >= 0)
        {
            Cursor += Marker.Length;

            int Include = Text.IndexOf("Include=\"", Cursor, StringComparison.OrdinalIgnoreCase);
            int ElementEnd = Text.IndexOf('>', Cursor);
            if (Include < 0 || (ElementEnd >= 0 && Include > ElementEnd))
            {
                continue;
            }

            Include += "Include=\"".Length;
            int Close = Text.IndexOf('"', Include);
            if (Close < 0)
            {
                yield break;
            }

            string Relative = Text.Substring(Include, Close - Include);
            if (!string.IsNullOrWhiteSpace(Relative))
            {
                yield return Path.GetFullPath(Path.Combine(ProjectDirectory, Relative));
            }
            Cursor = Close;
        }
    }

    private static IEnumerable<FileItem> EnumerateProjectSources(string ProjectDirectory)
    {
        foreach (string Source in Directory.EnumerateFiles(ProjectDirectory, "*.cs", SearchOption.AllDirectories))
        {
            bool bIgnored = false;

            foreach (string Segment in PathUtils.MakeRelativeTo(Source, ProjectDirectory)
                .Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar))
            {
                if (IgnoredDirectories.Contains(Segment, StringComparer.OrdinalIgnoreCase))
                {
                    bIgnored = true;
                    break;
                }
            }

            if (!bIgnored)
            {
                yield return FileItem.Get(Source);
            }
        }
    }

    /// <summary>The generated bindings this assembly compiles.</summary>
    private static IEnumerable<FileItem> EnumerateGeneratedBindings(BuildTarget Target, ManagedProject Project)
    {
        string BindingsDirectory = Path.Combine(
            Target.Directories.GetOutputRootFor(Path.GetDirectoryName(Project.ProjectFile)!),
            "Intermediates",
            "CSharpBindings");

        if (!Directory.Exists(BindingsDirectory))
        {
            yield break;
        }

        foreach (string Binding in Directory.EnumerateFiles(BindingsDirectory, "*.generated.cs", SearchOption.AllDirectories))
        {
            yield return FileItem.Get(Binding);
        }
    }

    private static string ResolveDotNet()
    {
        // The tool itself runs on the SDK, so the host executable is the one to reuse.
        string? HostPath = Environment.ProcessPath;

        if (HostPath is not null && Path.GetFileNameWithoutExtension(HostPath).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
        {
            return HostPath;
        }

        return OperatingSystem.IsWindows() ? "dotnet.exe" : "dotnet";
    }
}
