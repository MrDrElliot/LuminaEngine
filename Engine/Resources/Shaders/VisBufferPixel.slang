#include "Includes/SceneGlobals.slang"

[shader("fragment")]
uint FragmentMain(uint VisID : SV_PrimitiveID) : SV_Target0
{
    return VisID;
}

