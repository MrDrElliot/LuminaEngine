using System.Diagnostics;
using System.IO.Compression;
using System.Text.Json;
using System.Text.RegularExpressions;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.Rules;
using LuminaBuildTool.Toolchain.Windows;

namespace LuminaBuildTool.Modes;

// Stages a prebuilt engine in the source tree's layout, since the engine resolves its root from its exe.
public static class PackageMode
{
    private const string LauncherTarget = "LuminaLauncher";
    private const string LauncherFileName = "Lumina Editor.exe";
    private const string EngineTarget = "Lumina";

    // Must match Paths::InstalledBuildMarker, which is how the engine knows it was packaged.
    private const string InstalledBuildMarker = "Engine/InstalledBuild.json";

    // Build and IDE leftovers that only ever appear inside otherwise shipped folders.
    private static readonly HashSet<string> ExcludedDirectoryNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "obj", "bin", ".idea", ".vs", "Intermediates", "Saved",
    };

    private static readonly HashSet<string> ExcludedExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".csproj", ".user", ".ilk", ".exp", ".lib",
    };

    public static async Task<int> RunAsync(CommandLine Arguments, BuildDirectories Directories, CancellationToken Cancellation)
    {
        if (Directories.ProjectRoot is not null)
        {
            throw new BuildException("Package stages the engine itself, so it does not take -Project.");
        }

        BuildPlatform PlatformValue = Arguments.GetEnum("Platform", BuildPlatformRegistry.HostPlatform);
        if (PlatformValue != BuildPlatform.Windows64)
        {
            throw new BuildException("Package currently produces Windows64 packages only.");
        }

        BuildConfiguration ConfigurationValue = Arguments.GetEnum("Configuration", BuildConfiguration.Development);
        if (ConfigurationValue == BuildConfiguration.Debug)
        {
            throw new BuildException(
                "A Debug package would need the debug C runtime, which may not be redistributed. Use Development or Shipping.");
        }

        bool bIncludeGame = !Arguments.HasFlag("NoGame");
        bool bIncludeSymbols = Arguments.HasFlag("Symbols");

        Stopwatch Timer = Stopwatch.StartNew();
        RulesAssembly Assembly = RulesAssembly.Create(Directories, Arguments.HasFlag("RecompileRules"));

        // Every engine plugin is built, since a designer's project may opt into one the engine leaves off.
        Arguments = EnableEnginePlugins(Arguments, Assembly, Directories);

        if (!Arguments.HasFlag("NoBuild"))
        {
            int Result = await BuildEverything(Arguments, Directories, Assembly, PlatformValue, ConfigurationValue, bIncludeGame, Cancellation)
                .ConfigureAwait(false);
            if (Result != 0)
            {
                return Result;
            }
        }

        string Version = ReadEngineVersion(Directories);
        string PackageName = $"Lumina-{Version}-{PlatformValue.GetOutputDirectoryName()}-{ConfigurationValue}";
        string OutputDirectory = Path.GetFullPath(Arguments.GetString("Output", Path.Combine(Directories.EngineRoot, "Saved", "Packages")));
        string StageDirectory = Path.Combine(OutputDirectory, PackageName);

        Log.Info("Staging {0} in {1}", PackageName, StageDirectory);

        DeleteStage(StageDirectory);
        Directory.CreateDirectory(StageDirectory);

        PackageStager Stager = new(Directories.EngineRoot, StageDirectory, bIncludeSymbols);

        IBuildPlatform PlatformSupport = BuildPlatformRegistry.Get(PlatformValue);
        BuildOptions Options = BuildOptions.Load(Directories, Arguments);

        List<TargetType> Types = new() { TargetType.Editor };
        if (bIncludeGame)
        {
            Types.Add(TargetType.Game);
        }

        HashSet<string> StagedPlugins = new(StringComparer.OrdinalIgnoreCase);
        foreach (TargetType Type in Types)
        {
            // An assembler keeps the modules it resolved, so reusing one would carry editor modules into the game.
            TargetInfo Info = new(EngineTarget, Type, PlatformValue, ConfigurationValue, Directories, Options);
            BuildTarget Target = new TargetAssembler(Assembly, Directories, PlatformSupport).Assemble(EngineTarget, Info);
            StageTargetBinaries(Stager, Target);

            foreach (PluginDescriptor Plugin in Target.EnabledPlugins)
            {
                if (StagedPlugins.Add(Plugin.Name))
                {
                    Stager.CopyTree(Plugin.RootDirectory, ExcludedPluginFolder);
                }
            }
        }

        StageLauncher(Stager, new TargetAssembler(Assembly, Directories, PlatformSupport), Directories, PlatformValue, Options);
        StageRuntimeLibraries(Stager, Directories.BinariesDirectory(PlatformValue));

        Stager.CopyTree(Path.Combine(Directories.EngineRoot, "Engine", "Resources"));
        Stager.CopyTree(Path.Combine(Directories.EngineEditorDirectory, "Config"));
        Stager.CopyTree(Path.Combine(Directories.EngineEditorDirectory, "Resources"));
        Stager.CopyTree(Path.Combine(Directories.EngineRoot, "External", "DotNet", "runtime"));
        StageTemplates(Stager, Directories);

        // Last, so a packaged README or template file replaces the source tree's version of it.
        Stager.CopyTree(Path.Combine(Directories.EngineRoot, "Engine", "Build", "Package", "Overlay"), RelativeRoot: "");

        WriteInstalledBuildMarker(StageDirectory, Version, ConfigurationValue, PlatformValue, Directories);

        Log.Info("Staged {0} files, {1:F1} MB.", Stager.FileCount, Stager.ByteCount / (1024.0 * 1024.0));

        if (!Arguments.HasFlag("NoZip"))
        {
            string ZipPath = StageDirectory + ".zip";
            Log.Info("Compressing {0}", ZipPath);

            if (File.Exists(ZipPath))
            {
                File.Delete(ZipPath);
            }
            ZipFile.CreateFromDirectory(StageDirectory, ZipPath, CompressionLevel.Optimal, includeBaseDirectory: true);
            Log.Info("Package written to {0} ({1:F1} MB).", ZipPath, new FileInfo(ZipPath).Length / (1024.0 * 1024.0));
        }

        Log.Info("Total package time: {0:F2}s.", Timer.Elapsed.TotalSeconds);
        return 0;
    }

    private static CommandLine EnableEnginePlugins(CommandLine Arguments, RulesAssembly Assembly, BuildDirectories Directories)
    {
        IEnumerable<string> EnginePlugins = Assembly.Plugins
            .Where(Plugin => PathUtils.IsUnder(Plugin.RootDirectory, Directories.EngineRoot))
            .Select(Plugin => Plugin.Name);

        string? Requested = Arguments.GetString("EnablePlugin");
        IEnumerable<string> All = Requested is null ? EnginePlugins : EnginePlugins.Append(Requested);
        return Arguments.WithOption("EnablePlugin", string.Join(",", All));
    }

    // Earlier stages may hold read-only files, which Directory.Delete refuses.
    private static void DeleteStage(string StageDirectory)
    {
        if (!Directory.Exists(StageDirectory))
        {
            return;
        }

        foreach (string File in Directory.EnumerateFiles(StageDirectory, "*", SearchOption.AllDirectories))
        {
            System.IO.File.SetAttributes(File, FileAttributes.Normal);
        }
        Directory.Delete(StageDirectory, recursive: true);
    }

    private static async Task<int> BuildEverything(
        CommandLine Arguments,
        BuildDirectories Directories,
        RulesAssembly Assembly,
        BuildPlatform PlatformValue,
        BuildConfiguration ConfigurationValue,
        bool bIncludeGame,
        CancellationToken Cancellation)
    {
        List<(string Name, TargetType Type)> Targets = new()
        {
            (EngineTarget, TargetType.Editor),
            (LauncherTarget, TargetType.Program),
        };
        if (bIncludeGame)
        {
            // Standalone play launches these, and an installed build cannot compile them on demand.
            Targets.Add((EngineTarget, TargetType.Game));
        }

        foreach ((string Name, TargetType Type) in Targets)
        {
            Log.Info("Building {0} ({1} {2})", Name, Type, ConfigurationValue);

            int Result = await BuildMode.BuildTargetsAsync(
                new[] { Name }, Arguments, Directories, Assembly, Type, PlatformValue, ConfigurationValue, Cancellation)
                .ConfigureAwait(false);
            if (Result != 0)
            {
                return Result;
            }
        }

        return 0;
    }

    private static void StageTargetBinaries(PackageStager Stager, BuildTarget Target)
    {
        foreach (BuildModule Module in Target.Modules)
        {
            bool bProducesBinary = Module.BinaryType is ModuleBinaryType.SharedLibrary
                or ModuleBinaryType.ConsoleApplication
                or ModuleBinaryType.WindowedApplication;

            if (bProducesBinary && Module.OutputFile.Length > 0)
            {
                Stager.CopyBinary(Module.OutputFile);
            }
        }

        foreach (RuntimeDependency Dependency in Target.RuntimeDependencies)
        {
            string Staged = Path.Combine(Target.BinariesDirectory, Path.GetFileName(Dependency.SourcePath));
            if (File.Exists(Staged))
            {
                Stager.CopyFile(Staged);
            }
            else if (!Dependency.bOptional)
            {
                throw new BuildException($"Runtime dependency '{Staged}' is missing. Build {Target.Name} first.");
            }
        }

        // LuminaSharp brings its Roslyn and satellite assemblies, and scripts compile against all of them.
        foreach (ManagedProject Project in Target.Rules.ManagedProjects)
        {
            string ManagedDirectory = Path.GetDirectoryName(Project.OutputAssembly)!;
            if (!File.Exists(Project.OutputAssembly))
            {
                throw new BuildException($"Managed assembly '{Project.OutputAssembly}' is missing. Build {Target.Name} first.");
            }
            Stager.CopyTree(ManagedDirectory);
        }
    }

    private static void StageLauncher(
        PackageStager Stager, TargetAssembler Assembler, BuildDirectories Directories, BuildPlatform PlatformValue, BuildOptions Options)
    {
        TargetInfo Info = new(LauncherTarget, TargetType.Program, PlatformValue, BuildConfiguration.Development, Directories, Options);
        BuildTarget Target = Assembler.Assemble(LauncherTarget, Info);

        string Launcher = Target.LaunchModule?.OutputFile ?? string.Empty;
        if (!File.Exists(Launcher))
        {
            throw new BuildException($"The launcher '{Launcher}' is missing. Build {LauncherTarget} first.");
        }

        Stager.CopyFileAs(Launcher, LauncherFileName);
    }

    // The editor links the dynamic MSVC runtime, which a designer's machine may not have installed.
    private static void StageRuntimeLibraries(PackageStager Stager, string BinariesDirectory)
    {
        string? CrtDirectory = FindRedistributableCrt();
        if (CrtDirectory is null)
        {
            Log.Warning("No MSVC redistributable runtime was found. The package will need the Visual C++ runtime installed.");
            return;
        }

        foreach (string Library in Directory.EnumerateFiles(CrtDirectory, "*.dll"))
        {
            Stager.CopyFileTo(Library, Path.Combine(BinariesDirectory, Path.GetFileName(Library)));
        }
    }

    private static string? FindRedistributableCrt()
    {
        string RedistRoot = Path.Combine(VisualStudioLocator.Locate().InstallationPath, "VC", "Redist", "MSVC");
        if (!Directory.Exists(RedistRoot))
        {
            return null;
        }

        IEnumerable<string> Versions = Directory.EnumerateDirectories(RedistRoot)
            .Where(Candidate => Version.TryParse(Path.GetFileName(Candidate), out _))
            .OrderByDescending(Candidate => Version.Parse(Path.GetFileName(Candidate)));

        foreach (string VersionDirectory in Versions)
        {
            string Architecture = Path.Combine(VersionDirectory, "x64");
            if (!Directory.Exists(Architecture))
            {
                continue;
            }

            string? Crt = Directory.EnumerateDirectories(Architecture, "Microsoft.VC*.CRT").FirstOrDefault();
            if (Crt is not null)
            {
                return Crt;
            }
        }

        return null;
    }

    // A designer has no toolchain, so the project template ships without its C++ module and generators.
    private static void StageTemplates(PackageStager Stager, BuildDirectories Directories)
    {
        string Templates = Path.Combine(Directories.EngineRoot, "Templates");
        string Blank = Path.Combine(Templates, "Blank");

        Stager.CopyTree(Templates, Relative =>
        {
            // The plugin template is a pair of C++ modules, and the editor offers it only in source builds.
            if (!PathUtils.IsUnder(Path.Combine(Templates, Relative), Blank))
            {
                return true;
            }

            string WithinBlank = Path.GetRelativePath(Blank, Path.Combine(Templates, Relative));
            string First = WithinBlank.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)[0];
            return First.Equals("Source", StringComparison.OrdinalIgnoreCase)
                || First.StartsWith("GenerateProject.", StringComparison.OrdinalIgnoreCase);
        });
    }

    private static bool ExcludedPluginFolder(string Relative)
    {
        string First = Relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)[0];

        // Plugin binaries arrive through their modules' own output files, in the configuration packaged.
        return First.Equals("Source", StringComparison.OrdinalIgnoreCase)
            || First.Equals("Binaries", StringComparison.OrdinalIgnoreCase);
    }

    private static string ReadEngineVersion(BuildDirectories Directories)
    {
        string Header = File.ReadAllText(Path.Combine(Directories.EngineRuntimeDirectory, "Source", "Lumina.h"));

        string Part(string Name)
        {
            Match Found = Regex.Match(Header, $@"#define\s+LUMINA_VERSION_{Name}\s+(\d+)");
            if (!Found.Success)
            {
                throw new BuildException($"LUMINA_VERSION_{Name} was not found in Lumina.h.");
            }
            return Found.Groups[1].Value;
        }

        return $"{Part("MAJOR")}.{Part("MINOR")}.{Part("PATCH")}";
    }

    private static void WriteInstalledBuildMarker(
        string StageDirectory, string Version, BuildConfiguration ConfigurationValue, BuildPlatform PlatformValue, BuildDirectories Directories)
    {
        Dictionary<string, string> Marker = new()
        {
            ["Version"] = Version,
            ["Configuration"] = ConfigurationValue.ToString(),
            ["Platform"] = PlatformValue.GetOutputDirectoryName(),
            ["Commit"] = ReadCommit(Directories.EngineRoot),
            ["PackagedAt"] = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ"),
        };

        string MarkerPath = Path.Combine(StageDirectory, InstalledBuildMarker);
        PathUtils.EnsureDirectoryForFile(MarkerPath);
        File.WriteAllText(MarkerPath, JsonSerializer.Serialize(Marker, new JsonSerializerOptions { WriteIndented = true }));
    }

    private static string ReadCommit(string EngineRoot)
    {
        try
        {
            ProcessStartInfo Start = new("git", "rev-parse --short HEAD")
            {
                WorkingDirectory = EngineRoot,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };

            using Process? Git = Process.Start(Start);
            if (Git is null)
            {
                return string.Empty;
            }

            string Output = Git.StandardOutput.ReadToEnd().Trim();
            Git.WaitForExit();
            return Git.ExitCode == 0 ? Output : string.Empty;
        }
        catch (Exception)
        {
            return string.Empty;
        }
    }

    private sealed class PackageStager
    {
        private readonly string EngineRoot;
        private readonly string StageRoot;
        private readonly bool bIncludeSymbols;
        private readonly HashSet<string> Staged = new(StringComparer.OrdinalIgnoreCase);

        public PackageStager(string EngineRoot, string StageRoot, bool bIncludeSymbols)
        {
            this.EngineRoot = Path.GetFullPath(EngineRoot);
            this.StageRoot = StageRoot;
            this.bIncludeSymbols = bIncludeSymbols;
        }

        public int FileCount => Staged.Count;

        public long ByteCount { get; private set; }

        public void CopyBinary(string SourcePath)
        {
            CopyFile(SourcePath);

            string Symbols = Path.ChangeExtension(SourcePath, ".pdb");
            if (bIncludeSymbols && File.Exists(Symbols))
            {
                CopyFile(Symbols);
            }
        }

        public void CopyFile(string SourcePath)
        {
            CopyFileTo(SourcePath, SourcePath);
        }

        // Places the file where DestinationLike would sit in the stage, for a file staged under another name or place.
        public void CopyFileTo(string SourcePath, string DestinationLike)
        {
            string Full = Path.GetFullPath(DestinationLike);
            if (!PathUtils.IsUnder(Full, EngineRoot))
            {
                throw new BuildException($"'{Full}' is outside the engine root, so it has no place in the package.");
            }

            Write(SourcePath, Path.GetRelativePath(EngineRoot, Full));
        }

        public void CopyFileAs(string SourcePath, string RelativeDestination)
        {
            Write(SourcePath, RelativeDestination);
        }

        public void CopyTree(string SourceDirectory, Func<string, bool>? bExclude = null, string? RelativeRoot = null)
        {
            if (!Directory.Exists(SourceDirectory))
            {
                return;
            }

            string Full = Path.GetFullPath(SourceDirectory);
            string Prefix = RelativeRoot ?? Path.GetRelativePath(EngineRoot, Full);

            foreach (string File in Directory.EnumerateFiles(Full, "*", SearchOption.AllDirectories))
            {
                string Relative = Path.GetRelativePath(Full, File);
                if (IsJunk(Relative) || (bExclude?.Invoke(Relative) ?? false))
                {
                    continue;
                }

                if (!bIncludeSymbols && Path.GetExtension(File).Equals(".pdb", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                Write(File, Path.Combine(Prefix, Relative));
            }
        }

        private static bool IsJunk(string Relative)
        {
            string[] Parts = Relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            for (int Index = 0; Index < Parts.Length - 1; ++Index)
            {
                if (ExcludedDirectoryNames.Contains(Parts[Index]))
                {
                    return true;
                }
            }
            return ExcludedExtensions.Contains(Path.GetExtension(Relative));
        }

        private void Write(string SourcePath, string RelativeDestination)
        {
            string Destination = Path.Combine(StageRoot, RelativeDestination);
            if (!Staged.Add(Destination))
            {
                return;
            }

            PathUtils.EnsureDirectoryForFile(Destination);
            System.IO.File.Copy(SourcePath, Destination, overwrite: true);

            // The .NET runtime ships read-only, and a designer should be able to delete or replace the folder.
            System.IO.File.SetAttributes(Destination, FileAttributes.Normal);
            ByteCount += new FileInfo(Destination).Length;
        }
    }
}
