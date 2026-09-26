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
            Adaptive.Observe(Mergeable, Target.Rules.AdaptiveUnityMaxFiles, SourceWorkingSet.For(Module.Rules.ModuleDirectory));
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

        // One blob per module puts every source in one translation unit, which is where a file-scope
        // name collision has to show up. Third party keeps its budget, since its collisions are not ours to fix.
        int BytesPerBlob = Target.Info.Options.bMaximalUnityBuild && !Module.Rules.bIsThirdParty
            ? int.MaxValue
            : Math.Max(1024, Target.Rules.UnityBuildBytesPerFile);

        List<List<FileItem>> Groups = GroupByStableBucket(Module, Mergeable, BytesPerBlob);

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

            // Declared rather than left to the header scan, since no dependency file exists on a first compile.
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
        // Merging would drop the flags and miscompile, since the fiber scheduler needs /GT on exactly one file.
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

    /// <summary>Packs sources into blobs by a hash of each path, so a file's blob does not depend on what sorts before it.</summary>
    private static List<List<FileItem>> GroupByStableBucket(BuildModule Module, IReadOnlyList<FileItem> Sources, int BytesPerBlob)
    {
        long TotalBytes = Sources.Sum(Source => Source.Length);

        // Aimed below the budget so a blob has room to absorb a new file without spilling one of its own elsewhere.
        long TargetBytes = Math.Max(1024, BytesPerBlob * 9 / 10);
        int BlobCount = (int)Math.Max(1, (TotalBytes + TargetBytes - 1) / TargetBytes);

        // An empty blob still parses the PCH, so a module of few large files never gets more blobs than sources.
        BlobCount = Math.Min(BlobCount, Sources.Count);

        List<List<FileItem>> Groups = new(BlobCount);
        long[] Bytes = new long[BlobCount];

        for (int Index = 0; Index < BlobCount; Index++)
        {
            Groups.Add(new List<FileItem>());
        }

        // Largest first, so a big file claims its preferred blob before the small ones fill it.
        List<FileItem> Ordered = Sources
            .OrderByDescending(Source => Source.Length)
            .ThenBy(Source => Source.Location, StringComparer.OrdinalIgnoreCase)
            .ToList();

        foreach (FileItem Source in Ordered)
        {
            int[] Preference = PreferenceFor(Module, Source, BlobCount);
            int Chosen = -1;

            foreach (int Bucket in Preference)
            {
                if (Groups[Bucket].Count == 0 || Bytes[Bucket] + Source.Length <= BytesPerBlob)
                {
                    Chosen = Bucket;
                    break;
                }
            }

            // Every blob is already full, so the lightest takes it and the budget is treated as a target.
            if (Chosen < 0)
            {
                Chosen = Array.IndexOf(Bytes, Bytes.Min());
            }

            Groups[Chosen].Add(Source);
            Bytes[Chosen] += Source.Length;
        }

        // Sorted inside the blob so its text depends only on which files landed there.
        foreach (List<FileItem> Group in Groups)
        {
            Group.Sort((Left, Right) => string.Compare(Left.Location, Right.Location, StringComparison.OrdinalIgnoreCase));
        }

        return Groups;
    }

    /// <summary>Blobs in descending preference for a source, so raising the blob count moves one blob's worth of files rather than all of them.</summary>
    private static int[] PreferenceFor(BuildModule Module, FileItem Source, int BlobCount)
    {
        ulong Seed = StableHash(BucketKeyFor(Module, Source));

        return Enumerable.Range(0, BlobCount)
            .OrderByDescending(Bucket => Scramble(Seed ^ ((ulong)(Bucket + 1) * 0x9E3779B97F4A7C15UL)))
            .ToArray();
    }

    /// <summary>Module-relative, lowercased and slash-normalized, so two checkouts of one commit bucket alike.</summary>
    private static string BucketKeyFor(BuildModule Module, FileItem Source)
    {
        string Key = Path.GetRelativePath(Module.Rules.ModuleDirectory, Source.Location);

        // A source reached from outside the module keeps its file name, which is still checkout independent.
        if (Key.StartsWith("..", StringComparison.Ordinal))
        {
            Key = Source.Name;
        }

        return Key.Replace('\\', '/').ToLowerInvariant();
    }

    // FNV-1a, because string.GetHashCode is randomized per process and would repack every build.
    private static ulong StableHash(string Text)
    {
        ulong Hash = 14695981039346656037UL;

        foreach (char Character in Text)
        {
            Hash = (Hash ^ Character) * 1099511628211UL;
        }

        return Hash;
    }

    private static ulong Scramble(ulong Value)
    {
        Value ^= Value >> 33;
        Value *= 0xFF51AFD7ED558CCDUL;
        Value ^= Value >> 33;
        Value *= 0xC4CEB9FE1A85EC53UL;
        return Value ^ (Value >> 33);
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
            // Forward slashes because the path sits inside a C++ string literal, where a separator would read as an escape.
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
