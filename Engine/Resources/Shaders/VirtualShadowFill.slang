#include "Includes/VirtualShadow.slang"

// One group per listed page, clearing its static layer to the far plane or copying it into the dynamic layer.

struct FPushConstants
{
    GPUSpan<FVirtualShadowPage> Pages;
    GPUSpan<uint>               List;
    GPUSpan<uint>               Counters;
    FRWTexture2DUInt            Pool;
    uint                        CountSlot;
    uint                        bCopy;
    uint                        _Pad0;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

static const uint kTexelsPerThread = (VSM_PAGE_SIZE * VSM_PAGE_SIZE) / VSM_FILL_GROUP_SIZE;

[shader("compute")]
[numthreads(VSM_FILL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint ThreadID : SV_GroupIndex)
{
    if (GroupID.x >= min(Pass().Counters.Get(Pass().CountSlot), Pass().List.Count))
    {
        return;
    }

    const uint  Entry    = Pass().Pages.Get(Pass().List.Get(GroupID.x)).Entry;
    const uint  Physical = Entry & VSM_PAGE_PHYSICAL_MASK;
    const uint2 Static   = VirtualShadowPhysicalPageOrigin(Physical, false);
    const uint2 Dynamic  = VirtualShadowPhysicalPageOrigin(Physical, true);
    const uint  FarDepth = asuint(1.0);

    for (uint i = 0u; i < kTexelsPerThread; ++i)
    {
        const uint  Texel = ThreadID + i * VSM_FILL_GROUP_SIZE;
        const uint2 Local = uint2(Texel % VSM_PAGE_SIZE, Texel / VSM_PAGE_SIZE);
        if (Pass().bCopy != 0u)
        {
            Pass().Pool[Dynamic + Local] = Pass().Pool[Static + Local];
        }
        else
        {
            Pass().Pool[Static + Local] = FarDepth;
        }
    }
}
