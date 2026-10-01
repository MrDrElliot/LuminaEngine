using LuminaBuildTool.Configuration;

public class RPMalloc : LuminaThirdPartyModuleRules
{
    public RPMalloc(TargetInfo Target)
        : base(Target)
    {
        PublicIncludePaths.Add(".");

        // Every worker thread keeps its own span cache, and at the stock limits three dozen of them held a gigabyte of free memory.
        PrivateDefinitions.Add("MAX_THREAD_SPAN_CACHE=128");
        PrivateDefinitions.Add("MAX_THREAD_SPAN_LARGE_CACHE=40");

        // rpmalloc_global_statistics() needs this to report mapped, peak and huge counters, which
        // the memory profiler reads. Kept out of Shipping.
        if (Target.Configuration != BuildConfiguration.Shipping)
        {
            PublicDefinitions.Add("ENABLE_STATISTICS=1");
        }
    }
}
