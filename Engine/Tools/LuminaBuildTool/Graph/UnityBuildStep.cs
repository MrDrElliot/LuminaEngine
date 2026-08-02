using System.Text;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>
/// Folds a module's translation units into a few generated blobs, each of which includes the
/// sources it stands for, so shared headers are parsed once per blob instead of once per file.
/// </summary>
/// <remarks>
/// What a unity build costs is isolation. Merged sources stop being independent translation units,
/// so a file-scope static, an anonymous namespace, a using-directive or a macro left defined now
/// reaches its neighbours, and a name that was private to one file can collide with another's.
/// That is a real change in what the language guarantees, which is why this is opt in per target
/// and per module rather than a global default.
///
/// Anything that cannot be merged is kept out automatically rather than by remembering to list it:
/// a file with its own compiler options would silently lose them, a precompiled header source has
/// to stay its own translation unit, and the reflection shards are already blobs. Getting those
/// wrong is not a compile error, it is a miscompile, so the decision does not belong to whoever
/// edits a Build.cs next.
/// </remarks>
public static class UnityBuildStep
{
    /// <summary>Subdirectory of a module's intermediates holding its generated blobs.</summary>
    public const string BlobDirectoryName = "Unity";

    private const string BlobExtension = ".unity.cpp";

    /// <summary>
    /// Resolves every module's compile inputs, generating unity blobs where enabled. Runs on the
    /// build path only: project generation lists real source files, never blobs.
    /// </summary>
    public static void Prepare(BuildTarget Target)
    {
        foreach (BuildModule Module in Target.Modules)
        {
            Prepare(Target, Module);
        }
    }

    private static void Prepare(BuildTarget Target, BuildModule Module)
    {
        if (!IsEnabledFor(Target, Module) || !Module.BinaryType.ProducesCompiledOutput())
        {
            return;
        }

        List<FileItem> Mergeable = new();
        List<FileItem> Standalone = new();

        foreach (FileItem Source in Module.Sources.CppFiles)
        {
            if (MustCompileAlone(Module, Source, out string Reason))
            {
                Log.Trace("Unity: '{0}' compiles alone ({1})", Source.Name, Reason);
                Standalone.Add(Source);
            }
            else
            {
                Mergeable.Add(Source);
            }
        }

        int Minimum = Math.Max(2, Target.Rules.MinFilesForUnityBuild);

        if (Mergeable.Count < Minimum)
        {
            Log.Verbose(
                "Unity: module '{0}' has {1} mergeable sources, below the minimum of {2}; compiling file by file.",
                Module.Name,
                Mergeable.Count,
                Minimum);

            return;
        }

        // Sorted so the grouping depends only on which files exist, not on the order the
        // directory walk returned them. Two machines building the same tree produce the same
        // blobs, and an unchanged module keeps the object files it already has.
        Mergeable.Sort((Left, Right) => string.Compare(Left.Location, Right.Location, StringComparison.OrdinalIgnoreCase));

        List<List<FileItem>> Groups = GroupByByteBudget(Mergeable, Math.Max(1024, Target.Rules.UnityBuildBytesPerFile));
        string BlobDirectory = Path.Combine(Module.IntermediateDirectory, BlobDirectoryName);

        Directory.CreateDirectory(BlobDirectory);

        List<FileItem> Blobs = new();
        int Written = 0;

        for (int Index = 0; Index < Groups.Count; Index++)
        {
            string BlobPath = Path.Combine(BlobDirectory, $"{Module.Name}.{Index}{BlobExtension}");

            if (PathUtils.WriteFileIfChanged(BlobPath, BuildBlobContents(Module, Groups[Index])))
            {
                Written++;
            }

            FileItem Blob = FileItem.Get(BlobPath);

            // Declared rather than left to the header scan: on a first compile no dependency file
            // exists yet, and the blob's own timestamp only moves when the membership changes, so
            // without this an edit to a member would not rebuild the blob that contains it.
            Module.SubsumedSourceFiles[Blob.Location] = Groups[Index];
            Blobs.Add(Blob);
        }

        RemoveStaleBlobs(BlobDirectory, Blobs);

        Module.CppCompileInputs.Clear();
        Module.CppCompileInputs.AddRange(Blobs);
        Module.CppCompileInputs.AddRange(Standalone);
        Module.CppCompileInputs.AddRange(Module.GeneratedSourceFiles);

        Log.Verbose(
            "Unity: module '{0}' folded {1} sources into {2} blobs ({3} rewritten), {4} compiling alone.",
            Module.Name,
            Mergeable.Count,
            Blobs.Count,
            Written,
            Standalone.Count);
    }

    private static bool IsEnabledFor(BuildTarget Target, BuildModule Module)
    {
        if (Target.Info.Options.bDisableUnityBuild)
        {
            return false;
        }

        return Module.Rules.bUseUnityBuild ?? Target.Rules.bUseUnityBuild;
    }

