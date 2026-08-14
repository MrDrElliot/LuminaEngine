using System.Security.Cryptography;
using System.Text;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Toolchain;

namespace LuminaBuildTool.ProjectFiles.VisualStudio;

/// <summary>Generates NMake-style vcxproj files plus a solution.</summary>
public sealed class VisualStudioGenerator : IProjectFileGenerator
{
    /// <summary>MSBuild's own platform axis.</summary>
    private const string PlatformName = "x64";

    private readonly IToolchain Toolchain;

    public VisualStudioGenerator(IToolchain Toolchain)
    {
        this.Toolchain = Toolchain;
    }

    public string Name => "VisualStudio";

    public int Generate(
        BuildDirectories Directories,
        IReadOnlyList<ProjectTargetInfo> Targets,
        IReadOnlyList<ProjectConfiguration> Configurations,
        string RulesProjectPath)
    {
        string ProjectsDirectory = Directories.ProjectFilesDirectory;
        PathUtils.EnsureDirectory(ProjectsDirectory);

        int Changed = 0;
        List<GeneratedProject> Projects = new();

        // One buildable project per target, so a solution build never drives one output set from several.
        foreach (ProjectTargetInfo Target in Targets)
        {
            Projects.Add(new GeneratedProject
            {
                ProjectName = Target.TargetName,
                OwningTargetName = Target.TargetName,
                Module = Target.PrimaryVariant.LaunchModule,
                Target = Target,
                FilePath = Path.Combine(ProjectsDirectory, Target.TargetName + ".vcxproj"),
                Guid = MakeDeterministicGuid("Target:" + Target.TargetName),
                SolutionFolder = ResolveTargetSolutionFolder(Directories, Target),
                bBuildable = true,
                bBuildByDefault = Target.PrimaryVariant.Rules.bBuildByDefault,
                bIsStartup = IsStartupTarget(Directories, Target),
            });
        }

        // Module projects browse only; giving them the build command ran one tool instance per module.
        Dictionary<string, GeneratedProject> ProjectsByModule = new(StringComparer.OrdinalIgnoreCase);

        // A target is named after its launch module, so that module is already represented by the
        // target's own project. Emitting both would put two projects at the same path.
        HashSet<string> TakenNames = new(Targets.Select(T => T.TargetName), StringComparer.OrdinalIgnoreCase);

        foreach (ProjectTargetInfo Target in Targets)
        {
            foreach (BuildModule Module in Target.PrimaryVariant.Modules)
            {
                if (!TakenNames.Add(Module.Name))
                {
                    continue;
                }

                GeneratedProject Project = new()
                {
                    ProjectName = Module.Name,
                    OwningTargetName = Target.TargetName,
                    Module = Module,
                    Target = Target,
                    FilePath = Path.Combine(ProjectsDirectory, Module.Name + ".vcxproj"),
                    Guid = MakeDeterministicGuid(Module.Name),
                    SolutionFolder = ResolveSolutionFolder(Directories, Module),
                    bBuildable = false,
                };

                ProjectsByModule[Module.Name] = Project;
                Projects.Add(Project);
            }
        }

        foreach (GeneratedProject Project in Projects.OrderBy(P => P.ProjectName, StringComparer.OrdinalIgnoreCase))
        {
            if (PathUtils.WriteFileIfChanged(Project.FilePath, BuildProjectXml(Directories, Project, Configurations)))
            {
                Changed++;
            }

            if (PathUtils.WriteFileIfChanged(Project.FilePath + ".filters", BuildFiltersXml(Project)))
            {
                Changed++;
            }

            if (Project.bBuildable
                && PathUtils.WriteFileIfChanged(Project.FilePath + ".user", BuildUserXml(Directories, Project, Configurations)))
            {
                Changed++;
            }
        }

        string SolutionName = Directories.ProjectRoot is null
            ? "Lumina"
            : Path.GetFileName(Directories.ProjectRoot);

        string SolutionPath = Path.Combine(Directories.OutputRoot, SolutionName + ".sln");

        if (PathUtils.WriteFileIfChanged(SolutionPath, BuildSolution(Projects, Configurations, SolutionPath, RulesProjectPath)))
        {
            Changed++;
        }

        Log.Info(
            "Generated {0} target projects and {1} module projects at '{2}'.",
            Targets.Count,
            ProjectsByModule.Count,
            SolutionPath);

        return Changed;
    }

