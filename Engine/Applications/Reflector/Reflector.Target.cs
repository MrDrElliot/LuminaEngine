using LuminaBuildTool.Configuration;

public class ReflectorTarget : LuminaTargetRules
{
    public ReflectorTarget(TargetInfo Target)
        : base(Target)
    {
        Type = TargetType.Program;
        LaunchModuleName = "Reflector";

        OutputSuffix = string.Empty;
        
        if (Target.Configuration == BuildConfiguration.Debug)
        {
            ConfigurationOverride = BuildConfiguration.Development;
        }

        bLinkTimeCodeGeneration = false;
        bDebugSymbols = true;
    }
}