    /// <summary>
    /// Whether a source has to keep its own translation unit, and why. The reason is returned so
    /// a verbose build can show it: "my file was not in a blob" is otherwise invisible.
    /// </summary>
    private static bool MustCompileAlone(BuildModule Module, FileItem Source, out string Reason)
    {
        // Merging would drop the flags, and the failure would be a miscompile rather than an
        // error: the engine's fiber scheduler needs /GT on exactly one file to read thread-local
        // state correctly.
        if (Module.Rules.PerFileCompilerOptions.ContainsKey(Source.Name))
        {
            Reason = "has per-file compiler options";
            return true;
        }

        // /Yc has to run in its own translation unit; it is what produces the PCH the others use.
        if (Module.Rules.PrecompiledHeader is not null
            && PathUtils.AreSame(Source.Location, Module.Rules.ModulePath(Module.Rules.PrecompiledHeader.Source)))
        {
            Reason = "is the precompiled header source";
            return true;
        }

        // Compiled once per loaded image and deliberately outside the module's own tree.
        if (Module.Rules.PerImageSourceFiles.Any(P => PathUtils.AreSame(Source.Location, Module.Rules.ModulePath(P))))
        {
            Reason = "is a per-image source";
            return true;
        }

        if (Module.Rules.ExcludeFromUnity.Contains(Source.Name, StringComparer.OrdinalIgnoreCase))
        {
            Reason = "is listed in ExcludeFromUnity";
            return true;
        }

        Reason = string.Empty;
        return false;
    }

    /// <summary>
    /// Packs sources into groups of roughly equal source size. Size rather than file count because
    /// a module's files vary by orders of magnitude, and it is bytes of preprocessed input that
    /// decide how long a blob takes and how much memory it needs.
    /// </summary>
    private static List<List<FileItem>> GroupByByteBudget(IReadOnlyList<FileItem> Sources, int BytesPerBlob)
    {
        List<List<FileItem>> Groups = new();
        List<FileItem> Current = new();
        long CurrentBytes = 0;

        foreach (FileItem Source in Sources)
        {
            // A file larger than the whole budget still lands somewhere; it simply gets a blob of
            // its own rather than being split, which is not something a unity build can do.
            if (Current.Count > 0 && CurrentBytes + Source.Length > BytesPerBlob)
            {
                Groups.Add(Current);
                Current = new List<FileItem>();
                CurrentBytes = 0;
            }

            Current.Add(Source);
            CurrentBytes += Source.Length;
        }

        if (Current.Count > 0)
        {
            Groups.Add(Current);
        }

        return Groups;
    }

    private static string BuildBlobContents(BuildModule Module, IReadOnlyList<FileItem> Members)
    {
        StringBuilder Text = new();

        Text.AppendLine($"// Generated by LuminaBuildTool for module '{Module.Name}'. Edits are overwritten.");
        Text.AppendLine("//");
        Text.AppendLine("// These translation units are compiled together, so they share file-scope names, macros");
        Text.AppendLine("// and using-directives. To give one of them its own translation unit again, add its file");
        Text.AppendLine($"// name to ExcludeFromUnity in {Module.Name}.Build.cs. To stop merging this module at");
        Text.AppendLine("// all, set bUseUnityBuild = false there. To rule it out as the cause of a build failure,");
        Text.AppendLine("// build once with -NoUnity.");
        Text.AppendLine();

        foreach (FileItem Member in Members)
        {
            // Forward slashes because the path sits inside a C++ string literal, where a Windows
            // separator would read as an escape sequence.
            Text.AppendLine($"#include \"{Member.Location.Replace('\\', '/')}\"");
        }

        return Text.ToString();
    }

    /// <summary>
    /// Deletes blobs a previous grouping left behind. Nothing links them, because the link step
    /// takes its objects from the compile actions, but leaving them makes the intermediate
    /// directory misreport what the module is built from.
    /// </summary>
    private static void RemoveStaleBlobs(string BlobDirectory, IReadOnlyList<FileItem> Current)
    {
        HashSet<string> Live = new(Current.Select(F => F.Location), StringComparer.OrdinalIgnoreCase);

        foreach (string Existing in Directory.EnumerateFiles(BlobDirectory, "*" + BlobExtension))
        {
            if (Live.Contains(PathUtils.Normalize(Existing)))
            {
                continue;
            }

            try
            {
                File.Delete(Existing);
                Log.Trace("Unity: removed stale blob '{0}'", Existing);
            }
            catch (IOException Ex)
            {
                Log.Verbose("Unity: could not remove stale blob '{0}': {1}", Existing, Ex.Message);
            }
        }
    }
}
