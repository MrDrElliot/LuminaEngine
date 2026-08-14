using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;

namespace LuminaBuildTool.Toolchain.Linux;

public sealed class ClangToolchain : IToolchain
{
    private readonly UnixToolchainInstallation Installation;

    public ClangToolchain(UnixToolchainInstallation Installation)
    {
        this.Installation = Installation;
    }

    public string Name => Installation.Name;

    public string Description => Installation.ToString();

    public string VersionKey => $"{Installation.Name}-{Installation.Version}-{Installation.LinkerName ?? "default"}";

    public string ProjectToolsetName => "Linux";

    private bool bSupportsPrecompiledHeaders => Installation.Family == CompilerFamily.Clang;


    public List<BuildAction> CreateCompileActions(BuildTarget Target, BuildModule Module)
    {
        List<BuildAction> Actions = new();

        if (!Module.BinaryType.ProducesCompiledOutput())
        {
            return Actions;
        }

        List<FileItem> CppFiles = Module.CppCompileInputs;
        List<FileItem> CFiles = Module.Sources.CFiles;

        if (CppFiles.Count == 0 && CFiles.Count == 0)
        {
            return Actions;
        }

        BuildAction? PchAction = null;
        string? PchFile = null;
        FileItem? PchSourceItem = null;

        bool bUsePch = Module.Rules.PrecompiledHeader is not null
            && !Module.Rules.bCompileAsC
            && bSupportsPrecompiledHeaders;

        if (bUsePch)
        {
            string PchHeader = Module.Rules.ModulePath(Module.Rules.PrecompiledHeader!.Header);
            PchSourceItem = FileItem.Get(PchHeader);

            if (!PchSourceItem.Exists)
            {
                PchSourceItem = ResolveHeaderOnIncludePaths(Module, Module.Rules.PrecompiledHeader.Header)
                    ?? throw new BuildException(
                        $"Module '{Module.Name}' declares precompiled header "
                        + $"'{Module.Rules.PrecompiledHeader.Header}', which was not found on its include paths.");
            }

            PchFile = Path.Combine(Module.IntermediateDirectory, Module.Name + ".pch");
            PchAction = CreatePrecompiledHeaderAction(Target, Module, PchSourceItem, PchFile);
            Actions.Add(PchAction);
        }

        foreach (FileItem Source in CppFiles)
        {
            BuildAction Action = CreateCompileAction(Target, Module, Source, bIsC: false, PchFile);

            if (Module.SubsumedSourceFiles.TryGetValue(Source.Location, out List<FileItem>? Members))
            {
                Action.PrerequisiteItems.AddRange(Members);
                Action.StatusText = $"{Source.Name} ({Members.Count} files)";
            }

            if (PchAction is not null)
            {
                Action.OrderDependencies.Add(PchAction);
                Action.PrerequisiteItems.AddRange(PchAction.ProducedItems);
            }

            Actions.Add(Action);
        }

        foreach (FileItem Source in CFiles)
        {
            Actions.Add(CreateCompileAction(Target, Module, Source, bIsC: true, null));
        }

        return Actions;
    }

    private static FileItem? ResolveHeaderOnIncludePaths(BuildModule Module, string Header)
    {
        foreach (string IncludePath in Module.CompileIncludePaths)
        {
            FileItem Candidate = FileItem.Get(Path.Combine(IncludePath, Header));

            if (Candidate.Exists)
            {
                return Candidate;
            }
        }

        return null;
    }

    private BuildAction CreatePrecompiledHeaderAction(
        BuildTarget Target,
        BuildModule Module,
        FileItem Header,
        string PchFile)
    {
        string DependencyFile = PchFile + ".d";
        string ResponseFile = PchFile + ".rsp";

        List<string> Arguments = new()
        {
            "-x",
            "c++-header",
            "-c",
            "-o",
            PathUtils.QuoteUnix(PchFile),
            "-MD",
            "-MF",
            PathUtils.QuoteUnix(DependencyFile),
        };

        AddLanguageFlags(Target, Module, Arguments, bIsC: false);
        AddConfigurationFlags(Target, Arguments);
        AddEnvironmentFlags(Target, Module, Arguments);
        AddWarningFlags(Target, Module, Arguments);

        Arguments.AddRange(Target.Rules.GlobalCompilerOptions);
        Arguments.AddRange(Module.Rules.PrivateCompilerOptions);
        Arguments.Add(PathUtils.QuoteUnix(Header.Location));

        BuildAction Action = new(ActionType.Compile, Module.Name)
        {
            StatusText = Header.Name,
            EchoedInputName = Header.Name,
            ToolPath = Installation.CompilerPath,

            Arguments = "@" + PathUtils.Quote(ResponseFile),
            WorkingDirectory = Module.IntermediateDirectory,
            ResponseFilePath = ResponseFile,
            ResponseFileContents = string.Join(Environment.NewLine, Arguments),
            DependencyListFile = DependencyFile,
            DependencyListFormat = DependencyListFormat.Makefile,
            ToolchainIdentity = VersionKey,
        };

        Action.PrerequisiteItems.Add(Header);
        Action.ProducedItems.Add(FileItem.Get(PchFile));

        return Action;
    }

