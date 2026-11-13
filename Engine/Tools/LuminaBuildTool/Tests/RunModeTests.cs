using LuminaBuildTool.Core;
using LuminaBuildTool.Modes;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class RunModeTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("RunModeTests").FullName;

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    [Theory]
    [InlineData("-Foo -Bar", new[] { "-Foo", "-Bar" })]
    [InlineData("-Foo \"bar baz\"", new[] { "-Foo", "bar baz" })]
    [InlineData("-Foo \"\" -Bar", new[] { "-Foo", "", "-Bar" })]
    [InlineData("-Foo \"unterminated", new[] { "-Foo", "unterminated" })]
    public void SplitArguments_SplitsAsExpected(string Input, string[] Expected)
    {
        List<string> Result = RunMode.SplitArguments(Input);

        Assert.Equal(Expected, Result);
    }

    [Fact]
    public void SplitArguments_ReturnsEmptyForBlankInput()
    {
        Assert.Empty(RunMode.SplitArguments(string.Empty));
        Assert.Empty(RunMode.SplitArguments("   "));
    }

    [Fact]
    public void ResolveLaunch_PrefersDebuggerCommandOverLaunchModuleOutput()
    {
        string Executable = CreateExecutable();
        TestTarget Test = new(nameof(ResolveLaunch_PrefersDebuggerCommandOverLaunchModuleOutput));
        Test.Target.Rules.DebuggerCommand = Executable;
        Test.Target.LaunchModule = Test.AddModule("Launch");
        Test.Target.LaunchModule.OutputFile = Path.Combine(TempDirectory, "Missing.exe");

        RunMode.LaunchCommand Command = RunMode.ResolveLaunch(Test.Target);

        Assert.Equal(Executable, Command.Executable);
    }

    [Fact]
    public void ResolveLaunch_FallsBackToLaunchModuleOutputWhenNoDebuggerCommand()
    {
        string Executable = CreateExecutable();
        TestTarget Test = new(nameof(ResolveLaunch_FallsBackToLaunchModuleOutputWhenNoDebuggerCommand));
        Test.Target.LaunchModule = Test.AddModule("Launch");
        Test.Target.LaunchModule.OutputFile = Executable;

        RunMode.LaunchCommand Command = RunMode.ResolveLaunch(Test.Target);

        Assert.Equal(Executable, Command.Executable);
    }

    [Fact]
    public void ResolveLaunch_ThrowsWhenResolvedExecutableDoesNotExist()
    {
        TestTarget Test = new(nameof(ResolveLaunch_ThrowsWhenResolvedExecutableDoesNotExist));
        Test.Target.Rules.DebuggerCommand = Path.Combine(TempDirectory, "Missing.exe");

        BuildException Thrown = Assert.Throws<BuildException>(() => RunMode.ResolveLaunch(Test.Target));
        Assert.Contains(Test.Target.Rules.DebuggerCommand, Thrown.Message);
    }

    [Fact]
    public void ResolveLaunch_ThrowsWhenNothingNamesAnExecutable()
    {
        TestTarget Test = new(nameof(ResolveLaunch_ThrowsWhenNothingNamesAnExecutable));

        BuildException Thrown = Assert.Throws<BuildException>(() => RunMode.ResolveLaunch(Test.Target));
        Assert.Contains("produces no executable", Thrown.Message);
    }

    private string CreateExecutable()
    {
        string FilePath = Path.Combine(TempDirectory, "Game.exe");
        File.WriteAllBytes(FilePath, Array.Empty<byte>());
        return FilePath;
    }
}
