#include "Includes/SceneGlobals.slang"
#include "Includes/VisBufferSurface.slang"

// Pixels the deferred lane does not own write 0, the clear later picker writers load and paint over.

struct FVisBufferPickerArgs
{
    FTexture2DUInt VisBuffer;
    uint           DrawListCount;
};
FVisBufferPickerArgs* Pass() { return GetPassArgs<FVisBufferPickerArgs>(); }

[shader("fragment")]
uint FragmentMain(float4 SvPosition : SV_Position) : SV_Target0
{
    const uint Raw        = Pass().VisBuffer.Load(int2(SvPosition.xy));
    const uint InstanceID = ResolveVisInstance(Raw, Pass().DrawListCount);

    return (InstanceID == ~0u) ? 0u : GetInstanceEntityID(InstanceID);
}
