using LuminaBuildTool.Configuration;

public class RPMalloc : LuminaThirdPartyModuleRules
{
    public RPMalloc(TargetInfo Target)
        : base(Target)
    {
        PublicIncludePaths.Add(".");

        // rpmalloc_global_statistics() needs this to report mapped, peak and huge counters, which
        // the memory profiler reads. Kept out of Shipping.
        if (Target.Configuration != BuildConfiguration.Shipping)
        {
            PublicDefinitions.Add("ENABLE_STATISTICS=1");
        }
    }
}
