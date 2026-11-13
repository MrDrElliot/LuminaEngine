using LuminaBuildTool.Configuration;

// The generator ReflectorTests loads to drive each way a Reflection module can succeed or fail.
public class ReflectorTestGenerator : ModuleRules
{
    public ReflectorTestGenerator(TargetInfo Target)
        : base(Target)
    {
        HostType = ModuleHostType.Reflection;
    }
}
