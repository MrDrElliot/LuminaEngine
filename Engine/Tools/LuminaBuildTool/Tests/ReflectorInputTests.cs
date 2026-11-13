using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class ReflectorInputTests
{
    private static ReflectionInputDocument MakeDocument()
    {
        return new ReflectionInputDocument
        {
            CodeGenerationVersion = 3,
            ReflectorFingerprint = "ReflectorFp",
            CoreReflectionFingerprint = "CoreFp",
            WorkspaceName = "Workspace",
            WorkspacePath = "C:/Workspace",
        };
    }

    [Fact]
    public void GeneratorOptionsAreJoinedAsKeyValuePairs()
    {
        ReflectionInputDocument Document = MakeDocument();
        Document.Generators.Add(new ReflectionGeneratorEntry("Lib", "LibFp", "Rules", new Dictionary<string, string>
        {
            ["First"] = "One",
            ["Second"] = "Two",
        }));

        string Output = ReflectorInput.Write(Document);

        Assert.Contains("GeneratorOption First=One\n", Output);
        Assert.Contains("GeneratorOption Second=Two\n", Output);
    }

    [Fact]
    public void OptionalRecordsAreSkippedWhenEmpty()
    {
        ReflectionInputDocument Document = MakeDocument();
        Document.Projects.Add(new ReflectionProjectEntry { Name = "Module", Path = "Path" });

        string Output = ReflectorInput.Write(Document);

        Assert.DoesNotContain("GeneratedDir", Output);
        Assert.DoesNotContain("PrecompiledHeader", Output);
        Assert.DoesNotContain("CSharpBindingsDir", Output);
        Assert.DoesNotContain("RouteTypeBindings", Output);
        Assert.DoesNotContain("ReferenceOnly", Output);
    }

    [Fact]
    public void OptionalRecordsAndFlagsAreEmittedWhenSet()
    {
        ReflectionInputDocument Document = MakeDocument();
        Document.Projects.Add(new ReflectionProjectEntry
        {
            Name = "Module",
            Path = "Path",
            GeneratedDir = "Generated",
            PrecompiledHeader = "PCH.h",
            CSharpBindingsDir = "Bindings",
            RouteTypeBindings = true,
            ReferenceOnly = true,
        });

        string Output = ReflectorInput.Write(Document);

        Assert.Contains("GeneratedDir Generated\n", Output);
        Assert.Contains("PrecompiledHeader PCH.h\n", Output);
        Assert.Contains("CSharpBindingsDir Bindings\n", Output);
        Assert.Contains("RouteTypeBindings\n", Output);
        Assert.Contains("ReferenceOnly\n", Output);
    }

    [Theory]
    [InlineData("Bad\nValue")]
    [InlineData("Bad\rValue")]
    public void RecordThrowsOnLineBreaksInValue(string Value)
    {
        ReflectionInputDocument Document = MakeDocument();
        Document.WorkspaceName = Value;

        Assert.Throws<BuildException>(() => ReflectorInput.Write(Document));
    }
}
