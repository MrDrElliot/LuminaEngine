using LuminaBuildTool.Configuration;

/// <summary>
/// Compiles nothing. It exists so the engine's Slang shaders appear in the IDE as an editable
/// tree; the runtime compiles them itself through Slang.
/// </summary>
public class Shaders : ModuleRules
{
    public Shaders(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.HeaderOnly;
        bEnableReflection = false;
        bRootSourceFiles = true;
    }
}