    private BuildAction CreateCompileAction(
        BuildTarget Target,
        BuildModule Module,
        FileItem Source,
        bool bIsC,
        string? PchFile)
    {
        string ObjectFile = GetObjectFilePath(Module, Source);
        string DependencyFile = ObjectFile + ".d";
        string ResponseFile = ObjectFile + ".rsp";

        List<string> Arguments = new()
        {
            "-c",
            "-o",
            PathUtils.QuoteUnix(ObjectFile),

            "-MD",
            "-MF",
            PathUtils.QuoteUnix(DependencyFile),
        };

        AddLanguageFlags(Target, Module, Arguments, bIsC);
        AddConfigurationFlags(Target, Arguments);
        AddEnvironmentFlags(Target, Module, Arguments);

        if (PchFile is not null)
        {
            Arguments.Add("-include-pch");
            Arguments.Add(PathUtils.QuoteUnix(PchFile));
        }
        else if (!bIsC && Module.Rules.PrecompiledHeader is not null && !Module.Rules.bCompileAsC)
        {
            Arguments.Add("-include");
            Arguments.Add(PathUtils.QuoteUnix(Module.Rules.PrecompiledHeader.Header));
        }

        foreach (string ForceInclude in Module.ForceIncludeFiles)
        {
            Arguments.Add("-include");
            Arguments.Add(PathUtils.QuoteUnix(ForceInclude));
        }

        AddWarningFlags(Target, Module, Arguments);

        Arguments.AddRange(Target.Rules.GlobalCompilerOptions);
        Arguments.AddRange(Module.Rules.PrivateCompilerOptions);

        if (Module.Rules.PerFileCompilerOptions.TryGetValue(Source.Name, out List<string>? PerFile))
        {
            Arguments.AddRange(PerFile);
        }

        Arguments.Add(PathUtils.QuoteUnix(Source.Location));

        BuildAction Action = new(ActionType.Compile, Module.Name)
        {
            StatusText = Source.Name,
            EchoedInputName = Source.Name,
            ToolPath = bIsC ? Installation.CCompilerPath : Installation.CompilerPath,
            Arguments = "@" + PathUtils.Quote(ResponseFile),
            WorkingDirectory = Module.IntermediateDirectory,
            ResponseFilePath = ResponseFile,
            ResponseFileContents = string.Join(Environment.NewLine, Arguments),
            DependencyListFile = DependencyFile,
            DependencyListFormat = DependencyListFormat.Makefile,
            ToolchainIdentity = VersionKey,
        };

        Action.PrerequisiteItems.Add(Source);
        Action.ProducedItems.Add(FileItem.Get(ObjectFile));

        return Action;
    }

    /// <summary>Language standard, front end selection, exceptions and RTTI.</summary>
    private static void AddLanguageFlags(BuildTarget Target, BuildModule Module, List<string> Arguments, bool bIsC)
    {
        if (bIsC || Module.Rules.bCompileAsC)
        {
            Arguments.Add("-x");
            Arguments.Add("c");
            return;
        }

        bool bExceptions = Module.Rules.bEnableExceptions ?? Target.Rules.bEnableExceptions;
        bool bRtti = Module.Rules.bEnableRtti ?? Target.Rules.bEnableRtti;

        Arguments.Add($"-std={TranslateCppStandard(Module.Rules.CppStandardOverride ?? Target.Rules.CppStandard)}");
        Arguments.Add(bExceptions ? "-fexceptions" : "-fno-exceptions");
        Arguments.Add(bRtti ? "-frtti" : "-fno-rtti");
    }

