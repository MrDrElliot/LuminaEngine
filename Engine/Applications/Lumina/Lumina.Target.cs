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

        // A shipping game links every module into one executable. Editor targets stay modular
        // even in Shipping: Runtime and Editor each carry their own copy of the stb_image_write
        // implementation, which is fine in separate images and a duplicate symbol in one.
        bMonolithic = Target.Configuration == BuildConfiguration.Shipping && Target.Type == TargetType.Game;

        // The managed engine API. Not linked, but the editor loads it at startup, so a build that
        // skipped it would leave C# scripting silently dead.
        AddManagedProject(
            "../../Source/LuminaSharp/LuminaSharp.csproj",
            $"../../../Binaries/{Target.PlatformName}/DotNet/Managed/LuminaSharp.dll");
    }
}
