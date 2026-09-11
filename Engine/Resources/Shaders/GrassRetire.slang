// Deactivates [Cursor, PrevCursor) after a grass scatter, the only slots a smaller append leaves stale.

#include "Includes/SceneGlobals.slang"

#define LOCAL_SIZE_X 64

struct FGrassRetirePushConstants
{
    // Sliced to the species block, so an index here is block-local.
    GPUSpan<FInstanceCullEntry> CullEntries;
    GPUSpan<uint>               Cursor;       // this frame's append count
    GPUSpan<uint>               PrevCursor;   // last frame's, copied from Cursor after this pass
};
FGrassRetirePushConstants* Pass() { return GetPassArgs<FGrassRetirePushConstants>(); }

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void main(uint3 Tid : SV_DispatchThreadID)
{
    FGrassRetirePushConstants* P = Pass();

    const uint Slot = Tid.x;
    const uint Live = SpanGet(&P->Cursor, 0u);
    const uint Prev = SpanGet(&P->PrevCursor, 0u);

    if (Slot < Live || Slot >= Prev || !SpanContains(&P->CullEntries, Slot))
    {
        return;
    }

    FInstanceCullEntry Cull = SpanGet(&P->CullEntries, Slot);
    Cull.DrawIDAndFlags &= ~((uint)EInstanceFlags::Active << 16);
    SpanSet(&P->CullEntries, Slot, Cull);
}