    private sealed class GeneratedProject
    {
        public required string ProjectName { get; init; }

        public required string OwningTargetName { get; init; }

        /// <summary>Module whose sources and IntelliSense settings this project shows.</summary>
        public required BuildModule? Module { get; init; }

        /// <summary>Whether building this project actually runs the build tool.</summary>
        public required bool bBuildable { get; init; }

        /// <summary>Whether a whole-solution build includes it.</summary>
        public bool bBuildByDefault { get; init; }

        /// <summary>Whether this is the target the IDE runs by default.</summary>
        public bool bIsStartup { get; init; }

        public required ProjectTargetInfo Target { get; init; }

        public required string FilePath { get; init; }

        public required Guid Guid { get; init; }

        public required string SolutionFolder { get; init; }
    }

    /// <summary>Groups projects as the source tree does, with the game project's own under Games/&lt;Project&gt;.</summary>
    private static string ResolveSolutionFolder(BuildDirectories Directories, BuildModule Module)
    {
        string? GameFolder = ResolveGameFolder(Directories, Module.Rules.ModuleDirectory);

        if (Module.bIsPlugin)
        {
            // A plugin under the project belongs to the project, not beside the engine's.
            return GameFolder is not null
                ? $"{GameFolder}/Plugins/{Module.Rules.PluginName}"
                : "Plugins/" + Module.Rules.PluginName;
        }

        if (GameFolder is not null)
        {
            return GameFolder + "/Source";
        }

        if (PathUtils.IsUnder(Module.Rules.ModuleDirectory, Directories.ThirdPartyDirectory))
        {
            return "ThirdParty";
        }

        if (PathUtils.IsUnder(Module.Rules.ModuleDirectory, Directories.EngineApplicationsDirectory))
        {
            return "Applications";
        }

        return "Engine";
    }

    /// <summary>Whether this is the target the IDE should start with.</summary>
    private static bool IsStartupTarget(BuildDirectories Directories, ProjectTargetInfo Target)
    {
        if (Directories.ProjectRoot is null)
        {
            return Target.PrimaryVariant.Rules.bIsStartupTarget;
        }

        return PathUtils.IsUnder(Target.PrimaryVariant.Rules.RulesDirectory, Directories.ProjectRoot);
    }

    /// <summary>Solution folder for a target's own project.</summary>
    private static string ResolveTargetSolutionFolder(BuildDirectories Directories, ProjectTargetInfo Target)
    {
        string? GameFolder = ResolveGameFolder(Directories, Target.PrimaryVariant.Rules.RulesDirectory);

        return GameFolder is not null ? GameFolder + "/Source" : "Targets";
    }

    /// <summary>"Games/&lt;ProjectName&gt;" when a path belongs to the game project being built, else null.</summary>
    private static string? ResolveGameFolder(BuildDirectories Directories, string Location)
    {
        if (Directories.ProjectRoot is null || !PathUtils.IsUnder(Location, Directories.ProjectRoot))
        {
            return null;
        }

        return "Games/" + Path.GetFileName(PathUtils.Normalize(Directories.ProjectRoot));
    }

    // vcxproj.

