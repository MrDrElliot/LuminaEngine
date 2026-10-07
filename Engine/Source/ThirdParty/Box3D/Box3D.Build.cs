using LuminaBuildTool.Configuration;

public class Box3D : LuminaThirdPartyModuleRules
{
    public Box3D(TargetInfo Target)
        : base(Target)
    {
        bCompileAsC = true;

        PublicIncludePaths.Add("include");
        PrivateIncludePaths.Add("src");

        SourceDirectories.Add("src");

        // Box3D's C files declare file-scope statics that collide once merged into one translation unit.
        bUseUnityBuild = false;

        // Box3D needs C17 for _Static_assert and anonymous unions; neither toolchain defaults there.
        if (Target.Platform == BuildPlatform.Windows64)
        {
            PrivateCompilerOptions.Add("/std:c17");
        }
        else
        {
            PrivateCompilerOptions.Add("-std=c17");
            PublicSystemLibraries.Add("m");
        }

        // BOX3D_DOUBLE_PRECISION widens b3Pos and changes every header's ABI, so it stays off.

        if (LuminaFeatures.IsActive(Target, LuminaFeatures.Box3DDebugChecks))
        {
            PublicDefinitions.Add("B3_ENABLE_ASSERT");
            PrivateDefinitions.Add("BOX3D_VALIDATE");
        }
    }
}
