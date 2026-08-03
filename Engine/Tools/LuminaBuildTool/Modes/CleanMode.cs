using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.Modes;

/// <summary>
/// Removes a target's intermediate and output files.
/// </summary>
public static class CleanMode
{
    public static int Run(CommandLine Arguments, BuildDirectories Directories)
    {
        string? TargetName = Arguments.GetPositional(1);

        if (string.IsNullOrEmpty(TargetName))
        {
            // No target named: wipe everything the tool owns. Generated code goes with the objects,
            // because a .generated.h for a type that no longer exists stays includable and turns a
            // clean build into a confusing one.
            DeleteDirectory(Directories.BuildToolIntermediatesDirectory);
            DeleteDirectory(Path.Combine(Directories.IntermediatesDirectory, "Obj"));
            DeleteDirectory(Directories.ReflectionDirectory);
            DeleteDirectory(Directories.CSharpBindingsDirectory);
            Log.Info("Cleaned all build tool intermediates.");
            return 0;
        }

        BuildPlatform PlatformValue = Arguments.GetEnum("Platform", BuildPlatformRegistry.HostPlatform);
        BuildConfiguration ConfigurationValue = Arguments.GetEnum("Configuration", BuildConfiguration.Development);
        TargetType TypeValue = Arguments.GetEnum("TargetType", TargetType.Editor);

        RulesAssembly Assembly = RulesAssembly.Create(Directories, bForceRecompile: false);
        IBuildPlatform PlatformSupport = BuildPlatformRegistry.Get(PlatformValue);
        BuildOptions Options = BuildOptions.Load(Directories, Arguments);

        TargetInfo Info = new(TargetName, TypeValue, PlatformValue, ConfigurationValue, Directories, Options);
        BuildTarget Target = new TargetAssembler(Assembly, Directories, PlatformSupport).Assemble(TargetName, Info);

        CleanTarget(Target);

        return 0;
    }

    public static void CleanTarget(BuildTarget Target)
    {
        DeleteDirectory(Target.IntermediateDirectory);

        foreach (BuildModule Module in Target.Modules)
        {
            // Asked of each module rather than assumed from the target, because a module does not
            // necessarily keep its intermediates under the target's own directory: engine modules
            // share one set across every target that compiles them the same way. Deleting only the
            // target's directory would leave those behind and make a clean build a partial one.
            DeleteDirectory(Module.IntermediateDirectory);
            DeleteDirectory(Module.GeneratedCodeDirectory);

            DeleteFile(Module.OutputFile);
            DeleteFile(Module.ImportLibraryFile);

            if (Module.OutputFile.Length > 0)
            {
                DeleteFile(Path.ChangeExtension(Module.OutputFile, ".pdb"));
            }
        }

        Log.Info("Cleaned {0}.", Target);
    }

    private static void DeleteDirectory(string DirectoryPath)
    {
        if (!Directory.Exists(DirectoryPath))
        {
            return;
        }

        try
        {
            Directory.Delete(DirectoryPath, recursive: true);
            Log.Verbose("Deleted '{0}'", DirectoryPath);
        }
        catch (Exception Ex) when (Ex is IOException or UnauthorizedAccessException)
        {
            Log.Warning("Could not delete '{0}': {1}", DirectoryPath, Ex.Message);
        }
    }

    private static void DeleteFile(string FilePath)
    {
        if (FilePath.Length == 0 || !File.Exists(FilePath))
        {
            return;
        }

        try
        {
            File.Delete(FilePath);
            FileItem.Get(FilePath).Invalidate();
        }
        catch (Exception Ex) when (Ex is IOException or UnauthorizedAccessException)
        {
            Log.Warning("Could not delete '{0}': {1}", FilePath, Ex.Message);
        }
    }
}
