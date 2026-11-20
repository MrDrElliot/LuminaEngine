#include "Includes/VirtualShadow.slang"

// The sun's page table work, one step per dispatch in frame order, then the masks the cull reads.

static const uint kModeReset        = 0u;
static const uint kModeInvalidate   = 1u;
static const uint kModeRelease      = 2u;
static const uint kModeAllocate     = 3u;
static const uint kModeFinish       = 4u;
static const uint kModeMaskBase     = 5u;
static const uint kModeMaskMip      = 6u;
static const uint kModeInitialize   = 7u;
static const uint kModeCount        = 8u;

// Counters, the free stack's size first and the lists' lengths after it.
static const uint kFreeCount        = 0u;
static const uint kConsumed         = 1u;
static const uint kStaticCount      = 2u;
static const uint kRequestedCount   = 3u;
static const uint kAllocFailures    = 4u;
// Requested pages without a physical page, counted before the release so it knows whether the pool will run out.
static const uint kNeeded           = 5u;
static const uint kFreeAtReset      = 6u;
// Meshlets the shadow cull handed to the compute raster this frame, the static layer's then the dynamic layer's.
static const uint kComputeRasterCount = 7u;
static const uint kComputeRasterDynamicCount = 8u;
// Frames a page may go unrequested once the pool is short, so a fast turn reclaims what it left behind at once.
static const uint kPressuredEvictFrames = 1u;