    private string BuildProjectXml(
        BuildDirectories Directories,
        GeneratedProject Project,
        IReadOnlyList<ProjectConfiguration> Configurations)
    {
        BuildModule? Module = Project.Module;

        StringBuilder Xml = new();
        Xml.AppendLine("""<?xml version="1.0" encoding="utf-8"?>""");
        Xml.AppendLine("""<Project DefaultTargets="Build" ToolsVersion="Current" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">""");

        Xml.AppendLine("""  <ItemGroup Label="ProjectConfigurations">""");

        foreach (ProjectConfiguration Configuration in Configurations)
        {
            Xml.AppendLine($"""    <ProjectConfiguration Include="{Configuration.DisplayName}|{PlatformName}">""");
            Xml.AppendLine($"      <Configuration>{Configuration.DisplayName}</Configuration>");
            Xml.AppendLine($"      <Platform>{PlatformName}</Platform>");
            Xml.AppendLine("    </ProjectConfiguration>");
        }

        Xml.AppendLine("  </ItemGroup>");

        Xml.AppendLine("""  <PropertyGroup Label="Globals">""");
        Xml.AppendLine($"    <ProjectGuid>{{{Project.Guid.ToString().ToUpperInvariant()}}}</ProjectGuid>");
        Xml.AppendLine($"    <RootNamespace>{Escape(Project.ProjectName)}</RootNamespace>");
        Xml.AppendLine("    <Keyword>MakeFileProj</Keyword>");
        Xml.AppendLine("    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>");
        Xml.AppendLine("  </PropertyGroup>");

        Xml.AppendLine("""  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />""");

        foreach (ProjectConfiguration Configuration in Configurations)
        {
            Xml.AppendLine($"""  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='{Configuration.DisplayName}|{PlatformName}'" Label="Configuration">""");
            Xml.AppendLine("    <ConfigurationType>Makefile</ConfigurationType>");
            Xml.AppendLine($"    <PlatformToolset>{Toolchain.ProjectToolsetName}</PlatformToolset>");
            Xml.AppendLine("  </PropertyGroup>");
        }

        Xml.AppendLine("""  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />""");

        string ToolInvocation = BuildToolInvocation(Directories);
        string ToolGuard = BuildToolGuard(Directories) + Environment.NewLine;

        foreach (ProjectConfiguration Configuration in Configurations)
        {
            BuildTarget? Variant = Project.Target.Variants.GetValueOrDefault(Configuration) ?? Project.Target.PrimaryVariant;
            BuildModule? VariantModule = Module is null ? null : Variant.FindModule(Module.Name) ?? Module;

            string CommonArguments =
                $"{Project.OwningTargetName} -Platform={Variant.Info.Platform} "
                + $"-Configuration={Configuration.Configuration} -TargetType={Configuration.Type}";

            Xml.AppendLine($"""  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='{Configuration.DisplayName}|{PlatformName}'">""");

            // MSBuild creates both anyway and would otherwise litter the repo root with an empty x64/<Config>.
            string MsBuildScratch = Path.Combine(
                Directories.ProjectFilesDirectory, "MSBuild", Project.ProjectName, Configuration.DisplayName);

            Xml.AppendLine($"    <OutDir>{Escape(MsBuildScratch)}\\</OutDir>");
            Xml.AppendLine($"    <IntDir>{Escape(MsBuildScratch)}\\</IntDir>");

            // The IDE's own up-to-date check knows nothing about what this build actually reads,
            // so left on it will decide a source edit changed nothing and never invoke the tool.
            Xml.AppendLine("    <DisableFastUpToDateCheck>true</DisableFastUpToDateCheck>");

            if (Project.bBuildable)
            {
                Xml.AppendLine($"    <NMakeBuildCommandLine>{Escape($"{ToolGuard}{ToolInvocation} Build {CommonArguments}")}</NMakeBuildCommandLine>");
                Xml.AppendLine($"    <NMakeReBuildCommandLine>{Escape($"{ToolGuard}{ToolInvocation} Build {CommonArguments} -Clean")}</NMakeReBuildCommandLine>");
                Xml.AppendLine($"    <NMakeCleanCommandLine>{Escape($"{ToolGuard}{ToolInvocation} Clean {CommonArguments}")}</NMakeCleanCommandLine>");
            }
            else
            {
                // Source and IntelliSense only. Building this project would run a second copy of
                // the same target's build; the target's own project is the one to build.
                Xml.AppendLine(
                    "    <NMakeBuildCommandLine>"
                    + Escape($"echo {Project.ProjectName} is built as part of the {Project.OwningTargetName} target.")
                    + "</NMakeBuildCommandLine>");
                Xml.AppendLine(
                    "    <NMakeReBuildCommandLine>"
                    + Escape($"echo {Project.ProjectName} is built as part of the {Project.OwningTargetName} target.")
                    + "</NMakeReBuildCommandLine>");
                Xml.AppendLine("    <NMakeCleanCommandLine></NMakeCleanCommandLine>");
            }

            if (VariantModule is not null)
            {
                // Rider makes any project with an NMakeOutput a run configuration, so only targets name one.
                if (Project.bBuildable)
                {
                    Xml.AppendLine($"    <NMakeOutput>{Escape(VariantModule.OutputFile)}</NMakeOutput>");
                }

                Xml.AppendLine($"    <NMakePreprocessorDefinitions>{Escape(string.Join(';', VariantModule.CompileDefinitions))}</NMakePreprocessorDefinitions>");
                Xml.AppendLine($"    <NMakeIncludeSearchPath>{Escape(string.Join(';', VariantModule.CompileIncludePaths))}</NMakeIncludeSearchPath>");
                Xml.AppendLine($"    <NMakeForcedIncludes>{Escape(string.Join(';', VariantModule.ForceIncludeFiles))}</NMakeForcedIncludes>");
            }

            // Matches the compile: USING(flag) expands differently under the legacy preprocessor.
            Xml.AppendLine($"    <AdditionalOptions>/std:{Variant.Rules.CppStandard} /Zc:__cplusplus /Zc:preprocessor</AdditionalOptions>");

            if (Project.bBuildable)
            {
                AppendDebuggerSettings(Xml, Directories, Variant, VariantModule);
            }

            Xml.AppendLine("  </PropertyGroup>");
        }

        AppendSourceItems(Xml, Project);

        Xml.AppendLine("""  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />""");
        Xml.AppendLine("</Project>");

        return Xml.ToString();
    }

