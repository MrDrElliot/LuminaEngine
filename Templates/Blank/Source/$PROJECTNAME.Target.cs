using LuminaBuildTool.Configuration;

public class $PROJECTNAMETarget : LuminaGameTargetRules
{
    public $PROJECTNAMETarget(TargetInfo Target)
        : base(Target)
    {
        LaunchModuleName = "$PROJECTNAME";

        // This target builds a library the editor loads, so Run and Debug launch the editor with
        // this project already open rather than trying to execute the library.
        SetProjectFileToOpen("../$PROJECTNAME.lproject");
    }
}
