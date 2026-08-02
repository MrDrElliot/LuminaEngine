using System.Text;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.Modes;

/// <summary>
/// Prints what the tool discovered, for inspecting the graph without building it.
/// </summary>
public static class QueryMode
{
    public static int Run(CommandLine Arguments, BuildDirectories Directories)
    {
        RulesAssembly Assembly = RulesAssembly.Create(Directories, Arguments.HasFlag("RecompileRules"));

        string? TargetName = Arguments.GetPositional(1);

        if (string.IsNullOrEmpty(TargetName))
        {
            Log.Info("Engine root: {0}", Directories.EngineRoot);

            Log.Info("Targets ({0}):", Assembly.TargetNames.Count);

            foreach (string Name in Assembly.TargetNames.OrderBy(N => N, StringComparer.OrdinalIgnoreCase))
            {
                Log.Info("  {0}", Name);
            }

            Log.Info("Modules ({0}):", Assembly.ModuleNames.Count);

            foreach (string Name in Assembly.ModuleNames.OrderBy(N => N, StringComparer.OrdinalIgnoreCase))
            {
                Log.Info("  {0}", Name);
            }

            Log.Info("Plugins ({0}):", Assembly.Plugins.Count);

            foreach (PluginDescriptor Plugin in Assembly.Plugins)
            {
                Log.Info(
                    "  {0} ({1}, {2})",
                    Plugin.Name,
                    Plugin.EnabledByDefault ? "enabled by default" : "opt in",
                    string.Join(", ", Plugin.Modules.Select(M => M.Name)));
            }

            return 0;
        }

        BuildPlatform PlatformValue = Arguments.GetEnum("Platform", BuildPlatformRegistry.HostPlatform);
        BuildConfiguration ConfigurationValue = Arguments.GetEnum("Configuration", BuildConfiguration.Development);
        TargetType TypeValue = Arguments.GetEnum("TargetType", TargetType.Editor);

        IBuildPlatform PlatformSupport = BuildPlatformRegistry.Get(PlatformValue);
        BuildOptions Options = BuildOptions.Load(Directories, Arguments);

        TargetInfo Info = new(TargetName, TypeValue, PlatformValue, ConfigurationValue, Directories, Options);
        BuildTarget Target = new TargetAssembler(Assembly, Directories, PlatformSupport).Assemble(TargetName, Info);

        StringBuilder Report = new();
        Report.AppendLine($"Target {Target.Name} ({Target.Rules.Type}) {Target.Info.PlatformName} {Target.Info.Configuration}");
        Report.AppendLine($"  Launch module: {Target.LaunchModule?.Name ?? "(none)"}");
        Report.AppendLine($"  Binaries:      {Target.BinariesDirectory}");
        Report.AppendLine($"  Intermediates: {Target.IntermediateDirectory}");
        Report.AppendLine($"  Plugins:       {(Target.EnabledPlugins.Count == 0 ? "(none)" : string.Join(", ", Target.EnabledPlugins.Select(P => P.Name)))}");
        Report.AppendLine();
        Report.AppendLine($"Modules in build order ({Target.Modules.Count}):");

        foreach (BuildModule Module in Target.Modules)
        {
            int SourceCount = Module.Sources.CppFiles.Count + Module.Sources.CFiles.Count;

            Report.AppendLine($"  {Module.Name} [{Module.BinaryType}] {SourceCount} sources");

            if (Arguments.HasFlag("Verbose"))
            {
                Report.AppendLine($"      output:   {(Module.OutputFile.Length > 0 ? Module.OutputFile : "(none)")}");
                Report.AppendLine($"      public:   {string.Join(", ", Module.Rules.PublicDependencyModuleNames)}");
                Report.AppendLine($"      private:  {string.Join(", ", Module.Rules.PrivateDependencyModuleNames)}");
                Report.AppendLine($"      includes: {Module.CompileIncludePaths.Count}");
                Report.AppendLine($"      defines:  {Module.CompileDefinitions.Count}");
                Report.AppendLine($"      links:    {Module.LinkLibraries.Count}");
            }
        }

        Log.Raw(Report.ToString());

        return 0;
    }
}
