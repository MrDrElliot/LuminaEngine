using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>
/// Source and header files discovered for one module.
/// </summary>
public sealed class ModuleSourceSet
{
    public List<FileItem> CppFiles { get; } = new();

    public List<FileItem> CFiles { get; } = new();

    public List<FileItem> HeaderFiles { get; } = new();

    /// <summary>Files shown in generated projects but never compiled.</summary>
    public List<FileItem> ResourceFiles { get; } = new();

    public IEnumerable<FileItem> CompilableFiles => CppFiles.Concat(CFiles);

    public IEnumerable<FileItem> AllFiles => CppFiles.Concat(CFiles).Concat(HeaderFiles).Concat(ResourceFiles);
}

/// <summary>
/// Walks a module's source directories. A subdirectory that declares its own Build.cs belongs to
/// another module and is skipped, so nesting modules never double-compiles their sources.
/// </summary>
public static class SourceFileScanner
{
    private static readonly HashSet<string> HeaderExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".h", ".hpp", ".hh", ".inl", ".ipp",
    };

    private static readonly HashSet<string> ResourceExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".slang", ".hlsl", ".rc", ".natvis",
    };

    private static readonly string[] AlwaysIgnoredDirectories =
    {
        "Intermediates", "Binaries", "Saved", "obj", "bin", ".git", ".vs",
    };

    /// <param name="bFormsOwnImage">
    /// True when this module links into its own shared library or executable. Decides whether its
    /// per-image sources are compiled here or left to whichever image absorbs the module.
    /// </param>
    public static ModuleSourceSet Scan(ModuleRules Rules, bool bFormsOwnImage)
    {
        ModuleSourceSet Result = new();
        HashSet<string> Seen = new(StringComparer.OrdinalIgnoreCase);

        // A per-image source may physically live inside the module's own tree. Reserving its path
        // up front keeps the per-image list the single way it can enter the compile set, so the
        // directory walk cannot smuggle in a second copy when the module is not an image.
        List<string> PerImageSources = Rules.PerImageSourceFiles.Select(Rules.ModulePath).ToList();

        foreach (string PerImageSource in PerImageSources)
        {
            Seen.Add(PerImageSource);
        }

        if (!Rules.bUseExplicitSourceList)
        {
            foreach (string Root in ResolveSourceRoots(Rules))
            {
                if (!Directory.Exists(Root))
                {
                    Log.Verbose("Module '{0}' source directory '{1}' does not exist", Rules.Name, Root);
                    continue;
                }

                ScanDirectory(Root, Rules, Result, Seen);
            }
        }
        else
        {
            // Headers are still discovered so reflection and the IDE see the whole tree; only
            // compilation is restricted to the explicit list.
            CollectHeadersOnly(Rules.ModuleDirectory, Rules, Result, Seen);
        }

        foreach (string Extra in Rules.ExtraSourceFiles)
        {
            AddExplicitSource(Rules.ModulePath(Extra), Rules, Result, Seen);
        }

        // A module that must compile something but contributed nothing of its own has a source root
        // pointing somewhere it does not live. That used to produce an image built from its
        // per-image sources alone, which links and installs and then fails at run time with no
        // module registration in it. Checked before per-image sources are added so those cannot
        // mask an empty module. Header-only modules are exempt: they exist to carry include paths
        // and prebuilt libraries, and their headers may not live in the module tree at all.
        if (Rules.BinaryType != ModuleBinaryType.HeaderOnly && !Result.AllFiles.Any())
        {
            throw new BuildException(
                $"Module '{Rules.Name}' has no source or header files. Looked in "
                + $"'{Rules.ResolveSourceRoot()}'. An engine module keeps its sources in a Source "
                + "subdirectory beside its Build.cs; a plugin module keeps them in the same "
                + "directory as its Build.cs. Set SourceDirectories to override.");
        }

        ValidateFileScopedRules(Rules, Result, PerImageSources);

        if (bFormsOwnImage)
        {
            foreach (string PerImageSource in PerImageSources)
            {
                // Reserved above, so release it before adding.
                Seen.Remove(PerImageSource);
                AddExplicitSource(PerImageSource, Rules, Result, Seen);
            }
        }

        return Result;
    }

    /// <summary>
    /// Fails a build whose rules name a source file the module does not have.
    /// </summary>
    /// <remarks>
    /// Both settings are keyed by bare file name, so renaming or moving a file silently drops what
    /// was attached to it. The module still compiles, which is exactly the problem: what it loses
    /// is a flag it asked for. JobScheduler.cpp quietly losing /GT is a fiber-scheduler crash, not
    /// a diagnostic, and StbImageImpl.cpp quietly rejoining a unity file is a duplicate-symbol link
    /// error a long way from its cause. Neither should be discoverable only at run time.
    /// </remarks>
    private static void ValidateFileScopedRules(
        ModuleRules Rules, ModuleSourceSet Result, IReadOnlyList<string> PerImageSources)
    {
        HashSet<string> Known = new(Result.AllFiles.Select(F => F.Name), StringComparer.OrdinalIgnoreCase);

        // Included whether or not this module compiles them: whether it forms its own image is a
        // property of the link layout, not a statement about which files the rules may name.
        foreach (string PerImageSource in PerImageSources)
        {
            Known.Add(Path.GetFileName(PerImageSource));
        }

        void Require(string FileName, string Setting)
        {
            if (!Known.Contains(FileName))
            {
                throw new BuildException(
                    $"Module '{Rules.Name}' lists '{FileName}' in {Setting}, but has no such source file. "
                    + "The name is matched exactly; update it to match the file, or drop the entry if it "
                    + "is no longer needed.");
            }
        }

        foreach (string FileName in Rules.PerFileCompilerOptions.Keys)
        {
            Require(FileName, nameof(Rules.PerFileCompilerOptions));
        }

        foreach (string FileName in Rules.ExcludeFromUnity)
        {
            Require(FileName, nameof(Rules.ExcludeFromUnity));
        }
    }

    private static void AddExplicitSource(string Absolute, ModuleRules Rules, ModuleSourceSet Result, HashSet<string> Seen)
    {
        if (File.Exists(Absolute))
        {
            Classify(FileItem.Get(Absolute), Rules, Result, Seen);
        }
        else
        {
            Log.Warning("Module '{0}' lists missing source file '{1}'", Rules.Name, Absolute);
        }
    }

    /// <summary>
    /// Walks a module tree recording only headers and resources, for modules whose compile set is
    /// an explicit list.
    /// </summary>
    private static void CollectHeadersOnly(string Directory, ModuleRules Rules, ModuleSourceSet Result, HashSet<string> Seen)
    {
        if (!System.IO.Directory.Exists(Directory))
        {
            return;
        }

        foreach (string FilePath in System.IO.Directory.EnumerateFiles(Directory))
        {
            FileItem Item = FileItem.Get(FilePath);

            if (HeaderExtensions.Contains(Item.Extension) || ResourceExtensions.Contains(Item.Extension))
            {
                Classify(Item, Rules, Result, Seen);
            }
        }

        foreach (string SubDirectory in System.IO.Directory.EnumerateDirectories(Directory))
        {
            string Name = Path.GetFileName(SubDirectory);

            if (AlwaysIgnoredDirectories.Contains(Name, StringComparer.OrdinalIgnoreCase))
            {
                continue;
            }

            if (System.IO.Directory.EnumerateFiles(SubDirectory, "*.Build.cs").Any())
            {
                continue;
            }

            CollectHeadersOnly(SubDirectory, Rules, Result, Seen);
        }
    }

    private static IEnumerable<string> ResolveSourceRoots(ModuleRules Rules)
    {
        if (Rules.SourceDirectories.Count > 0)
        {
            foreach (string Relative in Rules.SourceDirectories)
            {
                yield return Rules.ModulePath(Relative);
            }

            yield break;
        }

        yield return Rules.ResolveSourceRoot();
    }

    private static void ScanDirectory(
        string Directory,
        ModuleRules Rules,
        ModuleSourceSet Result,
        HashSet<string> Seen)
    {
        foreach (string FilePath in System.IO.Directory.EnumerateFiles(Directory))
        {
            Classify(FileItem.Get(FilePath), Rules, Result, Seen);
        }

        foreach (string SubDirectory in System.IO.Directory.EnumerateDirectories(Directory))
        {
            string Name = Path.GetFileName(SubDirectory);

            if (AlwaysIgnoredDirectories.Contains(Name, StringComparer.OrdinalIgnoreCase))
            {
                continue;
            }

            // A nested Build.cs marks another module's tree.
            if (System.IO.Directory.EnumerateFiles(SubDirectory, "*.Build.cs").Any())
            {
                Log.Trace("Module '{0}' skipping nested module directory '{1}'", Rules.Name, SubDirectory);
                continue;
            }

            ScanDirectory(SubDirectory, Rules, Result, Seen);
        }
    }

    private static void Classify(FileItem Item, ModuleRules Rules, ModuleSourceSet Result, HashSet<string> Seen)
    {
        if (!Seen.Add(Item.Location))
        {
            return;
        }

        if (IsExcluded(Item, Rules))
        {
            return;
        }

        string Extension = Item.Extension;

        if (Extension.Equals(".cpp", StringComparison.OrdinalIgnoreCase)
            || Extension.Equals(".cc", StringComparison.OrdinalIgnoreCase)
            || Extension.Equals(".cxx", StringComparison.OrdinalIgnoreCase))
        {
            Result.CppFiles.Add(Item);
        }
        else if (Extension.Equals(".c", StringComparison.OrdinalIgnoreCase))
        {
            Result.CFiles.Add(Item);
        }
        else if (HeaderExtensions.Contains(Extension))
        {
            Result.HeaderFiles.Add(Item);
        }
        else if (ResourceExtensions.Contains(Extension))
        {
            Result.ResourceFiles.Add(Item);
        }
    }

    private static bool IsExcluded(FileItem Item, ModuleRules Rules)
    {
        if (Rules.ExcludedSourcePathFragments.Count == 0)
        {
            return false;
        }

        string Relative = PathUtils.MakeRelativeTo(Item.Location, Rules.ModuleDirectory);

        foreach (string Fragment in Rules.ExcludedSourcePathFragments)
        {
            if (Relative.Contains(Fragment, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }

        return false;
    }
}
