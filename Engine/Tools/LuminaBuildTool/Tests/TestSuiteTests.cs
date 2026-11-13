using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Rules;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class TestSuiteTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("TestSuiteTests").FullName;

    public void Dispose() => Directory.Delete(TempDirectory, recursive: true);

    private RulesFile MakeModuleRulesFile(string? PluginName = null)
    {
        string Location = Path.Combine(TempDirectory, "Foo.Build.cs");
        File.WriteAllText(Location, string.Empty);
        return new RulesFile(PathUtils.Normalize(Location), RulesFileKind.Module, PluginName);
    }

    private TestTarget MakeTestTarget() => new("Foo") { ModuleDirectory = TempDirectory };

    [Theory]
    [InlineData(TestSuiteKind.Tests)]
    [InlineData(TestSuiteKind.Benchmarks)]
    public void DiscoverFindsADirectoryContainingASourceFile(TestSuiteKind Kind)
    {
        RulesFile Module = MakeModuleRulesFile();
        string SuiteDirectory = Path.Combine(TempDirectory, Kind.ToString(), "Nested");
        Directory.CreateDirectory(SuiteDirectory);
        File.WriteAllText(Path.Combine(SuiteDirectory, "FooTests.cpp"), string.Empty);

        TestSuite Suite = Assert.Single(TestSuite.Discover(Module));

        Assert.Equal(Kind, Suite.Kind);
        Assert.Equal("Foo", Suite.ModuleName);
    }

    [Fact]
    public void DiscoverSkipsAnEmptyDirectory()
    {
        RulesFile Module = MakeModuleRulesFile();
        Directory.CreateDirectory(Path.Combine(TempDirectory, "Tests"));
        File.WriteAllText(Path.Combine(TempDirectory, "Tests", "Notes.txt"), string.Empty);

        Assert.Empty(TestSuite.Discover(Module));
    }

    [Fact]
    public void DiscoverSkipsAMissingDirectory()
    {
        RulesFile Module = MakeModuleRulesFile();

        Assert.Empty(TestSuite.Discover(Module));
    }

    [Theory]
    [InlineData(ModuleHostType.Editor, ModuleHostType.Editor)]
    [InlineData(ModuleHostType.Runtime, ModuleHostType.Developer)]
    [InlineData(ModuleHostType.Developer, ModuleHostType.Developer)]
    public void ApplyToModuleFollowsTheTestedModulesHostType(ModuleHostType TestedHostType, ModuleHostType ExpectedHostType)
    {
        TestTarget Target = MakeTestTarget();
        ModuleRules Tested = Target.MakeModuleRules("Foo");
        Tested.HostType = TestedHostType;
        ModuleRules Rules = Target.MakeModuleRules("FooTests");
        TestSuite Suite = new(Tested.Name, TestSuiteKind.Tests, TempDirectory, MakeModuleRulesFile());

        Suite.ApplyTo(Rules, Tested);

        Assert.Equal(ExpectedHostType, Rules.HostType);
    }

    [Fact]
    public void ApplyToModuleForcesUnityOffAndClearsThePrecompiledHeader()
    {
        TestTarget Target = MakeTestTarget();
        ModuleRules Tested = Target.MakeModuleRules("Foo");
        ModuleRules Rules = Target.MakeModuleRules("FooTests");
        Rules.bUseUnityBuild = true;
        Rules.PrecompiledHeader = new PrecompiledHeaderRules("pch.h", "pch.cpp");
        TestSuite Suite = new(Tested.Name, TestSuiteKind.Tests, TempDirectory, MakeModuleRulesFile());

        Suite.ApplyTo(Rules, Tested);

        Assert.False(Rules.bUseUnityBuild);
        Assert.Null(Rules.PrecompiledHeader);
    }

    [Fact]
    public void ApplyToModuleDependsOnTheTestedModuleOnlyWhenItIsNotAnExecutable()
    {
        TestTarget Target = MakeTestTarget();
        ModuleRules Tested = Target.MakeModuleRules("Foo");
        Tested.BinaryType = ModuleBinaryType.SharedLibrary;
        ModuleRules Rules = Target.MakeModuleRules("FooTests");
        TestSuite Suite = new(Tested.Name, TestSuiteKind.Tests, TempDirectory, MakeModuleRulesFile());

        Suite.ApplyTo(Rules, Tested);

        Assert.Contains("Foo", Rules.PrivateDependencyModuleNames);
    }

    [Fact]
    public void ApplyToModuleSkipsTheDependencyWhenTheTestedModuleIsAnExecutable()
    {
        TestTarget Target = MakeTestTarget();
        ModuleRules Tested = Target.MakeModuleRules("Foo");
        Tested.BinaryType = ModuleBinaryType.ConsoleApplication;
        ModuleRules Rules = Target.MakeModuleRules("FooTests");
        TestSuite Suite = new(Tested.Name, TestSuiteKind.Tests, TempDirectory, MakeModuleRulesFile());

        Suite.ApplyTo(Rules, Tested);

        Assert.DoesNotContain("Foo", Rules.PrivateDependencyModuleNames);
    }

    [Fact]
    public void ApplyToTargetAddsReflectorExactlyOnceEvenWhenAlreadyPresent()
    {
        TargetRules Rules = MakeTestTarget().MakeTargetRules();
        Rules.PreBuildTargetNames.Add("Reflector");
        TestSuite Suite = new("Foo", TestSuiteKind.Tests, TempDirectory, MakeModuleRulesFile());

        Suite.ApplyTo(Rules);

        Assert.Single(Rules.PreBuildTargetNames, Name => Name == "Reflector");
    }

    [Fact]
    public void ApplyToTargetAddsReflectorWhenAbsent()
    {
        TargetRules Rules = MakeTestTarget().MakeTargetRules();
        TestSuite Suite = new("Foo", TestSuiteKind.Tests, TempDirectory, MakeModuleRulesFile());

        Suite.ApplyTo(Rules);

        Assert.Single(Rules.PreBuildTargetNames, Name => Name == "Reflector");
    }

    [Fact]
    public void ApplyToTargetEnablesTheModulesPluginWhenSet()
    {
        TargetRules Rules = MakeTestTarget().MakeTargetRules();
        TestSuite Suite = new("Foo", TestSuiteKind.Tests, TempDirectory, MakeModuleRulesFile("MyPlugin"));

        Suite.ApplyTo(Rules);

        Assert.Contains("MyPlugin", Rules.EnabledPlugins);
    }

    [Fact]
    public void ApplyToTargetLeavesEnabledPluginsEmptyWhenTheModuleHasNone()
    {
        TargetRules Rules = MakeTestTarget().MakeTargetRules();
        TestSuite Suite = new("Foo", TestSuiteKind.Tests, TempDirectory, MakeModuleRulesFile());

        Suite.ApplyTo(Rules);

        Assert.Empty(Rules.EnabledPlugins);
    }
}
