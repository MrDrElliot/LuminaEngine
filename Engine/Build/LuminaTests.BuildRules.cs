using LuminaBuildTool.Configuration;

// Supplies a synthesized test suite the engine-wide defaults it cannot inherit by deriving.
[TestSuiteTargetTemplate]
public class LuminaTestSuiteTarget : LuminaTargetRules
{
    public LuminaTestSuiteTarget(TargetInfo Target)
        : base(Target)
    {
    }
}

[TestSuiteModuleTemplate]
public class LuminaTestSuiteModule : LuminaModuleRules
{
    public LuminaTestSuiteModule(TargetInfo Target)
        : base(Target)
    {
    }
}