    private static string TranslateCppStandard(string Standard)
    {
        return Standard.ToLowerInvariant() switch
        {
            "c++latest" => "c++23",
            "c++23" or "c++2b" => "c++23",
            "c++20" or "c++2a" => "c++20",
            "c++17" => "c++17",
            "c++14" => "c++14",
            _ => Standard,
        };
    }

    private const string TargetTriple = "x86_64-unknown-linux-gnu";

    private void AddConfigurationFlags(BuildTarget Target, List<string> Arguments)
    {
        if (Installation.Family == CompilerFamily.Clang)
        {
            Arguments.Add($"--target={TargetTriple}");
        }

        Arguments.Add("-fPIC");

        // Classes whose vtable crosses a module boundary carry LUMINA_VISIBLE_TYPE. -fvisibility-ms-compat
        // does not cover them: it relaxes typeinfo, but vtables still follow the class's own visibility.
        Arguments.Add("-fvisibility=hidden");
        Arguments.Add("-fvisibility-inlines-hidden");

        if (Target.Rules.VectorExtensions.Length > 0)
        {
            Arguments.Add(TranslateVectorExtensions(Target.Rules.VectorExtensions));
        }

        if (Target.Rules.bDebugSymbols)
        {
            Arguments.Add("-g");
        }

        switch (Target.Info.Configuration)
        {
            case BuildConfiguration.Debug:
                Arguments.Add("-O0");
                Arguments.Add("-fno-inline");
                break;

            case BuildConfiguration.Development:
                Arguments.Add("-O2");
                break;

            case BuildConfiguration.Shipping:
                Arguments.Add("-O3");

                Arguments.Add("-ffunction-sections");
                Arguments.Add("-fdata-sections");

                if (Target.Rules.bLinkTimeCodeGeneration)
                {
                    Arguments.Add(GetLinkTimeOptimizationFlag());
                }

                break;
        }
    }

    private string GetLinkTimeOptimizationFlag()
    {
        return Installation.Family == CompilerFamily.Clang ? "-flto=thin" : "-flto";
    }

    private static string TranslateVectorExtensions(string Extensions)
    {
        return Extensions.ToUpperInvariant() switch
        {
            "AVX" => "-mavx",
            "AVX2" => "-mavx2",
            "AVX512" => "-mavx512f",
            "SSE2" => "-msse2",
            "SSE4.2" or "SSE42" => "-msse4.2",
            _ => "-m" + Extensions.ToLowerInvariant(),
        };
    }

    private static void AddEnvironmentFlags(BuildTarget Target, BuildModule Module, List<string> Arguments)
    {
        foreach (string Definition in Module.CompileDefinitions)
        {
            Arguments.Add("-D" + PathUtils.QuoteUnix(Definition));
        }

        foreach (string IncludePath in Module.CompileIncludePaths)
        {
            Arguments.Add("-I" + PathUtils.QuoteUnix(IncludePath));
        }
    }

    private static void AddWarningFlags(BuildTarget Target, BuildModule Module, List<string> Arguments)
    {
        if (Module.Rules.bIsThirdParty)
        {
            Arguments.Add("-w");
            return;
        }

        Arguments.Add(Target.Rules.WarningLevel switch
        {
            <= 0 => "-w",
            1 or 2 => "-Wall",
            _ => "-Wall",
        });

        if (Target.Rules.WarningLevel >= 4)
        {
            Arguments.Add("-Wextra");
        }

        foreach (string Warning in Target.Rules.GlobalDisabledWarnings.Concat(Module.Rules.DisabledWarnings))
        {
            if (TranslateWarningName(Warning) is string Name)
            {
                Arguments.Add("-Wno-" + Name);
            }
        }

        foreach (string Warning in Module.Rules.FatalWarnings)
        {
            if (TranslateWarningName(Warning) is string Name)
            {
                Arguments.Add("-Werror=" + Name);
            }
        }

        if (Target.Rules.bWarningsAsErrors)
        {
            Arguments.Add("-Werror");
        }

        Arguments.Add("-Wno-unknown-warning-option");
        Arguments.Add("-Wno-unknown-pragmas");
    }