    /// <summary>What the IDE runs for this target.</summary>
    private static void AppendDebuggerSettings(
        StringBuilder Xml,
        BuildDirectories Directories,
        BuildTarget Variant,
        BuildModule? LaunchModule)
    {
        string Command = ResolveDebuggerCommand(Variant, LaunchModule);

        if (Command.Length == 0)
        {
            return;
        }

        string WorkingDirectory = Variant.Rules.DebuggerWorkingDirectory.Length > 0
            ? Variant.Rules.DebuggerWorkingDirectory
            : Directories.EngineRoot;

        Xml.AppendLine($"    <LocalDebuggerCommand>{Escape(Command)}</LocalDebuggerCommand>");
        Xml.AppendLine($"    <LocalDebuggerWorkingDirectory>{Escape(WorkingDirectory)}</LocalDebuggerWorkingDirectory>");

        if (Variant.Rules.DebuggerArguments.Length > 0)
        {
            Xml.AppendLine($"    <LocalDebuggerCommandArguments>{Escape(Variant.Rules.DebuggerArguments)}</LocalDebuggerCommandArguments>");
        }

        Xml.AppendLine("    <LocalDebuggerAttach>false</LocalDebuggerAttach>");
        Xml.AppendLine("    <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>");
    }

    private static string ResolveDebuggerCommand(BuildTarget Variant, BuildModule? LaunchModule)
    {
        if (Variant.Rules.DebuggerCommand.Length > 0)
        {
            return Variant.Rules.DebuggerCommand;
        }

        return LaunchModule is not null && LaunchModule.BinaryType.ProducesExecutable()
            ? LaunchModule.OutputFile
            : string.Empty;
    }

