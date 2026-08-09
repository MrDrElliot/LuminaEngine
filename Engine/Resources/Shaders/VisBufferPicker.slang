#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"

// Editor entity picking, resolved straight out of the visibility buffer.
//
// The entity id is a property of the geometry, not of the material, so it has no business in the GBuffer
// or in the per-material passes: this is one full-screen resolve of VisID -> draw -> instance -> EntityID.
// Pixels the deferred lane does not own write 0, which is also the clear the later picker writers
// (terrain, billboards, text, widgets) load and paint over.

struct FVisBufferPickerArgs
{
    uint VisBufferIndex;
    uint DrawListCount;
};
FVisBufferPickerArgs* Pass() { return GetPassArgs<FVisBufferPickerArgs>(); }

[shader("fragment")]
uint FragmentMain(float4 SvPosition : SV_Position) : SV_Target0
{
    const uint Raw        = LoadTexture2DUInt(Pass().VisBufferIndex, int2(SvPosition.xy));
    const uint InstanceID = ResolveVisInstance(Raw, Pass().DrawListCount);

    return (InstanceID == ~0u) ? 0u : GetInstance(InstanceID).EntityID;
}
