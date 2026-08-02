using LuminaBuildTool.Configuration;

public class ConcurrentQueue : LuminaThirdPartyModuleRules
{
    public ConcurrentQueue(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.HeaderOnly;

        // Included both as <concurrentqueue.h> and <concurrentqueue/concurrentqueue.h>.
        PublicIncludePaths.Add(".");
        PublicIncludePaths.Add("..");
    }
}