    /// <summary>Debugger settings live in a .user file, which is what Visual Studio and Rider read and write.</summary>
    private static string BuildUserXml(
        BuildDirectories Directories,
        GeneratedProject Project,
        IReadOnlyList<ProjectConfiguration> Configurations)
    {
        StringBuilder Xml = new();
        Xml.AppendLine("""<?xml version="1.0" encoding="utf-8"?>""");
        Xml.AppendLine("""<Project ToolsVersion="Current" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">""");

        foreach (ProjectConfiguration Configuration in Configurations)
        {
            BuildTarget Variant = Project.Target.Variants.GetValueOrDefault(Configuration) ?? Project.Target.PrimaryVariant;
            BuildModule? VariantModule = Project.Module is null
                ? null
                : Variant.FindModule(Project.Module.Name) ?? Project.Module;

            Xml.AppendLine($"""  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='{Configuration.DisplayName}|{PlatformName}'">""");
            AppendDebuggerSettings(Xml, Directories, Variant, VariantModule);
            Xml.AppendLine("  </PropertyGroup>");
        }

        Xml.AppendLine("</Project>");

        return Xml.ToString();
    }

    private static void AppendSourceItems(StringBuilder Xml, GeneratedProject Project)
    {
        BuildModule? Module = Project.Module;

        if (Module is null)
        {
            // A target with no launch module still needs its rules file listed so the project is
            // not empty in the IDE.
            Xml.AppendLine("  <ItemGroup>");
            Xml.AppendLine($"""    <None Include="{Escape(Project.Target.PrimaryVariant.Rules.RulesFile)}" />""");
            Xml.AppendLine("  </ItemGroup>");
            return;
        }

        Xml.AppendLine("  <ItemGroup>");

        foreach (FileItem Source in Module.Sources.CppFiles.Concat(Module.Sources.CFiles).OrderBy(F => F.Location, StringComparer.OrdinalIgnoreCase))
        {
            Xml.AppendLine($"""    <ClCompile Include="{Escape(Source.Location)}" />""");
        }

        Xml.AppendLine("  </ItemGroup>");
        Xml.AppendLine("  <ItemGroup>");

        foreach (FileItem Header in Module.Sources.HeaderFiles.OrderBy(F => F.Location, StringComparer.OrdinalIgnoreCase))
        {
            Xml.AppendLine($"""    <ClInclude Include="{Escape(Header.Location)}" />""");
        }

        Xml.AppendLine("  </ItemGroup>");
        Xml.AppendLine("  <ItemGroup>");

        Xml.AppendLine($"""    <None Include="{Escape(Module.Rules.RulesFile)}" />""");

        foreach (FileItem Resource in Module.Sources.ResourceFiles.OrderBy(F => F.Location, StringComparer.OrdinalIgnoreCase))
        {
            Xml.AppendLine($"""    <None Include="{Escape(Resource.Location)}" />""");
        }

        Xml.AppendLine("  </ItemGroup>");
    }

    // vcxproj.filters, mirroring the on-disk directory layout.

