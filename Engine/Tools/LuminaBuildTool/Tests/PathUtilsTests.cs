using LuminaBuildTool.Core;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class PathUtilsTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("PathUtilsTests").FullName;

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    [Theory]
    [InlineData("", "\"\"")]
    [InlineData("Token", "Token")]
    [InlineData("has space", "\"has space\"")]
    [InlineData("has space\\", "\"has space\\\\\"")]
    [InlineData("has\"quote", "\"has\\\"quote\"")]
    public void Quote_EscapesForWindowsCommandLine(string Argument, string Expected)
    {
        Assert.Equal(Expected, PathUtils.Quote(Argument));
    }

    [Theory]
    [InlineData("", "\"\"")]
    [InlineData("Token", "Token")]
    [InlineData("has space", "\"has space\"")]
    [InlineData(@"trailing\", "\"trailing\\\\\"")]
    [InlineData("has\"quote", "\"has\\\"quote\"")]
    public void QuoteUnix_EscapesForUnixShell(string Argument, string Expected)
    {
        Assert.Equal(Expected, PathUtils.QuoteUnix(Argument));
    }

    [Fact]
    public void IsUnder_TrueForChildPath()
    {
        string Root = Path.Combine(TempDirectory, "Foo");
        string Child = Path.Combine(Root, "Bar.txt");

        Assert.True(PathUtils.IsUnder(Child, Root));
    }

    [Fact]
    public void IsUnder_FalseForSiblingWithSharedPrefix()
    {
        string Root = Path.Combine(TempDirectory, "Foo");
        string Sibling = Path.Combine(TempDirectory, "FooBar");

        Assert.False(PathUtils.IsUnder(Sibling, Root));
    }

    [Fact]
    public void IsUnder_TrueForDirectoryItself()
    {
        string Root = Path.Combine(TempDirectory, "Foo");

        Assert.True(PathUtils.IsUnder(Root, Root));
    }

    [Fact]
    public void WriteFileIfChanged_ReportsNoChangeOnIdenticalRewrite()
    {
        string FilePath = Path.Combine(TempDirectory, "Same.txt");

        Assert.True(PathUtils.WriteFileIfChanged(FilePath, "Content"));
        Assert.False(PathUtils.WriteFileIfChanged(FilePath, "Content"));
    }

    [Fact]
    public void WriteFileIfChanged_ReportsChangeWhenContentDiffers()
    {
        string FilePath = Path.Combine(TempDirectory, "Content.txt");

        Assert.True(PathUtils.WriteFileIfChanged(FilePath, "Content"));
        Assert.True(PathUtils.WriteFileIfChanged(FilePath, "Different"));
    }

    [Fact]
    public void WriteFileIfChanged_ReportsChangeWhenByteOrderMarkDiffers()
    {
        string FilePath = Path.Combine(TempDirectory, "Bom.txt");

        Assert.True(PathUtils.WriteFileIfChanged(FilePath, "Content", bByteOrderMark: true));
        Assert.True(PathUtils.WriteFileIfChanged(FilePath, "Content", bByteOrderMark: false));
    }
}