struct FPushConstants
{
    GPUSpan<FVirtualShadowPage>  Pages;
    GPUSpan<FVirtualShadowLevel> Levels;
    GPUSpan<uint>                Masks;
    GPUSpan<uint>                FreeStack;
    GPUSpan<uint>                Counters;
    GPUSpan<uint>                StaticList;
    GPUSpan<uint>                RequestedList;
    GPUSpan<float4>              Spheres;
    // Two indirect dispatches of one group per page, the static clear and then the dynamic copy.
    GPUSpan<uint4>               FillArgs;
    uint                         Mode;
    uint                         Frame;
    uint                         PoolPages;
    uint                         Mip;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// The global page a wrapped slot stands for, the one inside the level's current window.
int2 GlobalPageOfSlot(FVirtualShadowLevel Lv, uint Local)
{
    const int2 Wrapped = int2(Local % VSM_LEVEL_PAGES, Local / VSM_LEVEL_PAGES);
    return Lv.PageOrigin + ((Wrapped - Lv.PageOrigin) & int2(VSM_LEVEL_PAGES - 1u, VSM_LEVEL_PAGES - 1u));
}

void FreePhysical(uint Physical)
{
    uint Slot;
    InterlockedAdd(Pass().Counters.At(kFreeCount)[0], 1u, Slot);
    Pass().FreeStack.Set(Slot, Physical);
}

void Invalidate(uint Thread)
{
    const uint Sphere = Thread / VSM_MAX_LEVELS;
    const uint Level  = Thread % VSM_MAX_LEVELS;
    if (Sphere >= Pass().Spheres.Count)
    {
        return;
    }
    const FVirtualShadowLevel Lv = Pass().Levels.GetAligned(Level);
    if (Lv.bActive == 0u)
    {
        return;
    }

    const float4 S   = Pass().Spheres.Get(Sphere);
    const float3 Ndc = mul(Lv.ViewProjection, float4(S.xyz, 1.0)).xyz;
    const float2 R   = S.w * float2(length(Lv.ViewProjection[0].xyz), length(Lv.ViewProjection[1].xyz));
    const float2 MinTexel = ((Ndc.xy - R) * 0.5 + 0.5) * float(VSM_VIRTUAL_SIZE);
    const float2 MaxTexel = ((Ndc.xy + R) * 0.5 + 0.5) * float(VSM_VIRTUAL_SIZE);
    if (any(MaxTexel < 0.0) || any(MinTexel >= float(VSM_VIRTUAL_SIZE)))
    {
        return;
    }

    // Shadows fall away from the sun, so a caster's old shadow can reach any page its box spans along the light.
    const int2 Lo = clamp(int2(floor(MinTexel)) >> int(VSM_PAGE_SHIFT), int2(0, 0), int2(VSM_LEVEL_PAGES - 1u, VSM_LEVEL_PAGES - 1u));
    const int2 Hi = clamp(int2(floor(MaxTexel)) >> int(VSM_PAGE_SHIFT), int2(0, 0), int2(VSM_LEVEL_PAGES - 1u, VSM_LEVEL_PAGES - 1u));
    for (int y = Lo.y; y <= Hi.y; ++y)
    {
        for (int x = Lo.x; x <= Hi.x; ++x)
        {
            const uint Index = VirtualShadowPageIndex(Level, Lv.PageOrigin + int2(x, y));
            InterlockedOr(Pass().Pages.At(Index).State, VSM_STATE_STATIC_INVALID);
        }
    }
}

void Release(uint Index)
{
    const uint Level = Index / VSM_PAGES_PER_LEVEL;
    const FVirtualShadowLevel Lv = Pass().Levels.GetAligned(Level);
    FVirtualShadowPage Page = Pass().Pages.Get(Index);
    Page.Entry &= ~(VSM_PAGE_REQUESTED | VSM_PAGE_RENDER_STATIC);

    if ((Page.Entry & VSM_PAGE_ALLOCATED) != 0u)
    {
        const bool bRequested = Lv.bActive != 0u && Page.LastRequested == Pass().Frame;
        const uint Tag        = VirtualShadowPageTag(GlobalPageOfSlot(Lv, Index % VSM_PAGES_PER_LEVEL));
        const bool bStale     = Page.Tag != Tag || Lv.bInvalidate != 0u;
        const bool bPressured = Pass().Counters.Get(kNeeded) > Pass().Counters.Get(kFreeAtReset);
        const uint EvictFrames = bPressured ? kPressuredEvictFrames : VSM_EVICT_FRAMES;
        const bool bAged      = Pass().Frame - Page.LastRequested > EvictFrames;
        if (bStale && bRequested)
        {
            // Asked for again in its new place, so it keeps its physical page and draws afresh.
            Page.Tag   = Tag;
            Page.State = VSM_FRESH_FRAMES << VSM_STATE_FRESH_SHIFT;
        }
        else if (bStale || (!bRequested && bAged))
        {
            FreePhysical(Page.Entry & VSM_PAGE_PHYSICAL_MASK);
            Page.Entry = 0u;
            Page.State = 0u;
        }
    }
    Pass().Pages.Set(Index, Page);
}

void CountNeeded(uint Index)
{
    const FVirtualShadowLevel Lv = Pass().Levels.GetAligned(Index / VSM_PAGES_PER_LEVEL);
    const FVirtualShadowPage Page = Pass().Pages.Get(Index);
    if (Lv.bActive != 0u && Page.LastRequested == Pass().Frame && (Page.Entry & VSM_PAGE_ALLOCATED) == 0u)
    {
        InterlockedAdd(Pass().Counters.At(kNeeded)[0], 1u);
    }
}

void Allocate(uint Index)
{
    const uint Level = Index / VSM_PAGES_PER_LEVEL;
    const FVirtualShadowLevel Lv = Pass().Levels.GetAligned(Level);
    FVirtualShadowPage Page = Pass().Pages.Get(Index);
    if (Lv.bActive == 0u || Page.LastRequested != Pass().Frame)
    {
        return;
    }

    if ((Page.Entry & VSM_PAGE_ALLOCATED) == 0u)
    {
        uint Taken;
        InterlockedAdd(Pass().Counters.At(kConsumed)[0], 1u, Taken);
        const uint Free = Pass().Counters.Get(kFreeCount);
        if (Taken >= Free)
        {
            InterlockedAdd(Pass().Counters.At(kAllocFailures)[0], 1u);
            return;
        }
        Page.Entry = Pass().FreeStack.Get(Free - 1u - Taken) | VSM_PAGE_ALLOCATED;
        Page.Tag   = VirtualShadowPageTag(GlobalPageOfSlot(Lv, Index % VSM_PAGES_PER_LEVEL));
        Page.State = VSM_FRESH_FRAMES << VSM_STATE_FRESH_SHIFT;
    }

    Page.Entry |= VSM_PAGE_REQUESTED;

    // A fresh page draws twice, since the first frame's casters were culled against last frame's pages and may miss some.
    const uint Fresh = (Page.State & VSM_STATE_FRESH_MASK) >> VSM_STATE_FRESH_SHIFT;
    if (Fresh > 0u || (Page.State & VSM_STATE_STATIC_INVALID) != 0u)
    {
        Page.Entry |= VSM_PAGE_RENDER_STATIC;
        Page.State  = (Fresh > 0u ? (Fresh - 1u) << VSM_STATE_FRESH_SHIFT : 0u);
        uint Slot;
        InterlockedAdd(Pass().Counters.At(kStaticCount)[0], 1u, Slot);
        Pass().StaticList.Set(Slot, Index);
    }

    uint Slot;
    InterlockedAdd(Pass().Counters.At(kRequestedCount)[0], 1u, Slot);
    Pass().RequestedList.Set(Slot, Index);
    Pass().Pages.Set(Index, Page);
}

uint MaskBits(uint Level, int2 Wrapped)
{
    const uint Entry = Pass().Pages.Get(Level * VSM_PAGES_PER_LEVEL + uint(Wrapped.y) * VSM_LEVEL_PAGES + uint(Wrapped.x)).Entry;
    return ((Entry & VSM_PAGE_RENDER_STATIC) != 0u ? VSM_MASK_STATIC : 0u) | ((Entry & VSM_PAGE_REQUESTED) != 0u ? VSM_MASK_REQUESTED : 0u);
}

void MaskBase(uint Index)
{
    const uint Level = Index / VSM_PAGES_PER_LEVEL;
    const uint Local = Index % VSM_PAGES_PER_LEVEL;
    const int2 Wrapped = int2(Local % VSM_LEVEL_PAGES, Local / VSM_LEVEL_PAGES);

    uint Bits = MaskBits(Level, Wrapped);
    // Next frame's instance cull reads the requested pages grown by two, so a moving camera finds its casters already kept.
    bool bHot = false;
    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            bHot = bHot || (MaskBits(Level, (Wrapped + int2(dx, dy)) & int2(VSM_LEVEL_PAGES - 1u, VSM_LEVEL_PAGES - 1u)) & VSM_MASK_REQUESTED) != 0u;
        }
    }
    Bits |= bHot ? VSM_MASK_HOT : 0u;
    Pass().Masks.Set(Level * VSM_MASK_CELLS_PER_LEVEL + Local, Bits);
}

