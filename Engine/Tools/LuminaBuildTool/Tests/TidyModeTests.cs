using System.Text.Json;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Modes;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class TidyModeTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("TidyModeTests").FullName;

    public TidyModeTests()
    {
        Directory.CreateDirectory(Path.Combine(TempDirectory, "Engine", "Source", "Runtime"));
    }

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    private BuildDirectories Directories()
    {
        return BuildDirectories.Discover(TempDirectory, null);
    }

    private string WriteDatabase(params (string Directory, string File)[] Entries)
    {
        List<object> Records = Entries
            .Select(Entry => new { directory = Entry.Directory, file = Entry.File, command = "clang-tidy-does-not-care" })
            .Cast<object>()
            .ToList();

        string DatabasePath = Path.Combine(TempDirectory, "compile_commands.json");
        File.WriteAllText(DatabasePath, JsonSerializer.Serialize(Records));
        return DatabasePath;
    }

    [Fact]
    public void ReadSourcesExcludesExternalDirectories()
    {
        string Source = Path.Combine(TempDirectory, "Engine", "Source", "Runtime", "Foo.cpp");
        string ThirdParty = Path.Combine(TempDirectory, "Engine", "Source", "ThirdParty", "Bar.cpp");
        string External = Path.Combine(TempDirectory, "External", "Baz.cpp");
        string Intermediate = Path.Combine(TempDirectory, "Intermediates", "Gen.cpp");

        string Database = WriteDatabase(
            (TempDirectory, Source),
            (TempDirectory, ThirdParty),
            (TempDirectory, External),
            (TempDirectory, Intermediate));

        List<string> Sources = TidyMode.ReadSources(Database, Directories(), string.Empty);

        Assert.Equal(new[] { Path.GetFullPath(Source) }, Sources);
    }

    [Fact]
    public void ReadSourcesAppliesThePathFilter()
    {
        string Included = Path.Combine(TempDirectory, "Engine", "Source", "Runtime", "Keepme.cpp");
        string Excluded = Path.Combine(TempDirectory, "Engine", "Source", "Runtime", "Skipme.cpp");

        string Database = WriteDatabase((TempDirectory, Included), (TempDirectory, Excluded));

        List<string> Sources = TidyMode.ReadSources(Database, Directories(), "Keepme");

        Assert.Equal(new[] { Path.GetFullPath(Included) }, Sources);
    }

    [Fact]
    public void ReadSourcesDedupsRepeatedEntries()
    {
        string Source = Path.Combine(TempDirectory, "Engine", "Source", "Runtime", "Foo.cpp");
        string Database = WriteDatabase((TempDirectory, Source), (TempDirectory, Source));

        List<string> Sources = TidyMode.ReadSources(Database, Directories(), string.Empty);

        Assert.Equal(new[] { Path.GetFullPath(Source) }, Sources);
    }

    [Fact]
    public void DiagnosticPatternMatchesAndExtractsAllGroups()
    {
        System.Text.RegularExpressions.Match Diagnostic = TidyMode.DiagnosticPattern()
            .Match("Engine/Source/Foo.cpp:12:5: warning: unused variable 'x' [clang-diagnostic-unused-variable]");

        Assert.True(Diagnostic.Success);
        Assert.Equal("Engine/Source/Foo.cpp", Diagnostic.Groups["File"].Value);
        Assert.Equal("12", Diagnostic.Groups["Line"].Value);
        Assert.Equal("5", Diagnostic.Groups["Column"].Value);
        Assert.Equal("warning", Diagnostic.Groups["Severity"].Value);
        Assert.Equal("unused variable 'x'", Diagnostic.Groups["Message"].Value);
        Assert.Equal("clang-diagnostic-unused-variable", Diagnostic.Groups["Check"].Value);
    }

    [Fact]
    public void DiagnosticPatternIgnoresANonMatchingLine()
    {
        System.Text.RegularExpressions.Match Diagnostic = TidyMode.DiagnosticPattern()
            .Match("1 warning generated.");

        Assert.False(Diagnostic.Success);
    }
}
