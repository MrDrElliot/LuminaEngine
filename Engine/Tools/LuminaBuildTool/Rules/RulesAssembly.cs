using System.Reflection;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Rules;

/// <summary>The compiled rules assembly plus the lookups mapping a target or module name to its type.</summary>
public sealed class RulesAssembly
{
    private readonly Dictionary<string, Type> TargetTypes = new(StringComparer.OrdinalIgnoreCase);

    private readonly Dictionary<string, Type> ModuleTypes = new(StringComparer.OrdinalIgnoreCase);

    private readonly Dictionary<string, RulesFile> ModuleFiles = new(StringComparer.OrdinalIgnoreCase);

    private readonly Dictionary<string, RulesFile> TargetFiles = new(StringComparer.OrdinalIgnoreCase);

    private readonly Dictionary<string, TestSuite> TestSuites = new(StringComparer.OrdinalIgnoreCase);

    // Supply the tree's engine-wide defaults, which a synthesized suite cannot inherit by deriving.
    private readonly Type? TestSuiteTargetTemplate;

    private readonly Type? TestSuiteModuleTemplate;

    private RulesAssembly(Assembly Compiled, IReadOnlyList<RulesFile> Files, IReadOnlyList<PluginDescriptor> Plugins)
    {
        this.Compiled = Compiled;
        this.Plugins = Plugins;
        this.RulesFiles = Files;

        Dictionary<string, Type> DeclaredTypes = new(StringComparer.OrdinalIgnoreCase);

        foreach (Type Candidate in Compiled.GetTypes())
        {
            if (Candidate.IsAbstract || !Candidate.IsClass)
            {
                continue;
            }

            DeclaredTypes[Candidate.Name] = Candidate;

            if (Candidate.GetCustomAttributes(typeof(TestSuiteTargetTemplateAttribute), false).Length > 0)
            {
                TestSuiteTargetTemplate = Candidate;
            }

            if (Candidate.GetCustomAttributes(typeof(TestSuiteModuleTemplateAttribute), false).Length > 0)
            {
                TestSuiteModuleTemplate = Candidate;
            }
        }

        foreach (RulesFile File in Files)
        {
            switch (File.Kind)
            {
                case RulesFileKind.Target:
                    Register(File, DeclaredTypes, typeof(TargetRules), TargetTypes, TargetFiles, "Target");
                    break;

                case RulesFileKind.Module:
                    Register(File, DeclaredTypes, typeof(ModuleRules), ModuleTypes, ModuleFiles, "Module");
                    break;
            }
        }

        foreach (RulesFile File in ModuleFiles.Values)
        {
            foreach (TestSuite Suite in TestSuite.Discover(File))
            {
                TestSuites[Suite.SuiteName] = Suite;
                Log.Trace("Discovered {0} suite '{1}' at {2}", Suite.Kind, Suite.SuiteName, Suite.SourceDirectory);
            }
        }
    }

    public Assembly Compiled { get; }

    public IReadOnlyList<PluginDescriptor> Plugins { get; }

    /// <summary>Every rules file that went into the assembly, shared helpers included.</summary>
    public IReadOnlyList<RulesFile> RulesFiles { get; }

    /// <summary>Fingerprint of the rules files this assembly was compiled from.</summary>
    public string SourceHash => SourceHashValue ??= RulesCompiler.ComputeSourceHash(RulesFiles.Select(F => F.Location));

    private string? SourceHashValue;

    public IReadOnlyCollection<string> TargetNames => TargetTypes.Keys;

    public IReadOnlyCollection<string> ModuleNames => ModuleTypes.Keys;

    // Suites synthesized from a Tests directory beside a module's Build.cs, keyed by suite name.
    public IReadOnlyDictionary<string, TestSuite> DiscoveredTestSuites => TestSuites;

    public static RulesAssembly Create(BuildDirectories Directories, bool bForceRecompile)
    {
        RulesScanner Scanner = new(Directories);
        List<RulesFile> Files = Scanner.Scan();

        if (Files.Count == 0)
        {
            throw new BuildException(
                $"No Target.cs or Build.cs rules files were found under '{Directories.EngineRoot}'.");
        }

        Assembly Compiled = RulesCompiler.CompileOrLoad(Directories, Files, bForceRecompile);
        return new RulesAssembly(Compiled, Files, Scanner.DiscoveredPlugins);
    }

    public bool HasModule(string Name) => ModuleTypes.ContainsKey(Name) || TestSuites.ContainsKey(Name);

    public bool HasTarget(string Name) => TargetTypes.ContainsKey(Name) || TestSuites.ContainsKey(Name);

    /// <summary>Absolute path of the rules file a module was declared in.</summary>
    public string GetModuleRulesFile(string Name)
    {
        return ModuleFiles.TryGetValue(Name, out RulesFile? File) ? File.Location : string.Empty;
    }

    public IEnumerable<string> GetAllRulesFilePaths()
    {
        foreach (RulesFile File in TargetFiles.Values)
        {
            yield return File.Location;
        }

        foreach (RulesFile File in ModuleFiles.Values)
        {
            yield return File.Location;
        }
    }