    private static string? TranslateWarningName(string Warning)
    {
        string Trimmed = Warning.TrimStart('C', 'c');

        if (Trimmed.Length > 0 && Trimmed.All(char.IsDigit))
        {
            return null;
        }

        return Warning.StartsWith("-W", StringComparison.Ordinal) ? Warning[2..] : Warning;
    }

    private static string GetObjectFilePath(BuildModule Module, FileItem Source)
    {
        if (Module.GeneratedCodeDirectory.Length > 0 && PathUtils.IsUnder(Source.Location, Module.GeneratedCodeDirectory))
        {
            return Path.Combine(Module.IntermediateDirectory, "Generated", Path.ChangeExtension(Source.Name, ".o"));
        }

        if (PathUtils.IsUnder(Source.Location, Path.Combine(Module.IntermediateDirectory, UnityBuildStep.BlobDirectoryName)))
        {
            return Path.ChangeExtension(Source.Location, ".o");
        }

        string Relative = PathUtils.MakeRelativeTo(Source.Location, Module.Rules.ModuleDirectory);

        if (Relative.StartsWith("..", StringComparison.Ordinal) || Path.IsPathRooted(Relative))
        {
            Relative = Path.Combine("External", ContentHash.OfString(Source.Directory), Source.Name);
        }

        return Path.Combine(Module.IntermediateDirectory, Path.ChangeExtension(Relative, ".o"));
    }


    public BuildAction? CreateLinkAction(BuildTarget Target, BuildModule Module, IReadOnlyList<BuildAction> CompileActions)
    {
        if (!Module.BinaryType.ProducesCompiledOutput() || Module.OutputFile.Length == 0)
        {
            return null;
        }

        List<FileItem> ObjectFiles = CompileActions
            .SelectMany(A => A.ProducedItems)
            .Where(F => F.Extension.Equals(".o", StringComparison.Ordinal))
            .ToList();

        if (ObjectFiles.Count == 0)
        {
            return null;
        }

        return Module.BinaryType == ModuleBinaryType.StaticLibrary
            ? CreateArchiveAction(Target, Module, ObjectFiles, CompileActions)
            : CreateBinaryLinkAction(Target, Module, ObjectFiles, CompileActions);
    }

    private BuildAction CreateArchiveAction(
        BuildTarget Target,
        BuildModule Module,
        IReadOnlyList<FileItem> ObjectFiles,
        IReadOnlyList<BuildAction> CompileActions)
    {
        List<string> Arguments = new()
        {
            "rcs",
            PathUtils.Quote(Module.OutputFile),
        };

        Arguments.AddRange(ObjectFiles.Select(F => PathUtils.Quote(F.Location)));

        BuildAction Action = new(ActionType.Archive, Module.Name)
        {
            StatusText = Path.GetFileName(Module.OutputFile),
            ToolPath = Installation.ArchiverPath,
            Arguments = string.Join(' ', Arguments),
            WorkingDirectory = Module.IntermediateDirectory,
            ToolchainIdentity = VersionKey,
            bCanExecuteInParallel = true,
            bDeleteOutputsBeforeRun = true,
        };

        Action.PrerequisiteItems.AddRange(ObjectFiles);
        Action.ProducedItems.Add(FileItem.Get(Module.OutputFile));
        Action.OrderDependencies.AddRange(CompileActions);

        return Action;
    }