    private static string BuildFiltersXml(GeneratedProject Project)
    {
        BuildModule? Module = Project.Module;

        if (Module is null)
        {
            return """
                <?xml version="1.0" encoding="utf-8"?>
                <Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
                </Project>
                """;
        }

        string Root = Module.Rules.ResolveSourceRoot();

        SortedSet<string> Filters = new(StringComparer.OrdinalIgnoreCase);
        List<(string Element, FileItem Item, string Filter)> Entries = new();

        void Add(string Element, IEnumerable<FileItem> Items)
        {
            foreach (FileItem Item in Items)
            {
                string Filter = ResolveFilter(Item, Root);

                if (Filter.Length > 0)
                {
                    // Every ancestor filter must be declared or VS drops the leaf.
                    string Accumulated = string.Empty;

                    foreach (string Segment in Filter.Split('\\', StringSplitOptions.RemoveEmptyEntries))
                    {
                        Accumulated = Accumulated.Length == 0 ? Segment : Accumulated + "\\" + Segment;
                        Filters.Add(Accumulated);
                    }
                }

                Entries.Add((Element, Item, Filter));
            }
        }

        Add("ClCompile", Module.Sources.CppFiles.Concat(Module.Sources.CFiles));
        Add("ClInclude", Module.Sources.HeaderFiles);
        Add("None", Module.Sources.ResourceFiles);

        StringBuilder Xml = new();
        Xml.AppendLine("""<?xml version="1.0" encoding="utf-8"?>""");
        Xml.AppendLine("""<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">""");

        Xml.AppendLine("  <ItemGroup>");

        foreach (string Filter in Filters)
        {
            Xml.AppendLine($"""    <Filter Include="{Escape(Filter)}">""");
            Xml.AppendLine($"      <UniqueIdentifier>{{{MakeDeterministicGuid(Project.ProjectName + "/" + Filter).ToString().ToUpperInvariant()}}}</UniqueIdentifier>");
            Xml.AppendLine("    </Filter>");
        }

        Xml.AppendLine("  </ItemGroup>");
        Xml.AppendLine("  <ItemGroup>");

        foreach ((string Element, FileItem Item, string Filter) in Entries.OrderBy(E => E.Item.Location, StringComparer.OrdinalIgnoreCase))
        {
            if (Filter.Length == 0)
            {
                Xml.AppendLine($"""    <{Element} Include="{Escape(Item.Location)}" />""");
                continue;
            }

            Xml.AppendLine($"""    <{Element} Include="{Escape(Item.Location)}">""");
            Xml.AppendLine($"      <Filter>{Escape(Filter)}</Filter>");
            Xml.AppendLine($"    </{Element}>");
        }

        Xml.AppendLine("  </ItemGroup>");
        Xml.AppendLine("</Project>");

        return Xml.ToString();
    }

    private static string ResolveFilter(FileItem Item, string Root)
    {
        string Relative = PathUtils.MakeRelativeTo(Item.Directory, Root);

        if (Relative == "." || Relative.StartsWith("..", StringComparison.Ordinal) || Path.IsPathRooted(Relative))
        {
            return string.Empty;
        }

        return Relative.Replace('/', '\\');
    }

    // Solution.

