using LuminaBuildTool.Configuration;

public class ReflectorTarget : LuminaTargetRules
{
    public ReflectorTarget(TargetInfo Target)
        : base(Target)
    {
        Type = TargetType.Program;
        LaunchModuleName = "Reflector";

        bHostsReflectionGenerators = true;

        // The fixture ReflectorTests loads, small enough to build with every Reflector.
        ExtraModuleNames.Add("ReflectorTestGenerator");

        OutputSuffix = string.Empty;
        
        if (Target.Configuration == BuildConfiguration.Debug)
        {
            ConfigurationOverride = BuildConfiguration.Development;
        }

        bLinkTimeCodeGeneration = false;
        bDebugSymbols = true;
    }
}