void MaskMip(uint Thread)
{
    const uint Side   = VSM_LEVEL_PAGES >> Pass().Mip;
    const uint Cells  = Side * Side;
    const uint Level  = Thread / Cells;
    const uint Cell   = Thread % Cells;
    const uint2 XY    = uint2(Cell % Side, Cell / Side);
    const uint Parent = Level * VSM_MASK_CELLS_PER_LEVEL + VirtualShadowMaskMipOffset(Pass().Mip - 1u);
    const uint ParentSide = Side * 2u;
    uint Bits = 0u;
    for (uint y = 0u; y < 2u; ++y)
    {
        for (uint x = 0u; x < 2u; ++x)
        {
            Bits |= Pass().Masks.Get(Parent + (XY.y * 2u + y) * ParentSide + XY.x * 2u + x);
        }
    }
    Pass().Masks.Set(Level * VSM_MASK_CELLS_PER_LEVEL + VirtualShadowMaskMipOffset(Pass().Mip) + Cell, Bits);
}

[shader("compute")]
[numthreads(VSM_PAGE_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint Thread = DTid.x + DTid.y * (uint)MAX_DISPATCH_AXIS * VSM_PAGE_GROUP_SIZE;
    const uint Mode   = Pass().Mode;
    const uint Pages  = VSM_MAX_LEVELS * VSM_PAGES_PER_LEVEL;

    if (Mode == kModeInitialize)
    {
        if (Thread < Pages)
        {
            Pass().Pages.Set(Thread, (FVirtualShadowPage)0);
        }
        if (Thread < Pass().PoolPages)
        {
            Pass().FreeStack.Set(Thread, Thread);
        }
        if (Thread == 0u)
        {
            Pass().Counters.Set(kFreeCount, Pass().PoolPages);
        }
    }
    else if (Mode == kModeReset)
    {
        if (Thread == 0u)
        {
            Pass().Counters.Set(kConsumed, 0u);
            Pass().Counters.Set(kStaticCount, 0u);
            Pass().Counters.Set(kRequestedCount, 0u);
            Pass().Counters.Set(kAllocFailures, 0u);
            Pass().Counters.Set(kNeeded, 0u);
            Pass().Counters.Set(kFreeAtReset, Pass().Counters.Get(kFreeCount));
            Pass().Counters.Set(kComputeRasterCount, 0u);
            Pass().Counters.Set(kComputeRasterDynamicCount, 0u);
        }
    }
    else if (Mode == kModeInvalidate)
    {
        Invalidate(Thread);
    }
    else if (Mode == kModeCount)
    {
        if (Thread < Pages)
        {
            CountNeeded(Thread);
        }
    }
    else if (Mode == kModeRelease)
    {
        if (Thread < Pages)
        {
            Release(Thread);
        }
    }
    else if (Mode == kModeAllocate)
    {
        if (Thread < Pages)
        {
            Allocate(Thread);
        }
    }
    else if (Mode == kModeFinish)
    {
        if (Thread == 0u)
        {
            const uint Free     = Pass().Counters.Get(kFreeCount);
            const uint Consumed = Pass().Counters.Get(kConsumed);
            Pass().Counters.Set(kFreeCount, Free - min(Consumed, Free));
            Pass().FillArgs.Set(0u, uint4(min(Pass().Counters.Get(kStaticCount), Pass().StaticList.Count), 1u, 1u, 0u));
            Pass().FillArgs.Set(1u, uint4(min(Pass().Counters.Get(kRequestedCount), Pass().RequestedList.Count), 1u, 1u, 0u));
        }
    }
    else if (Mode == kModeMaskBase)
    {
        if (Thread < Pages)
        {
            MaskBase(Thread);
        }
    }
    else if (Mode == kModeMaskMip)
    {
        const uint Side = VSM_LEVEL_PAGES >> Pass().Mip;
        if (Thread < VSM_MAX_LEVELS * Side * Side)
        {
            MaskMip(Thread);
        }
    }
}
