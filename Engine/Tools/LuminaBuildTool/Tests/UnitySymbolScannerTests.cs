using LuminaBuildTool.Graph;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class UnitySymbolScannerTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("UnitySymbolScannerTests").FullName;

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    private string WriteSource(string Source)
    {
        string SourcePath = Path.Combine(TempDirectory, $"Source{Directory.GetFiles(TempDirectory).Length}.cpp");
        File.WriteAllText(SourcePath, Source);
        return SourcePath;
    }

    private string[] ScanSource(string Source)
    {
        return UnitySymbolScanner.Scan(WriteSource(Source)).Order(StringComparer.Ordinal).ToArray();
    }

    [Fact]
    public void FileScopeStaticCollides()
    {
        string First = WriteSource("static int Counter = 0;\n");
        string Second = WriteSource("static int Counter = 1;\n");

        UnitySymbolConflict Conflict = Assert.Single(UnitySymbolScanner.FindConflicts([First, Second]));

        Assert.Equal("Counter", Conflict.Name);
        Assert.Equal([First, Second], Conflict.Sources);
    }

    [Fact]
    public void NonStaticFreeFunctionDoesNotCollide()
    {
        string First = WriteSource("void Helper()\n{\n}\n");
        string Second = WriteSource("void Helper()\n{\n}\n");

        Assert.Empty(UnitySymbolScanner.FindConflicts([First, Second]));
    }

    [Fact]
    public void AnonymousNamespaceMemberCollidesWithoutStatic()
    {
        string First = WriteSource("namespace\n{\n    int Counter = 0;\n}\n");
        string Second = WriteSource("namespace\n{\n    void Counter() {}\n}\n");

        Assert.Equal("Counter", Assert.Single(UnitySymbolScanner.FindConflicts([First, Second])).Name);
    }

    [Fact]
    public void NamedNamespaceQualifiesStatics()
    {
        Assert.Equal(["Render::Counter"], ScanSource("namespace Render\n{\n    static int Counter = 0;\n    int Shared = 0;\n}\n"));
    }

    [Fact]
    public void FunctionLocalsAreNotReported()
    {
        Assert.Equal(["Tick"], ScanSource("static void Tick()\n{\n    static int Local = 0;\n    if (Local) { Local++; }\n}\n"));
    }

    [Fact]
    public void BracesInCommentsAndLiteralsDoNotShiftScope()
    {
        string Source =
            "// {\n" +
            "/* { */\n" +
            "static const char* Open = \"{\\\"\";\n" +
            "static char Close = '}';\n" +
            "static void Tick()\n{\n    const char* Text = \"}\";\n    static int Local = 0;\n}\n" +
            "static const char* Json = R\"json({\"Key\": \"{\"})json\";\n" +
            "static int After = 0;\n";

        Assert.Equal(["After", "Close", "Json", "Open", "Tick"], ScanSource(Source));
    }

    [Fact]
    public void DigitSeparatorIsNotACharacterLiteral()
    {
        Assert.Equal(["After", "Big"], ScanSource("static int Big = 1'000;\nstatic int After = 0;\n"));
    }

    [Fact]
    public void CrlfMacroContinuationIsStripped()
    {
        Assert.Equal(["After"], ScanSource("#define DECLARE(x) \\\r\n    static int Hidden = x;\r\nstatic int After = 0;\r\n"));
    }

    [Fact]
    public void IndentedDirectiveDoesNotSwallowNextDeclaration()
    {
        Assert.Equal(["Counter"], ScanSource("namespace\n{\n    #if WITH_EDITOR\n    int Counter = 0;\n    #endif\n}\n"));
    }
}