    private static string BuildSolution(
        IEnumerable<GeneratedProject> Projects,
        IReadOnlyList<ProjectConfiguration> Configurations,
        string SolutionPath,
        string RulesProjectPath)
    {
        const string VcxProjTypeGuid = "8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942";
        const string FolderTypeGuid = "2150E333-8FDC-42A3-9474-1A3956D46DE8";
        const string CSharpProjTypeGuid = "9A19103F-16F7-4668-BE54-9A1E7A4F7556";

        string SolutionDirectory = Path.GetDirectoryName(SolutionPath)!;

        // A .sln has no startup-project field; the IDE picks the first listed, so the startup target goes first.
        List<GeneratedProject> Ordered = Projects
            .OrderByDescending(P => P.bIsStartup)
            .ThenByDescending(P => P.bBuildable)
            .ThenBy(P => P.ProjectName, StringComparer.OrdinalIgnoreCase)
            .ToList();

        Dictionary<string, Guid> FolderGuids = new(StringComparer.OrdinalIgnoreCase);

        foreach (GeneratedProject Project in Ordered)
        {
            // Declare each level of a nested folder path so the solution tree resolves.
            string Accumulated = string.Empty;

            foreach (string Segment in Project.SolutionFolder.Split('/', StringSplitOptions.RemoveEmptyEntries))
            {
                Accumulated = Accumulated.Length == 0 ? Segment : Accumulated + "/" + Segment;
                FolderGuids.TryAdd(Accumulated, MakeDeterministicGuid("Folder:" + Accumulated));
            }
        }

        StringBuilder Solution = new();
        Solution.AppendLine();
        Solution.AppendLine("Microsoft Visual Studio Solution File, Format Version 12.00");
        Solution.AppendLine("# Visual Studio Version 17");

        foreach ((string FolderPath, Guid FolderGuid) in FolderGuids.OrderBy(P => P.Key, StringComparer.OrdinalIgnoreCase))
        {
            string DisplayName = FolderPath.Split('/').Last();
            Solution.AppendLine($"Project(\"{{{FolderTypeGuid}}}\") = \"{DisplayName}\", \"{DisplayName}\", \"{{{FolderGuid.ToString().ToUpperInvariant()}}}\"");
            Solution.AppendLine("EndProject");
        }

        foreach (GeneratedProject Project in Ordered)
        {
            string Relative = PathUtils.MakeRelativeTo(Project.FilePath, SolutionDirectory);
            Solution.AppendLine($"Project(\"{{{VcxProjTypeGuid}}}\") = \"{Project.ProjectName}\", \"{Relative}\", \"{{{Project.Guid.ToString().ToUpperInvariant()}}}\"");
            Solution.AppendLine("EndProject");
        }

        // The rules project is here purely so the IDE resolves Build.cs and Target.cs files.
        Guid RulesGuid = MakeDeterministicGuid("Project:" + RulesProjectGenerator.ProjectName);

        if (RulesProjectPath.Length > 0)
        {
            string Relative = PathUtils.MakeRelativeTo(RulesProjectPath, SolutionDirectory);
            Solution.AppendLine($"Project(\"{{{CSharpProjTypeGuid}}}\") = \"{RulesProjectGenerator.ProjectName}\", \"{Relative}\", \"{{{RulesGuid.ToString().ToUpperInvariant()}}}\"");
            Solution.AppendLine("EndProject");
        }

        Solution.AppendLine("Global");
        Solution.AppendLine("\tGlobalSection(SolutionConfigurationPlatforms) = preSolution");

        foreach (ProjectConfiguration Configuration in Configurations)
        {
            Solution.AppendLine($"\t\t{Configuration.DisplayName}|{PlatformName} = {Configuration.DisplayName}|{PlatformName}");
        }

        Solution.AppendLine("\tEndGlobalSection");
        Solution.AppendLine("\tGlobalSection(ProjectConfigurationPlatforms) = postSolution");

        foreach (GeneratedProject Project in Ordered)
        {
            string ProjectGuid = "{" + Project.Guid.ToString().ToUpperInvariant() + "}";

            foreach (ProjectConfiguration Configuration in Configurations)
            {
                string Key = $"{Configuration.DisplayName}|{PlatformName}";
                Solution.AppendLine($"\t\t{ProjectGuid}.{Key}.ActiveCfg = {Key}");

                // Only buildable, build-by-default projects get a Build entry; the rest stay individually buildable.
                if (Project.bBuildable && Project.bBuildByDefault)
                {
                    Solution.AppendLine($"\t\t{ProjectGuid}.{Key}.Build.0 = {Key}");
                }
            }
        }

        if (RulesProjectPath.Length > 0)
        {
            // Left out of the build: the build system compiles these rules files itself.
            string RulesProjectGuid = "{" + RulesGuid.ToString().ToUpperInvariant() + "}";

            foreach (ProjectConfiguration Configuration in Configurations)
            {
                Solution.AppendLine(
                    $"\t\t{RulesProjectGuid}.{Configuration.DisplayName}|{PlatformName}.ActiveCfg = Debug|Any CPU");
            }
        }

        Solution.AppendLine("\tEndGlobalSection");
        Solution.AppendLine("\tGlobalSection(SolutionProperties) = preSolution");
        Solution.AppendLine("\t\tHideSolutionNode = FALSE");
        Solution.AppendLine("\tEndGlobalSection");
        Solution.AppendLine("\tGlobalSection(NestedProjects) = preSolution");

        foreach ((string FolderPath, Guid FolderGuid) in FolderGuids)
        {
            int Split = FolderPath.LastIndexOf('/');

            if (Split > 0 && FolderGuids.TryGetValue(FolderPath.Substring(0, Split), out Guid ParentGuid))
            {
                Solution.AppendLine($"\t\t{{{FolderGuid.ToString().ToUpperInvariant()}}} = {{{ParentGuid.ToString().ToUpperInvariant()}}}");
            }
        }

        foreach (GeneratedProject Project in Ordered)
        {
            if (FolderGuids.TryGetValue(Project.SolutionFolder, out Guid FolderGuid))
            {
                Solution.AppendLine($"\t\t{{{Project.Guid.ToString().ToUpperInvariant()}}} = {{{FolderGuid.ToString().ToUpperInvariant()}}}");
            }
        }

        Solution.AppendLine("\tEndGlobalSection");
        Solution.AppendLine("EndGlobal");

        return Solution.ToString();
    }