    public TargetRules CreateTargetRules(string Name, TargetInfo Info)
    {
        if (TestSuites.TryGetValue(Name, out TestSuite? Suite))
        {
            return CreateTestSuiteTargetRules(Suite, Info);
        }

        if (!TargetTypes.TryGetValue(Name, out Type? RulesType))
        {
            throw new BuildException(
                $"No target named '{Name}'. Known targets: {string.Join(", ", TargetTypes.Keys.OrderBy(K => K))}");
        }

        RulesFile File = TargetFiles[Name];

        TargetRules.PendingConstruction = new TargetRules.ConstructionContext
        {
            RulesFile = File.Location,
            RulesDirectory = File.Directory,
        };

        try
        {
            TargetRules Rules = (TargetRules)Instantiate(RulesType, Info, File);

            if (string.IsNullOrEmpty(Rules.Name))
            {
                Rules.Name = Name;
            }

            return Rules;
        }
        finally
        {
            TargetRules.PendingConstruction = null;
        }
    }

    public ModuleRules CreateModuleRules(string Name, TargetInfo Info)
    {
        if (TestSuites.TryGetValue(Name, out TestSuite? Suite))
        {
            return CreateTestSuiteModuleRules(Suite, Info);
        }

        if (!ModuleTypes.TryGetValue(Name, out Type? RulesType))
        {
            throw new BuildException($"No module named '{Name}' was found in any Build.cs file.");
        }

        RulesFile File = ModuleFiles[Name];

        ModuleRules.PendingConstruction = new ModuleRules.ConstructionContext
        {
            Name = Name,
            RulesFile = File.Location,
            ModuleDirectory = File.Directory,
            PluginName = File.PluginName ?? string.Empty,
        };

        try
        {
            ModuleRules Rules = (ModuleRules)Instantiate(RulesType, Info, File);

            // A Build.cs sitting at its own source root would otherwise walk its suites back into itself.
            foreach (TestSuite OwnSuite in TestSuites.Values)
            {
                if (OwnSuite.ModuleName.Equals(Name, StringComparison.OrdinalIgnoreCase))
                {
                    Rules.ExcludedSourceDirectories.Add(OwnSuite.SourceDirectory);
                }
            }

            return Rules;
        }
        finally
        {
            ModuleRules.PendingConstruction = null;
        }
    }

    private ModuleRules CreateTestSuiteModuleRules(TestSuite Suite, TargetInfo Info)
    {
        ModuleRules Tested = CreateModuleRules(Suite.ModuleName, Info);

        ModuleRules.PendingConstruction = new ModuleRules.ConstructionContext
        {
            Name = Suite.SuiteName,

            // Borrowed from the module under test, so a rules edit still invalidates the suite.
            RulesFile = Suite.ModuleRulesFile.Location,
            ModuleDirectory = Suite.SourceDirectory,
            PluginName = Suite.ModuleRulesFile.PluginName ?? string.Empty,
        };

        try
        {
            ModuleRules Rules = TestSuiteModuleTemplate is not null
                ? (ModuleRules)Activator.CreateInstance(TestSuiteModuleTemplate, Info)!
                : new TestSuiteModuleRules(Info);

            Suite.ApplyTo(Rules, Tested);
            return Rules;
        }
        finally
        {
            ModuleRules.PendingConstruction = null;
        }
    }

    private TargetRules CreateTestSuiteTargetRules(TestSuite Suite, TargetInfo Info)
    {
        TargetRules.PendingConstruction = new TargetRules.ConstructionContext
        {
            RulesFile = Suite.ModuleRulesFile.Location,
            RulesDirectory = Suite.SourceDirectory,
        };

        try
        {
            TargetRules Rules = TestSuiteTargetTemplate is not null
                ? (TargetRules)Activator.CreateInstance(TestSuiteTargetTemplate, Info)!
                : new TestSuiteTargetRules(Info);

            Suite.ApplyTo(Rules);
            return Rules;
        }
        finally
        {
            TargetRules.PendingConstruction = null;
        }
    }

    private static object Instantiate(Type RulesType, TargetInfo Info, RulesFile File)
    {
        ConstructorInfo? Constructor = RulesType.GetConstructor(new[] { typeof(TargetInfo) });

        if (Constructor is null)
        {
            throw new BuildException(
                $"'{RulesType.Name}' in '{File.Location}' needs a public constructor taking a single TargetInfo parameter.");
        }

        try
        {
            return Constructor.Invoke(new object[] { Info });
        }
        catch (TargetInvocationException Ex)
        {
            Exception Inner = Ex.InnerException ?? Ex;
            throw new BuildException($"'{RulesType.Name}' in '{File.Location}' threw during construction: {Inner.Message}");
        }
    }

    private static void Register(
        RulesFile File,
        IReadOnlyDictionary<string, Type> DeclaredTypes,
        Type BaseType,
        Dictionary<string, Type> TypesByName,
        Dictionary<string, RulesFile> FilesByName,
        string Kind)
    {
        string Name = File.DeclaredName;

        // A Target.cs may name its class either <Name> or <Name>Target.
        Type? Found = null;

        if (DeclaredTypes.TryGetValue(Name, out Type? Exact) && BaseType.IsAssignableFrom(Exact))
        {
            Found = Exact;
        }
        else if (DeclaredTypes.TryGetValue(Name + Kind, out Type? Suffixed) && BaseType.IsAssignableFrom(Suffixed))
        {
            Found = Suffixed;
        }

        if (Found is null)
        {
            throw new BuildException(
                $"'{File.Location}' does not declare a public class named '{Name}' or '{Name}{Kind}' deriving from {BaseType.Name}.");
        }

        TypesByName[Name] = Found;
        FilesByName[Name] = File;

        Log.Trace("Registered {0} '{1}' from {2}", Kind, Name, File.Location);
    }
}
