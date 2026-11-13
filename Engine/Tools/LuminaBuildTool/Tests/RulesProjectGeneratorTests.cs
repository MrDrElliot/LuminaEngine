using LuminaBuildTool.ProjectFiles;
using LuminaBuildTool.Rules;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class RulesProjectGeneratorTests
{
    [Theory]
    [InlineData("Lumina.Target.cs", RulesFileKind.Target, null, "Targets/Lumina.Target.cs")]
    [InlineData("Core.Build.cs", RulesFileKind.Module, null, "Modules/Core.Build.cs")]
    [InlineData("Net.Build.cs", RulesFileKind.Module, "Net", "Plugins/Net/Net.Build.cs")]
    [InlineData("Net.Target.cs", RulesFileKind.Target, "Net", "Targets/Net/Net.Target.cs")]
    [InlineData("Helpers.BuildRules.cs", RulesFileKind.Shared, null, "Shared/Helpers.BuildRules.cs")]
    public void LinkPathGroupsByKindAndPlugin(string FileName, RulesFileKind Kind, string? PluginName, string Expected)
    {
        RulesFile File = new(Path.Combine(Path.GetTempPath(), "Rules", FileName), Kind, PluginName);

        Assert.Equal(Path.Combine(Expected.Split('/')), RulesProjectGenerator.MakeLinkPath(File));
    }
}