    /// <summary>Command that reinvokes this tool.</summary>
    private static string BuildToolInvocation(BuildDirectories Directories)
    {
        string ToolPath = Environment.ProcessPath ?? string.Empty;
        string Roots = $"-EngineRoot={PathUtils.Quote(Directories.EngineRoot)}";

        if (Directories.ProjectRoot is not null)
        {
            Roots += $" -Project={PathUtils.Quote(Directories.ProjectRoot)}";
        }

        if (ToolPath.EndsWith("dotnet.exe", StringComparison.OrdinalIgnoreCase))
        {
            return $"{PathUtils.Quote(ToolPath)} {PathUtils.Quote(BuildToolAssemblyPath())} {Roots}";
        }

        return $"{PathUtils.Quote(ToolPath)} {Roots}";
    }

    /// <summary>The file whose absence stops a generated project from building anything.</summary>
    private static string BuildToolAssemblyPath()
    {
        string ToolPath = Environment.ProcessPath ?? string.Empty;

        return ToolPath.EndsWith("dotnet.exe", StringComparison.OrdinalIgnoreCase)
            ? System.Reflection.Assembly.GetExecutingAssembly().Location
            : ToolPath;
    }

    /// <summary>Batch prefix that fails an IDE build with something actionable when the build tool is missing.</summary>
    private static string BuildToolGuard(BuildDirectories Directories)
    {
        string ToolPath = BuildToolAssemblyPath();
        string Script = Path.Combine(Directories.EngineRoot, "LuminaBuild.bat");
        string ToolProject = Path.Combine(
            Directories.EngineRoot, "Engine", "Tools", "LuminaBuildTool", "LuminaBuildTool.csproj");

        string Test = $"if not exist \"{ToolPath}\" ";

        // Rebuild first: a stale tool fails the rules against an API that does not exist yet, and blocks Clean.
        return $"dotnet build \"{ToolProject}\" -v quiet --nologo"
            + Environment.NewLine
            + "if errorlevel 1 echo error: LuminaBuildTool failed to build; the engine tree and the build"
            + " tool are out of step."
            + Environment.NewLine
            + "if errorlevel 1 exit /b 1"
            + Environment.NewLine
            + Test
            + $"echo error: LuminaBuildTool is missing at \"{ToolPath}\"."
            + $" Run \"{Script}\" once to rebuild it, then regenerate project files."
            + Environment.NewLine
            + Test
            + "exit /b 1";
    }

    /// <summary>Stable GUID from a name, so regenerating projects never churns the solution.</summary>
    private static Guid MakeDeterministicGuid(string Name)
    {
        byte[] Digest = MD5.HashData(Encoding.UTF8.GetBytes("LuminaBuildTool:" + Name));
        return new Guid(Digest);
    }

    private static string Escape(string Value)
    {
        return Value
            .Replace("&", "&amp;")
            .Replace("<", "&lt;")
            .Replace(">", "&gt;")
            .Replace("\"", "&quot;");
    }
}
