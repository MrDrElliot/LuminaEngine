using LuminaBuildTool.Configuration;

public class Entt : LuminaThirdPartyModuleRules
{
    public Entt(TargetInfo Target)
        : base(Target)
    {
        // Header only; included as <entt/entt.hpp> resolved from this directory.
        BinaryType = ModuleBinaryType.HeaderOnly;
        PublicIncludePaths.Add(".");
    }
}
