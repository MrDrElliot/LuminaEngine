// Deactivates [Cursor, PrevCursor) after a grass scatter, the only slots a smaller append leaves stale, and marks the block's cull groups past the cursor empty.

#include "Includes/SceneGlobals.slang"

#define LOCAL_SIZE_X 64

struct FGrassRetirePushConstants
{
    // Sliced to the species block, so an index here is block-local.
    GPUSpan<FInstanceCullEntry> CullEntries;
    GPUSpan<uint>               Cursor;       // this frame's append count
    GPUSpan<uint>               PrevCursor;   // last frame's, copied from Cursor after this pass
    // The whole scene's, empty when the cull runs without them.
    GPUSpan<FInstanceBlockBounds> BlockBounds;
    uint                        SlotBase;
    uint                        Capacity;
    uint                        _Pad0;
    uint                        _Pad1;
};
FGrassRetirePushConstants* Pass() { return GetPassArgs<FGrassRetirePushConstants>(); }

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void main(uint3 Tid : SV_DispatchThreadID)
{
    FGrassRetirePushConstants* P = Pass();

    const uint Slot = Tid.x;
    const uint Live = (P->Cursor).Get(0u);
    const uint Prev = (P->PrevCursor).Get(0u);

    // Only cull groups wholly inside the species block, since a shared one also holds slots the CPU bounds.
    const uint FirstGroup = (P->SlotBase + INSTANCE_CULL_BLOCK_SIZE - 1u) / INSTANCE_CULL_BLOCK_SIZE;
    const uint EndGroup   = (P->SlotBase + P->Capacity) / INSTANCE_CULL_BLOCK_SIZE;
    const uint Group      = FirstGroup + Slot;
    if (Group < EndGroup && (P->BlockBounds).Contains(Group))
    {
        // A group the cursor never reached holds no blade, so the cull skips it whole instead of testing every slot.
        const bool bLive = Group * INSTANCE_CULL_BLOCK_SIZE < P->SlotBase + Live;
        FInstanceBlockBounds Bounds;
        Bounds.Min         = float3(0.0, 0.0, 0.0);
        Bounds.Max         = float3(0.0, 0.0, 0.0);
        Bounds.Reach       = bLive ? 0.0 : -1.0;
        Bounds.bAlwaysScan = bLive ? 1u : 0u;
        (P->BlockBounds).Set(Group, Bounds);
    }

    if (Slot < Live || Slot >= Prev || !(P->CullEntries).Contains(Slot))
    {
        return;
    }

    FInstanceCullEntry Cull = (P->CullEntries).Get(Slot);
    Cull.DrawIDAndFlags &= ~((uint)EInstanceFlags::Active << 16);
    (P->CullEntries).Set(Slot, Cull);
}
