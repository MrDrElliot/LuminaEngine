using LuminaBuildTool.Configuration;

public class $PROJECTNAME : LuminaGameModuleRules
{
    public $PROJECTNAME(TargetInfo Target)
        : base(Target)
    {
        // Generated bindings go to this project's own script assembly rather than LuminaSharp.
        CSharpBindingsDirectory = ModulePath("Game/Scripts/Generated");

        // Add extra engine or third-party modules here, for example:
        // PrivateDependencyModuleNames.Add("JoltPhysics");
    }
}