    private BuildAction CreateBinaryLinkAction(
        BuildTarget Target,
        BuildModule Module,
        IReadOnlyList<FileItem> ObjectFiles,
        IReadOnlyList<BuildAction> CompileActions)
    {
        bool bIsSharedLibrary = Module.BinaryType == ModuleBinaryType.SharedLibrary;

        List<string> Arguments = new()
        {
            "-o",
            PathUtils.QuoteUnix(Module.OutputFile),
        };

        if (Installation.Family == CompilerFamily.Clang)
        {
            Arguments.Add($"--target={TargetTriple}");
        }

        if (Installation.LinkerName is not null)
        {
            Arguments.Add($"-fuse-ld={Installation.LinkerName}");
        }

        if (bIsSharedLibrary)
        {
            Arguments.Add("-shared");

            Arguments.Add($"-Wl,-soname,{Path.GetFileName(Module.OutputFile)}");
        }

        Arguments.Add("-Wl,-rpath,$ORIGIN");
        Arguments.Add("-Wl,-rpath,$ORIGIN/../lib");
        Arguments.Add("-Wl,-z,origin");

        if (Target.Rules.bDebugSymbols)
        {
            Arguments.Add("-g");
        }

        if (Target.Info.Configuration == BuildConfiguration.Shipping)
        {
            Arguments.Add("-Wl,--gc-sections");

            if (Target.Rules.bLinkTimeCodeGeneration)
            {
                Arguments.Add(GetLinkTimeOptimizationFlag());
            }
        }

        foreach (string LibraryPath in Module.LinkLibraryPaths)
        {
            Arguments.Add("-L" + PathUtils.QuoteUnix(LibraryPath));
        }

        Arguments.AddRange(Target.Rules.GlobalLinkerOptions);
        Arguments.AddRange(Module.Rules.PrivateLinkerOptions);
        Arguments.AddRange(ObjectFiles.Select(F => PathUtils.QuoteUnix(F.Location)));

        AddWholeArchiveLibraries(Module, Arguments);
        AddLinkLibraries(Module, Arguments);

        Arguments.Add("-pthread");
        Arguments.Add("-ldl");
        Arguments.Add("-lm");
        Arguments.Add("-lrt");

        string ResponseFile = Path.Combine(Module.IntermediateDirectory, Module.Name + ".link.rsp");

        BuildAction Action = new(ActionType.Link, Module.Name)
        {
            StatusText = Path.GetFileName(Module.OutputFile),
            ToolPath = Installation.CompilerPath,
            Arguments = "@" + PathUtils.Quote(ResponseFile),
            WorkingDirectory = Module.IntermediateDirectory,
            ResponseFilePath = ResponseFile,
            ResponseFileContents = string.Join(Environment.NewLine, Arguments),
            ToolchainIdentity = VersionKey,
        };

        Action.PrerequisiteItems.AddRange(ObjectFiles);

        foreach (string Library in Module.LinkLibraries.Where(Path.IsPathRooted))
        {
            Action.PrerequisiteItems.Add(FileItem.Get(Library));
        }

        Action.ProducedItems.Add(FileItem.Get(Module.OutputFile));
        Action.OrderDependencies.AddRange(CompileActions);

        return Action;
    }

    private static void AddWholeArchiveLibraries(BuildModule Module, List<string> Arguments)
    {
        List<string> Libraries = Module.EnumerateDependencyClosure()
            .Where(D => D.bRequiresWholeArchive && D.OutputFile.Length > 0)
            .Select(D => D.OutputFile)
            .ToList();

        if (Libraries.Count == 0)
        {
            return;
        }

        Arguments.Add("-Wl,--whole-archive");
        Arguments.AddRange(Libraries.Select(PathUtils.Quote));
        Arguments.Add("-Wl,--no-whole-archive");
    }

    /// <summary>Names a module may use to ask for the backtrace implementation behind std::stacktrace.</summary>
    private static readonly string[] StacktraceLibraryAliases =
    {
        "stdc++exp",
        "stdc++_libbacktrace",
    };

    private void AddLinkLibraries(BuildModule Module, List<string> Arguments)
    {
        List<string> Resolved = new();

        foreach (string Library in Module.LinkLibraries)
        {
            string? Substituted = Array.IndexOf(StacktraceLibraryAliases, Library) >= 0
                ? Installation.StacktraceLibrary
                : Library;

            // Null is the libc++ case: the header saw no __cpp_lib_stacktrace, so there is nothing to link.
            if (Substituted is null || Resolved.Contains(Substituted))
            {
                continue;
            }

            Resolved.Add(Substituted);
        }

        if (Resolved.Count == 0)
        {
            return;
        }

        Arguments.Add("-Wl,--start-group");

        foreach (string Library in Resolved)
        {
            Arguments.Add(TranslateLibraryReference(Library));
        }

        Arguments.Add("-Wl,--end-group");
    }

    private static string TranslateLibraryReference(string Library)
    {
        if (Path.IsPathRooted(Library))
        {
            return PathUtils.QuoteUnix(Library);
        }

        string Name = Library;

        if (Name.EndsWith(".so", StringComparison.Ordinal))
        {
            Name = Name[..^3];
        }
        else if (Name.EndsWith(".a", StringComparison.Ordinal))
        {
            Name = Name[..^2];
        }

        if (Name.StartsWith("lib", StringComparison.Ordinal))
        {
            Name = Name[3..];
        }

        return "-l" + PathUtils.QuoteUnix(Name);
    }
}
