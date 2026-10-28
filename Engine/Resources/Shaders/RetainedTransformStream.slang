#pragma once
#include "Includes/SceneGlobals.slang"

// Scatters the transforms the extract streamed for moved instances into the retained buffers, after this frame's CPU copies went up.

static const uint LOCAL_SIZE_X  = 64u;
static const uint kNoStreamSlot = 0xFFFFFFFFu;

// Mirror of Lumina::FTransformStreamRecord.
struct FTransformStreamRecord
{
    FTransform3x4 Transform;
    float4        SphereBounds;
};

// Mirror of Lumina::FTransformStreamArgs.
struct FPushConstants
{
    GPUSpan<FTransformStreamRecord> Records;
    GPUSpan<uint>                   Slots;
    GPUSpan<FTransform3x4>          RetainedTransforms;
    GPUSpan<FInstanceCullEntry>     RetainedCullEntries;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint3 LocalID : SV_GroupThreadID)
{
    const uint Record = (GroupID.x + GroupID.y * MAX_DISPATCH_AXIS) * LOCAL_SIZE_X + LocalID.x;
    if (!Pass().Slots.Contains(Record) || !Pass().Records.Contains(Record))
    {
        return;
    }

    const uint Slot = Pass().Slots.Get(Record);
    if (Slot == kNoStreamSlot || !Pass().RetainedTransforms.Contains(Slot) || !Pass().RetainedCullEntries.Contains(Slot))
    {
        return;
    }

    const FTransformStreamRecord Moved = Pass().Records.Get(Record);
    Pass().RetainedTransforms.Set(Slot, Moved.Transform);
    Pass().RetainedCullEntries.At(Slot).SphereBounds = Moved.SphereBounds;
}
