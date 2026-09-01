using System.Text;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>Folds a module's sources into a few blobs so shared headers are parsed once per blob.</summary>
public static class UnityBuildStep
{
    /// <summary>Subdirectory of a module's intermediates holding its generated blobs.</summary>
    public const string BlobDirectoryName = "Unity";

    private const string BlobExtension = ".unity.cpp";

    /// <summary>Resolves every module's compile inputs, generating unity blobs where enabled.</summary>
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

        // Sorted so blobs depend only on which files exist, not on directory-walk order.
        Mergeable.Sort((Left, Right) => string.Compare(Left.Location, Right.Location, StringComparison.OrdinalIgnoreCase));

        AdaptiveUnityState Adaptive = AdaptiveUnityState.Load(Module);

        if (Target.Rules.bAdaptiveUnityBuild && !Target.Info.Options.bDisableAdaptiveUnity)
        {
            Adaptive.Observe(Mergeable, Target.Rules.AdaptiveUnityMaxFiles);
        }
        else
        {
            Adaptive.Clear();
        }

        Adaptive.Save();

        if (Adaptive.WorkingSet.Count > 0)
        {
            Log.Verbose(
                "Adaptive unity: module '{0}' compiles {1} recently edited sources on their own.",
                Module.Name,
                Adaptive.WorkingSet.Count);
        }

        // Packed before the working set is applied, so holding a file out punches a hole in one blob
        // rather than shifting every file after it across a boundary and rewriting the lot.
        List<List<FileItem>> Groups = GroupByByteBudget(Mergeable, Math.Max(1024, Target.Rules.UnityBuildBytesPerFile));

        for (int Index = 0; Index < Groups.Count; Index++)
        {
            List<FileItem> Held = Groups[Index].Where(F => Adaptive.Contains(F.Location)).ToList();

            if (Held.Count == 0)
            {
                continue;
            }

            Groups[Index] = Groups[Index].Where(F => !Adaptive.Contains(F.Location)).ToList();

            foreach (FileItem Source in Held)
            {
                Log.Trace("Unity: '{0}' compiles alone (edited recently)", Source.Name);
                Standalone.Add(Source);
            }
        }

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

            // Declared, not left to the header scan: no dependency file exists on a first compile, and the blob's
            // timestamp only moves when its membership changes.
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

    /// <summary>Whether a source has to keep its own translation unit, and why.</summary>
    private static bool MustCompileAlone(BuildModule Module, FileItem Source, out string Reason)
    {
        // Merging would drop the flags and miscompile: the fiber scheduler needs /GT on exactly one file.
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

    /// <summary>Packs sources into groups of roughly equal source size.</summary>
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

    /// <summary>Deletes blobs a previous grouping left behind.</summary>
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
