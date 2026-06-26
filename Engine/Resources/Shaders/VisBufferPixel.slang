#include "Includes/SceneGlobals.slang"

// VisBuffer pixel shader: write the visibility ID to the R32_UINT VisBuffer. Depth comes from SV_Position
// via the bound depth target.

#ifdef VISBUFFER_PRIMID

[shader("fragment")]
uint FragmentMain(uint VisID : SV_PrimitiveID) : SV_Target0
{
    return VisID;
}

#else

struct FVisInput
{
    nointerpolation uint VisID : VIS_ID;
    float4 Position            : SV_Position;
};

[shader("fragment")]
uint FragmentMain(FVisInput Input) : SV_Target0
{
    return Input.VisID;
}

#endif
