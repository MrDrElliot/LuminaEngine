using System.Text.Json;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class PluginDescriptorTests : IDisposable
{
    private readonly string DescriptorPath = Path.Combine(Path.GetTempPath(), $"PluginDescriptorTests-{Guid.NewGuid():N}.lplugin");

    public void Dispose()
    {
        File.Delete(DescriptorPath);
    }

    private PluginDescriptor LoadFrom(string Json)
    {
        File.WriteAllText(DescriptorPath, Json);
        return PluginDescriptor.Load(DescriptorPath);
    }

    [Fact]
    public void PluginDependency_ParsesBareString()
    {
        PluginDependency Dependency = JsonSerializer.Deserialize<PluginDependency>("\"Core\"")!;

        Assert.Equal("Core", Dependency.Name);
        Assert.False(Dependency.Optional);
        Assert.Equal(0, Dependency.Version);
    }

    [Fact]
    public void PluginDependency_ParsesObjectForm()
    {
        PluginDependency Dependency = JsonSerializer.Deserialize<PluginDependency>(
            """{ "Name": "Core", "Optional": true, "Version": 3 }""")!;

        Assert.Equal("Core", Dependency.Name);
        Assert.True(Dependency.Optional);
        Assert.Equal(3, Dependency.Version);
    }

    [Fact]
    public void PluginDependency_ThrowsOnWrongTypedOptional()
    {
        Assert.Throws<JsonException>(() =>
            JsonSerializer.Deserialize<PluginDependency>("""{ "Name": "Core", "Optional": "yes" }"""));
    }

    [Fact]
    public void PluginDependency_ThrowsOnWrongTypedVersion()
    {
        Assert.Throws<JsonException>(() =>
            JsonSerializer.Deserialize<PluginDependency>("""{ "Name": "Core", "Version": "3" }"""));
    }

    [Fact]
    public void Load_ThrowsBuildExceptionOnMalformedJson()
    {
        Assert.Throws<BuildException>(() => LoadFrom("{ not valid json"));
    }

    [Fact]
    public void Load_ThrowsBuildExceptionOnMissingName()
    {
        BuildException Ex = Assert.Throws<BuildException>(() => LoadFrom("{}"));
        Assert.Contains("Name", Ex.Message);
    }

    [Fact]
    public void Load_ThrowsBuildExceptionOnDependencyWithNoName()
    {
        string Json = """{ "Name": "Plugin", "Dependencies": [ { "Optional": true } ] }""";
        Assert.Throws<BuildException>(() => LoadFrom(Json));
    }

    [Fact]
    public void Load_ThrowsBuildExceptionOnUnknownModuleType()
    {
        string Json = """{ "Name": "Plugin", "Modules": [ { "Name": "M", "Type": "Bogus" } ] }""";
        BuildException Ex = Assert.Throws<BuildException>(() => LoadFrom(Json));
        Assert.Contains("Type", Ex.Message);
    }

    [Fact]
    public void Load_ThrowsBuildExceptionOnUnknownLoadingPhase()
    {
        string Json = """{ "Name": "Plugin", "Modules": [ { "Name": "M", "LoadingPhase": "Bogus" } ] }""";
        BuildException Ex = Assert.Throws<BuildException>(() => LoadFrom(Json));
        Assert.Contains("LoadingPhase", Ex.Message);
    }

    [Fact]
    public void Load_ThrowsBuildExceptionOnNullSupportedPlatform()
    {
        string Json = """{ "Name": "Plugin", "SupportedPlatforms": [ "Windows", null ] }""";
        BuildException Ex = Assert.Throws<BuildException>(() => LoadFrom(Json));
        Assert.Contains("SupportedPlatforms", Ex.Message);
    }

    [Fact]
    public void Load_AcceptsAWellFormedDescriptor()
    {
        string Json = """
        {
            "Name": "Plugin",
            "SupportedPlatforms": [ "Windows" ],
            "Dependencies": [ "Core", { "Name": "Renderer", "Optional": true, "Version": 2 } ],
            "Modules": [ { "Name": "PluginModule", "Type": "Runtime", "LoadingPhase": "PreEngineInit" } ]
        }
        """;

        PluginDescriptor Descriptor = LoadFrom(Json);

        Assert.Equal("Plugin", Descriptor.Name);
        Assert.Equal(2, Descriptor.Dependencies.Count);
        Assert.Equal("Core", Descriptor.Dependencies[0].Name);
        Assert.Equal("Renderer", Descriptor.Dependencies[1].Name);
        Assert.True(Descriptor.Dependencies[1].Optional);
        Assert.Equal(2, Descriptor.Dependencies[1].Version);
        Assert.False(string.IsNullOrEmpty(Descriptor.DescriptorFile));
        Assert.Equal(Path.GetDirectoryName(Descriptor.DescriptorFile), Descriptor.RootDirectory);
    }
}
