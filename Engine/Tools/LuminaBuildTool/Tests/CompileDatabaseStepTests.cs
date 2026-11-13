using LuminaBuildTool.ProjectFiles;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class CompileDatabaseStepTests
{
    [Fact]
    public void PrecompiledHeaderFlagsBecomeForcedIncludes()
    {
        Assert.Equal(new[] { "/FIPCH.h", "/c" }, CompileDatabaseStep.Translate(new[] { "/YcPCH.h", "/c" }));
        Assert.Equal(new[] { "/FIPCH.h", "/c" }, CompileDatabaseStep.Translate(new[] { "/YuPCH.h", "/c" }));
    }

    [Fact]
    public void DuplicateForcedIncludesAreDropped()
    {
        string[] Arguments = { "/FIPCH.h", "/YuPCH.h", "/FIpch.h", "/FIOther.h", "/FIOther.h" };

        Assert.Equal(new[] { "/FIPCH.h", "/FIOther.h" }, CompileDatabaseStep.Translate(Arguments));
    }

    [Fact]
    public void CompilerOnlyFlagsAreStripped()
    {
        string[] Arguments = { "/sourceDependencies \"A.json\"", "/Fp\"A.pch\"", "/W4", "A.cpp" };

        Assert.Equal(new[] { "/W4", "A.cpp" }, CompileDatabaseStep.Translate(Arguments));
    }
}
