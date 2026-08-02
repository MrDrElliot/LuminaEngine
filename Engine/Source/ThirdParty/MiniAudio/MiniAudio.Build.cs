using LuminaBuildTool.Configuration;

public class MiniAudio : LuminaThirdPartyModuleRules
{
    public MiniAudio(TargetInfo Target)
        : base(Target)
    {
        // Included as <MiniAudio/miniaudio.h>, so the ThirdParty root is the include path.
        PublicIncludePaths.Add("..");
    }
}
