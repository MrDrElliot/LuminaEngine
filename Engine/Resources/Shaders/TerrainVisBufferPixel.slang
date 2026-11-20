#include "Includes/MeshletGeometry.slang"

// The terrain vertex path draws indexed, so a vertex cannot know its triangle and the primitive ID supplies it here.
[shader("fragment")]
uint FragmentMain(nointerpolation uint VisBase : TERRAIN_VIS_BASE, uint Primitive : SV_PrimitiveID) : SV_Target0
{
    return VisBase + Primitive;
}
