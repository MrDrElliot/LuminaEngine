using LuminaBuildTool.Configuration;

// Loaded into the Reflector while it generates, and never part of a shipped image.
public class ReflectionExampleGenerator : ModuleRules
{
    public ReflectionExampleGenerator(TargetInfo Target)
        : base(Target)
    {
        HostType = ModuleHostType.Reflection;
    }
}
