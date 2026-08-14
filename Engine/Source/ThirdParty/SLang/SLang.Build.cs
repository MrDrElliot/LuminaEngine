using LuminaBuildTool.Configuration;

public class SLang : LuminaThirdPartyModuleRules
{
    public SLang(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.HeaderOnly;
        PublicIncludePaths.Add(".");

        PublicLibraryPaths.Add(ModulePath("../../../../External/SLang/lib"));
        PublicSystemLibraries.Add("slang");
        PublicSystemLibraries.Add("slang-compiler");

        // Read rather than restated: MakeLinuxBundle.sh fetches Slang using the same value, and when
        // the two were written down separately they drifted.
        string SlangVersion = ReadDependencyVersion("SLANG_VERSION");

        // Optional: a running editor can hold these open while a second configuration builds.
        // Slang version-stamps the glsl-module and glslang filenames on Linux but not on Windows.
        string[] RuntimeLibraries = Target.Platform == BuildPlatform.Windows64
            ? new[]
            {
                "bin/slang.dll",
                "bin/slang-compiler.dll",
                "bin/slang-glsl-module.dll",
                "bin/slang-glslang.dll",
                "bin/slang-rt.dll",
            }
            // Staged under the versioned names, not the unversioned symlinks pointing at them: copying
            // a symlink flattens it, and the SONAME the loader asks for is the versioned one.
            : new[]
            {
                $"lib/libslang-compiler.so.0.{SlangVersion}",
                $"lib/libslang-rt.so.0.{SlangVersion}",
                $"lib/libslang-glsl-module-{SlangVersion}.so",
                $"lib/libslang-glslang-{SlangVersion}.so",
            };

        foreach (string Library in RuntimeLibraries)
        {
            AddRuntimeDependency($"../../../../External/SLang/{Library}", bOptional: true);
        }
    }

    private string ReadDependencyVersion(string Key)
    {
        string VersionsFile = ModulePath("../../../Build/DependencyVersions.txt");

        foreach (string Line in System.IO.File.ReadAllLines(VersionsFile))
        {
            string Trimmed = Line.Trim();
            if (Trimmed.StartsWith('#') || !Trimmed.StartsWith(Key + "="))
            {
                continue;
            }
            return Trimmed[(Key.Length + 1)..].Trim();
        }

        throw new System.InvalidOperationException($"{Key} is not defined in {VersionsFile}.");
    }
}
