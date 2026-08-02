using System.Text;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;

namespace LuminaBuildTool.Toolchain.Windows;

/// <summary>
/// Drives cl.exe, link.exe and lib.exe. Every MSVC-specific command-line decision lives here;
/// the rest of the build system never sees a compiler flag.
/// </summary>
public sealed class MsvcToolchain : IToolchain
{
    private readonly MsvcInstallation Compiler;

    private readonly WindowsSdkInstallation Sdk;

    private readonly Dictionary<string, string> Environment;

    public MsvcToolchain(MsvcInstallation Compiler, WindowsSdkInstallation Sdk)
    {
        this.Compiler = Compiler;
        this.Sdk = Sdk;

        Environment = BuildEnvironment();
    }

    public string Name => "MSVC";

    public string Description => $"{Compiler} with {Sdk}";

    public string VersionKey => $"MSVC-{Compiler.ToolsVersion}-SDK-{Sdk.Version}";

    /// <summary>
    /// Derived from the compiler's major and minor version. Visual Studio keeps shipping older
    /// toolsets, so an unrecognized future version falls back to the newest one this tool knows
    /// rather than failing: the value only has to be a toolset the IDE can load.
    /// </summary>
    public string ProjectToolsetName
    {
        get
        {
            if (!Version.TryParse(Compiler.ToolsVersion, out Version? Parsed))
            {
                return "v143";
            }

            return (Parsed.Major, Parsed.Minor) switch
            {
                (14, >= 30) => "v143",
                (14, >= 20) => "v142",
                (14, >= 10) => "v141",
                (14, _) => "v140",
                (< 14, _) => "v140",
                _ => "v143",
            };
        }
    }

