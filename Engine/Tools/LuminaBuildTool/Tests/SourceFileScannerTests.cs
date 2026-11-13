using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class SourceFileScannerTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("SourceFileScannerTests").FullName;

    private readonly ModuleRules Rules;

    public SourceFileScannerTests()
    {
        Rules = new TestTarget("SourceFileScannerTest") { ModuleDirectory = TempDirectory }.AddModule("Test").Rules;
        Directory.CreateDirectory(Path.Combine(TempDirectory, "Source"));
    }

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    private void Touch(params string[] RelativePaths)
    {
        foreach (string RelativePath in RelativePaths)
        {
            string FullPath = Path.Combine(TempDirectory, RelativePath);
            Directory.CreateDirectory(Path.GetDirectoryName(FullPath)!);
            File.WriteAllText(FullPath, string.Empty);
        }
    }

    private static string[] Names(IEnumerable<FileItem> Files)
    {
        return Files.Select(F => F.Name).Order(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    [Fact]
    public void ScanSkipsNestedModuleDirectory()
    {
        Touch("Source/Outer.cpp", "Source/Outer.h", "Source/Inner/Inner.Build.cs", "Source/Inner/Inner.cpp", "Source/Inner/Inner.h");

        ModuleSourceSet Result = SourceFileScanner.Scan(Rules, bFormsOwnImage: true);

        Assert.Equal(new[] { "Outer.cpp", "Outer.h" }, Names(Result.AllFiles));
    }

    [Fact]
    public void ExplicitSourceListCollectsHeadersBesideOwnRulesButSkipsNestedModule()
    {
        Touch("Test.Build.cs", "Source/Outer.cpp", "Source/Outer.h", "Source/Shader.slang", "Source/Inner/Inner.Build.cs", "Source/Inner/Inner.h");
        Rules.bUseExplicitSourceList = true;

        ModuleSourceSet Result = SourceFileScanner.Scan(Rules, bFormsOwnImage: true);

        Assert.Empty(Result.CompilableFiles);
        Assert.Equal(new[] { "Outer.h" }, Names(Result.HeaderFiles));
        Assert.Equal(new[] { "Shader.slang" }, Names(Result.ResourceFiles));
    }

    [Fact]
    public void ExcludedDirectoriesAndFragmentsDropEveryFileKind()
    {
        string[] Kinds = { ".cpp", ".c", ".h", ".slang" };
        Touch(Kinds.SelectMany(Ext => new[] { $"Source/Keep{Ext}", $"Source/Tests/Dir{Ext}", $"Source/Platform/Linux/Frag{Ext}" }).ToArray());
        Rules.ExcludedSourceDirectories.Add(Path.Combine(TempDirectory, "Source", "Tests"));
        Rules.ExcludedSourcePathFragments.Add("linux");

        ModuleSourceSet Result = SourceFileScanner.Scan(Rules, bFormsOwnImage: true);

        Assert.Equal(new[] { "Keep.cpp" }, Names(Result.CppFiles));
        Assert.Equal(new[] { "Keep.c" }, Names(Result.CFiles));
        Assert.Equal(new[] { "Keep.h" }, Names(Result.HeaderFiles));
        Assert.Equal(new[] { "Keep.slang" }, Names(Result.ResourceFiles));
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void PerImageSourceUnderSourceRootIsCountedOnlyWhenFormingOwnImage(bool bFormsOwnImage)
    {
        Touch("Source/Module.cpp", "Source/PerImage.cpp");
        Rules.PerImageSourceFiles.Add("Source/PerImage.cpp");

        ModuleSourceSet Result = SourceFileScanner.Scan(Rules, bFormsOwnImage);

        string[] Expected = bFormsOwnImage ? new[] { "Module.cpp", "PerImage.cpp" } : new[] { "Module.cpp" };
        Assert.Equal(Expected, Names(Result.CppFiles));
    }

    [Fact]
    public void EmptySourceRootThrowsForCompiledModule()
    {
        Assert.Throws<BuildException>(() => SourceFileScanner.Scan(Rules, bFormsOwnImage: true));
    }
}
