using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Rules;

// Correctness by default; a benchmark measures rather than asserts, so it is built and run separately.
public enum TestSuiteKind
{
    Tests,
    Benchmarks,
}

// A Tests or Benchmarks directory found beside a module's Build.cs.
public sealed class TestSuite
{
    // Module supplying main() and the engine start-up every test binary needs.
    public const string HarnessModuleName = "TestHarness";

    private static readonly TestSuiteKind[] Kinds = { TestSuiteKind.Tests, TestSuiteKind.Benchmarks };

    public TestSuite(string ModuleName, TestSuiteKind Kind, string SourceDirectory, RulesFile ModuleRulesFile)
    {
        this.ModuleName = ModuleName;
        this.Kind = Kind;
        this.SourceDirectory = SourceDirectory;
        this.ModuleRulesFile = ModuleRulesFile;
    }

    // Module under test.
    public string ModuleName { get; }

    public TestSuiteKind Kind { get; }

    // Name of the synthesized module and target, for example "RuntimeTests".
    public string SuiteName => ModuleName + Kind;

    // Absolute path of the suite's source directory.
    public string SourceDirectory { get; }

    // The tested module's rules file, which the suite borrows for identity and freshness.
    public RulesFile ModuleRulesFile { get; }

    // Every suite directory beside a module rules file, of either kind.
    public static IEnumerable<TestSuite> Discover(RulesFile ModuleRulesFile)
    {
        foreach (TestSuiteKind Kind in Kinds)
        {
            string Candidate = Path.Combine(ModuleRulesFile.Directory, Kind.ToString());

            if (!Directory.Exists(Candidate))
            {
                continue;
            }

            // An empty directory is not a suite; it would synthesize a target that links nothing.
            if (!Directory.EnumerateFiles(Candidate, "*.cpp", SearchOption.AllDirectories).Any())
            {
                continue;
            }

            yield return new TestSuite(
                ModuleRulesFile.DeclaredName, Kind, PathUtils.Normalize(Candidate), ModuleRulesFile);
        }
    }

    // Turns a freshly constructed rules object into this suite's module.
    public void ApplyTo(ModuleRules Rules, ModuleRules Tested)
    {
        Rules.BinaryType = ModuleBinaryType.ConsoleApplication;
        Rules.HostType = Tested.HostType == ModuleHostType.Editor ? ModuleHostType.Editor : ModuleHostType.Developer;

        Rules.bEnableReflection = Tested.bEnableTestReflection;
        Rules.bRootSourceFiles = true;

        // A test file has to compile on its own. Unity would let one borrow a neighbor's includes and
        // would collide the using-directives independent test files are entitled to write.
        Rules.bUseUnityBuild = false;

        // A template may declare a precompiled header the suite has no source to create it from.
        Rules.PrecompiledHeader = null;

        // The suite's own directory, then the tested module's, so a test reaches an internal header
        // by the same path the module's own sources use.
        Rules.PrivateIncludePaths.Add(".");
        Rules.PrivateIncludePaths.Add(Tested.ResolveSourceRoot());

        foreach (string Path in Tested.PrivateIncludePaths)
        {
            Rules.PrivateIncludePaths.Add(Tested.ModulePath(Path));
        }

        Rules.PrivateDependencyModuleNames.Add(Tested.Name);
        Rules.PrivateDependencyModuleNames.Add(HarnessModuleName);
        Rules.PrivateDependencyModuleNames.AddRange(Tested.TestDependencyModuleNames);
    }

    // Turns a freshly constructed rules object into this suite's target.
    public void ApplyTo(TargetRules Rules)
    {
        Rules.Name = SuiteName;
        Rules.LaunchModuleName = SuiteName;

        if (!Rules.PreBuildTargetNames.Contains("Reflector", StringComparer.OrdinalIgnoreCase))
        {
            Rules.PreBuildTargetNames.Add("Reflector");
        }

        // The tests link the same module images the editor loads.
        Rules.bMonolithic = false;

        // Built on request, so a solution build does not compile every suite in the tree.
        Rules.bBuildByDefault = false;

        // A suite is not the engine, and publishing would overwrite what project builds read.
        Rules.bPublishesEngineReflectionManifest = false;
    }
}

// Fallback module rules for a suite in a tree that declares no template.
public sealed class TestSuiteModuleRules : ModuleRules
{
    public TestSuiteModuleRules(TargetInfo Target)
        : base(Target)
    {
    }
}

// Fallback target rules for a suite in a tree that declares no template.
public sealed class TestSuiteTargetRules : TargetRules
{
    public TestSuiteTargetRules(TargetInfo Target)
        : base(Target)
    {
    }
}
