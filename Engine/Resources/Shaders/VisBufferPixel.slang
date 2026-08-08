#include "Includes/SceneGlobals.slang"

// VisBuffer pixel shader: write the visibility ID to the R32_UINT VisBuffer. Depth comes from SV_Position
// via the bound depth target.


[shader("fragment")]
uint FragmentMain(uint VisID : SV_PrimitiveID) : SV_Target0
{
    return VisID;
}