    private Dictionary<string, string> BuildEnvironment()
    {
        List<string> IncludePaths = new() { Compiler.IncludeDirectory };
        IncludePaths.AddRange(Sdk.IncludeDirectories);

        List<string> LibraryPaths = new() { Compiler.LibraryDirectory };
        LibraryPaths.AddRange(Sdk.LibraryDirectories);

        // cl.exe loads its own DLLs from the toolset bin directory, so it must be on PATH.
        string ExistingPath = System.Environment.GetEnvironmentVariable("PATH") ?? string.Empty;

        return new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["INCLUDE"] = string.Join(';', IncludePaths),
            ["LIB"] = string.Join(';', LibraryPaths),
            ["PATH"] = string.Join(';', new[] { Compiler.BinDirectory, Sdk.BinDirectory, ExistingPath }),
        };
    }

    // Compilation.

    public List<BuildAction> CreateCompileActions(BuildTarget Target, BuildModule Module)
    {
        List<BuildAction> Actions = new();

        if (!Module.BinaryType.ProducesCompiledOutput())
        {
            return Actions;
        }

        // What a C++ compile is was settled when the graph was assembled: a source on its own, or
        // a unity blob standing in for several.
        List<FileItem> CppFiles = Module.CppCompileInputs;
        List<FileItem> CFiles = Module.Sources.CFiles;

        if (CppFiles.Count == 0 && CFiles.Count == 0)
        {
            return Actions;
        }

        BuildAction? PchAction = null;
        string? PchFile = null;
        FileItem? PchSourceItem = null;

        if (Module.Rules.PrecompiledHeader is not null && !Module.Rules.bCompileAsC)
        {
            PchSourceItem = FileItem.Get(Module.Rules.ModulePath(Module.Rules.PrecompiledHeader.Source));

            if (!PchSourceItem.Exists)
            {
                throw new BuildException(
                    $"Module '{Module.Name}' declares PCH source '{PchSourceItem.Location}', which does not exist.");
            }

            PchFile = Path.Combine(Module.IntermediateDirectory, Module.Name + ".pch");
            PchAction = CreateCompileAction(Target, Module, PchSourceItem, bIsC: false, PchFile, PchMode.Create);
            Actions.Add(PchAction);
        }

        foreach (FileItem Source in CppFiles)
        {
            if (PchSourceItem is not null && Source.Equals(PchSourceItem))
            {
                continue;
            }

            BuildAction Action = CreateCompileAction(
                Target, Module, Source, bIsC: false, PchFile, PchFile is null ? PchMode.None : PchMode.Use);

            // A blob's members are inputs to it. The dependency JSON reports them too once the blob
            // has compiled at least once, but declaring them keeps the very first build, and any
            // build after the cache is cleared, from missing an edit to a member.
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

        // C sources never share the C++ precompiled header.
        foreach (FileItem Source in CFiles)
        {
            Actions.Add(CreateCompileAction(Target, Module, Source, bIsC: true, null, PchMode.None));
        }

        return Actions;
    }

    private enum PchMode
    {
        None,
        Create,
        Use,
    }

    private BuildAction CreateCompileAction(
        BuildTarget Target,
        BuildModule Module,
        FileItem Source,
        bool bIsC,
        string? PchFile,
        PchMode Pch)
    {
        string ObjectFile = GetObjectFilePath(Module, Source);
        string DependencyFile = ObjectFile + ".json";
        string ResponseFile = ObjectFile + ".rsp";

        List<string> Arguments = new()
        {
            "/nologo",
            "/c",
            "/bigobj",
            "/permissive-",
            "/Zc:preprocessor",
            "/Zc:inline",
            "/Zc:__cplusplus",

            // Third-party code is not ours to fix, so it compiles quietly.
            Module.Rules.bIsThirdParty ? "/W0" : $"/W{Target.Rules.WarningLevel}",
            $"/Fo{PathUtils.Quote(ObjectFile)}",

            // Emits the full header closure as JSON, which is what drives header-change rebuilds.
            // The path must stay on the flag's own line or cl reports D8004.
            $"/sourceDependencies {PathUtils.Quote(DependencyFile)}",
        };

        if (!bIsC && !Module.Rules.bCompileAsC)
        {
            bool bExceptions = Module.Rules.bEnableExceptions ?? Target.Rules.bEnableExceptions;
            bool bRtti = Module.Rules.bEnableRtti ?? Target.Rules.bEnableRtti;

            Arguments.Add($"/std:{Module.Rules.CppStandardOverride ?? Target.Rules.CppStandard}");
            Arguments.Add(bExceptions ? "/EHsc" : "/EHs-c-");
            Arguments.Add(bRtti ? "/GR" : "/GR-");
            Arguments.Add("/TP");
        }
        else
        {
            Arguments.Add("/TC");
        }

        if (Target.Rules.VectorExtensions.Length > 0)
        {
            Arguments.Add($"/arch:{Target.Rules.VectorExtensions}");
        }

        AddConfigurationFlags(Target, Arguments);

        // /Z7 keeps debug info in the object file, which avoids serializing every compile
        // through a single mspdbsrv instance.
        if (Target.Rules.bDebugSymbols)
        {
            Arguments.Add("/Z7");
        }

        foreach (string Definition in Module.CompileDefinitions)
        {
            Arguments.Add($"/D{PathUtils.Quote(Definition)}");
        }

        foreach (string IncludePath in Module.CompileIncludePaths)
        {
            Arguments.Add($"/I{PathUtils.Quote(IncludePath)}");
        }

        // The precompiled header must be force-included first so /Yu finds its marker in every
        // translation unit, including the ones that never write the #include themselves.
        if (Pch != PchMode.None && Module.Rules.PrecompiledHeader is not null)
        {
            Arguments.Add($"/FI{PathUtils.Quote(Module.Rules.PrecompiledHeader.Header)}");
        }

        foreach (string ForceInclude in Module.ForceIncludeFiles)
        {
            Arguments.Add($"/FI{PathUtils.Quote(ForceInclude)}");
        }

        foreach (string Warning in Target.Rules.GlobalDisabledWarnings.Concat(Module.Rules.DisabledWarnings))
        {
            Arguments.Add($"/wd{Warning}");
        }

        foreach (string Warning in Module.Rules.FatalWarnings)
        {
            Arguments.Add($"/we{Warning}");
        }

        if (Target.Rules.bWarningsAsErrors && !Module.Rules.bIsThirdParty)
        {
            Arguments.Add("/WX");
        }

        Arguments.AddRange(Target.Rules.GlobalCompilerOptions);
        Arguments.AddRange(Module.Rules.PrivateCompilerOptions);

        if (Module.Rules.PerFileCompilerOptions.TryGetValue(Source.Name, out List<string>? PerFile))
        {
            Arguments.AddRange(PerFile);
        }

        List<FileItem> Produced = new() { FileItem.Get(ObjectFile) };

        switch (Pch)
        {
            case PchMode.Create:
                Arguments.Add($"/Yc{PathUtils.Quote(Module.Rules.PrecompiledHeader!.Header)}");
                Arguments.Add($"/Fp{PathUtils.Quote(PchFile!)}");
                Produced.Add(FileItem.Get(PchFile!));
                break;

            case PchMode.Use:
                Arguments.Add($"/Yu{PathUtils.Quote(Module.Rules.PrecompiledHeader!.Header)}");
                Arguments.Add($"/Fp{PathUtils.Quote(PchFile!)}");
                break;
        }

        Arguments.Add(PathUtils.Quote(Source.Location));

        BuildAction Action = new(ActionType.Compile, Module.Name)
        {
            StatusText = Source.Name,
            EchoedInputName = Source.Name,
            ToolPath = Compiler.CompilerPath,
            Arguments = "@" + PathUtils.Quote(ResponseFile),
            WorkingDirectory = Module.IntermediateDirectory,
            ResponseFilePath = ResponseFile,
            ResponseFileContents = string.Join(System.Environment.NewLine, Arguments),
            DependencyListFile = DependencyFile,
            EnvironmentOverrides = Environment,
            ToolchainIdentity = VersionKey,
        };

        Action.PrerequisiteItems.Add(Source);
        Action.ProducedItems.AddRange(Produced);

        return Action;
    }

    private static void AddConfigurationFlags(BuildTarget Target, List<string> Arguments)
    {
        bool bDynamicCrt = Target.Rules.bUseDynamicCrt;
        bool bDebugCrt = Target.Rules.bUseDebugCrt;

        Arguments.Add((bDynamicCrt, bDebugCrt) switch
        {
            (true, true) => "/MDd",
            (true, false) => "/MD",
            (false, true) => "/MTd",
            (false, false) => "/MT",
        });

        switch (Target.Info.Configuration)
        {
            case BuildConfiguration.Debug:
                Arguments.Add("/Od");
                Arguments.Add("/Ob0");
                break;

            case BuildConfiguration.Development:
                Arguments.Add("/O2");
                Arguments.Add("/Ob2");
                break;

            case BuildConfiguration.Shipping:
                Arguments.Add("/O2");
                Arguments.Add("/Ob3");
                Arguments.Add("/Gy");

                if (Target.Rules.bLinkTimeCodeGeneration)
                {
                    Arguments.Add("/GL");
                }

                break;
        }
    }

    /// <summary>
    /// The set MSBuild's C++ rules put on every link line by default. Engine code reaches for
    /// COM, the shell and the common dialogs without declaring them, so the toolchain supplies
    /// them rather than making every module restate the platform baseline.
    /// </summary>
    private static readonly string[] DefaultSystemLibraries =
    {
        "kernel32.lib",
        "user32.lib",
        "gdi32.lib",
        "winspool.lib",
        "comdlg32.lib",
        "advapi32.lib",
        "shell32.lib",
        "ole32.lib",
        "oleaut32.lib",
        "uuid.lib",
        "odbc32.lib",
        "odbccp32.lib",
    };

    /// <summary>
    /// link.exe treats an extensionless name as an object file, so a bare library name such as
    /// "clangBasic" has to become "clangBasic.lib" before it reaches the command line.
    /// </summary>
    private static string NormalizeLibraryName(string Library)
    {
        return Path.HasExtension(Library) ? Library : Library + ".lib";
    }

    /// <summary>
    /// Mirrors the module's directory layout under the intermediate directory so two source
    /// files sharing a name never collide on one object file.
    /// </summary>
    private static string GetObjectFilePath(BuildModule Module, FileItem Source)
    {
        // Generated sources live outside the module tree; give them their own stable subdirectory
        // rather than letting them fall into the external hash bucket.
        if (Module.GeneratedCodeDirectory.Length > 0 && PathUtils.IsUnder(Source.Location, Module.GeneratedCodeDirectory))
        {
            return Path.Combine(Module.IntermediateDirectory, "Generated", Path.ChangeExtension(Source.Name, ".obj"));
        }

        // Unity files are generated into the intermediates rather than the module tree, so they
        // would otherwise be keyed by a hash of a directory that moves with the configuration.
        if (PathUtils.IsUnder(Source.Location, Path.Combine(Module.IntermediateDirectory, UnityBuildStep.BlobDirectoryName)))
        {
            return Path.ChangeExtension(Source.Location, ".obj");
        }

        string Relative = PathUtils.MakeRelativeTo(Source.Location, Module.Rules.ModuleDirectory);

        if (Relative.StartsWith("..", StringComparison.Ordinal) || Path.IsPathRooted(Relative))
        {
            // Outside the module tree; key it by its own directory so it stays unique and stable.
            Relative = Path.Combine("External", ContentHash.OfString(Source.Directory), Source.Name);
        }

        return Path.Combine(Module.IntermediateDirectory, Path.ChangeExtension(Relative, ".obj"));
    }

    // Linking.

    public BuildAction? CreateLinkAction(BuildTarget Target, BuildModule Module, IReadOnlyList<BuildAction> CompileActions)
    {
        if (!Module.BinaryType.ProducesCompiledOutput() || Module.OutputFile.Length == 0)
        {
            return null;
        }

        List<FileItem> ObjectFiles = CompileActions
            .SelectMany(A => A.ProducedItems)
            .Where(F => F.Extension.Equals(".obj", StringComparison.OrdinalIgnoreCase))
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
            "/nologo",
            "/MACHINE:X64",
            $"/OUT:{PathUtils.Quote(Module.OutputFile)}",
        };

        if (Target.Info.Configuration == BuildConfiguration.Shipping && Target.Rules.bLinkTimeCodeGeneration)
        {
            Arguments.Add("/LTCG");
        }

        Arguments.AddRange(ObjectFiles.Select(F => PathUtils.Quote(F.Location)));

        string ResponseFile = Path.Combine(Module.IntermediateDirectory, Module.Name + ".lib.rsp");

        BuildAction Action = new(ActionType.Archive, Module.Name)
        {
            StatusText = Path.GetFileName(Module.OutputFile),
            ToolPath = Compiler.ArchiverPath,
            Arguments = "@" + PathUtils.Quote(ResponseFile),
            WorkingDirectory = Module.IntermediateDirectory,
            ResponseFilePath = ResponseFile,
            ResponseFileContents = string.Join(System.Environment.NewLine, Arguments),
            EnvironmentOverrides = Environment,
            ToolchainIdentity = VersionKey,
            bCanExecuteInParallel = true,
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
            "/nologo",
            "/MACHINE:X64",
            $"/OUT:{PathUtils.Quote(Module.OutputFile)}",
        };

        if (bIsSharedLibrary)
        {
            Arguments.Add("/DLL");

            if (Module.ImportLibraryFile.Length > 0)
            {
                Arguments.Add($"/IMPLIB:{PathUtils.Quote(Module.ImportLibraryFile)}");
            }
        }
        else
        {
            Arguments.Add(Module.BinaryType == ModuleBinaryType.WindowedApplication
                ? "/SUBSYSTEM:WINDOWS"
                : "/SUBSYSTEM:CONSOLE");
        }

        if (Target.Rules.bDebugSymbols)
        {
            Arguments.Add("/DEBUG");
            Arguments.Add($"/PDB:{PathUtils.Quote(Path.ChangeExtension(Module.OutputFile, ".pdb"))}");
        }

        if (Target.Info.Configuration == BuildConfiguration.Shipping)
        {
            Arguments.Add("/OPT:REF");
            Arguments.Add("/OPT:ICF");
            Arguments.Add("/INCREMENTAL:NO");

            if (Target.Rules.bLinkTimeCodeGeneration)
            {
                Arguments.Add("/LTCG");
            }
        }
        else
        {
            Arguments.Add(Target.Rules.bIncrementalLinking ? "/INCREMENTAL" : "/INCREMENTAL:NO");
            Arguments.Add("/OPT:NOREF");
            Arguments.Add("/OPT:NOICF");
        }

        foreach (string LibraryPath in Module.LinkLibraryPaths)
        {
            Arguments.Add($"/LIBPATH:{PathUtils.Quote(LibraryPath)}");
        }

        // Monolithic: module registration happens in static constructors that nothing references,
        // so the linker would discard those objects without being told to take the whole archive.
        foreach (BuildModule Dependency in Module.EnumerateDependencyClosure())
        {
            if (Dependency.bRequiresWholeArchive && Dependency.OutputFile.Length > 0)
            {
                Arguments.Add($"/WHOLEARCHIVE:{PathUtils.Quote(Dependency.OutputFile)}");
            }
        }

        Arguments.AddRange(Target.Rules.GlobalLinkerOptions);
        Arguments.AddRange(Module.Rules.PrivateLinkerOptions);
        Arguments.AddRange(ObjectFiles.Select(F => PathUtils.Quote(F.Location)));
        Arguments.AddRange(Module.LinkLibraries.Select(L => PathUtils.Quote(NormalizeLibraryName(L))));
        Arguments.AddRange(DefaultSystemLibraries);

        string ResponseFile = Path.Combine(Module.IntermediateDirectory, Module.Name + ".link.rsp");

        BuildAction Action = new(ActionType.Link, Module.Name)
        {
            StatusText = Path.GetFileName(Module.OutputFile),
            ToolPath = Compiler.LinkerPath,
            Arguments = "@" + PathUtils.Quote(ResponseFile),
            WorkingDirectory = Module.IntermediateDirectory,
            ResponseFilePath = ResponseFile,
            ResponseFileContents = string.Join(System.Environment.NewLine, Arguments),
            EnvironmentOverrides = Environment,
            ToolchainIdentity = VersionKey,
        };

        Action.PrerequisiteItems.AddRange(ObjectFiles);

        // The import libraries this binary links are inputs; a dependency relink must relink us.
        foreach (string Library in Module.LinkLibraries.Where(Path.IsPathRooted))
        {
            Action.PrerequisiteItems.Add(FileItem.Get(Library));
        }

        Action.ProducedItems.Add(FileItem.Get(Module.OutputFile));

        if (bIsSharedLibrary && Module.ImportLibraryFile.Length > 0)
        {
            Action.OptionalProducedItems.Add(FileItem.Get(Module.ImportLibraryFile));
        }

        Action.OrderDependencies.AddRange(CompileActions);

        return Action;
    }
}
