using LuminaBuildTool.Configuration;

public class LuminaTarget : LuminaTargetRules
{
    public LuminaTarget(TargetInfo Target)
        : base(Target)
    {
        // Editor or Game, whichever was requested.
        Type = Target.Type;
        LaunchModuleName = "Lumina";

        // What the IDE runs when you press play.
        bIsStartupTarget = true;

        // The reflection generator must exist before this target's headers can be processed.
        PreBuildTargetNames.Add("Reflector");

        // Nothing links it; it puts the shader tree in the generated IDE projects.
        ExtraModuleNames.Add("Shaders");

        // Editor targets stay modular even in Shipping because Runtime and Editor each carry a stb_image_write copy that would collide in one image.
        bMonolithic = Target.Configuration == BuildConfiguration.Shipping && (Target.Type == TargetType.Game || Target.Type == TargetType.Server);

        // Not linked, but the editor loads it at startup, so skipping it leaves C# scripting silently dead.
        AddManagedProject(
            "../../Source/LuminaSharp/LuminaSharp.csproj",
            $"../../../Binaries/{Target.PlatformName}/DotNet/Managed/LuminaSharp.dll");
    }
}
