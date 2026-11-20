#include "RuntimePCH.h"
#include "SunShadowCache.h"

#include "World/Scene/RenderScene/ScenePrimitiveSet.h"

namespace Lumina
{
    namespace SunShadowCacheInternal
    {
        // A slot that stopped changing this many frames ago joins the cache, so a mover is drawn per frame instead of redrawing shadow pages.
        constexpr uint32 kSettleFrames = 15u;
        // A swayer joins the cache this far past its disable distance and leaves it this far, so the GPU has already stopped its sway.
        constexpr float  kSwayHoldScale    = 1.15f;
        constexpr float  kSwayReleaseScale = 1.05f;
    }

    bool FSunShadowCache::IsCacheable(const FInstanceCullEntry& Entry)
    {
        const EInstanceFlags Flags = (EInstanceFlags)((Entry.DrawIDAndFlags >> 16u) & 0xFFFFu);
        return EnumHasAllFlags(Flags, EInstanceFlags::Active | EInstanceFlags::HasGeometry | EInstanceFlags::CastShadow)
            && (Entry.FlagsAndThinning & INSTANCE_SHADOW_CACHEABLE_BIT) != 0u;
    }

    bool FSunShadowCache::IsSwayCandidate(const FInstanceCullEntry& Entry, const FInstanceStatic& Static)
    {
        const EInstanceFlags Flags = (EInstanceFlags)((Entry.DrawIDAndFlags >> 16u) & 0xFFFFu);
        return EnumHasAllFlags(Flags, EInstanceFlags::Active | EInstanceFlags::HasGeometry | EInstanceFlags::CastShadow)
            && (Entry.FlagsAndThinning & INSTANCE_SHADOW_CACHEABLE_BIT) == 0u
            && Static.WPODisableDistance > 0.0f && Static.SkinnedIndex == kNoSkinnedIndex;
    }

    bool FSunShadowCache::IsStill(const FInstanceCullEntry& Entry, const FInstanceStatic& Static, float DistanceScale) const
    {
        if (IsCacheable(Entry))
        {
            return true;
        }
        if (!IsSwayCandidate(Entry, Static))
        {
            return false;
        }
        const float Reach = Static.WPODisableDistance * DistanceScale;
        const FVector3 ToCamera = FVector3(Entry.SphereBounds) - Camera;
        return Math::Dot(ToCamera, ToCamera) > Reach * Reach;
    }

    void FSunShadowCache::AddSwayCandidate(uint32 Slot, const FScenePrimitiveSet& Primitives)
    {
        if (Slot < (uint32)bSwayCandidate.Num() && bSwayCandidate[Slot] == 0u
            && IsSwayCandidate(Primitives.GetRetainedCullEntries()[Slot], Primitives.GetRetainedStatic()[Slot]))
        {
            bSwayCandidate[Slot] = 1u;
            SwayCandidates.Add(Slot);
        }
    }

    void FSunShadowCache::UpdateSwayCandidates(const FScenePrimitiveSet& Primitives)
    {
        using namespace SunShadowCacheInternal;

        const uint32 SlotCount = Primitives.GetRetainedSlotCount();
        const FInstanceCullEntry* Entries = Primitives.GetRetainedCullEntries();
        const FInstanceStatic*    Statics = Primitives.GetRetainedStatic();

        int32 Kept = 0;
        for (const uint32 Slot : SwayCandidates)
        {
            if (Slot >= SlotCount || !IsSwayCandidate(Entries[Slot], Statics[Slot]))
            {
                if (Slot < (uint32)bSwayCandidate.Num())
                {
                    bSwayCandidate[Slot] = 0u;
                }
                continue;
            }
            SwayCandidates[Kept++] = Slot;

            const bool bHeld = HeldSpheres[Slot].w >= 0.0f;
            if (bHeld && !IsStill(Entries[Slot], Statics[Slot], kSwayReleaseScale))
            {
                Release(Slot);
                MarkChanged(Slot);
            }
            else if (!bHeld && ChangedFrame[Slot] == 0u && IsStill(Entries[Slot], Statics[Slot], kSwayHoldScale))
            {
                MarkChanged(Slot);
            }
        }
        SwayCandidates.SetNum(Kept);
    }

    void FSunShadowCache::Hold(uint32 Slot, const FVector4& Sphere)
    {
        HeldCount += HeldSpheres[Slot].w < 0.0f ? 1u : 0u;
        HeldSpheres[Slot] = Sphere;
        SlotBits[Slot >> 5u] |= 1u << (Slot & 31u);
    }

    void FSunShadowCache::Release(uint32 Slot)
    {
        if (HeldSpheres[Slot].w < 0.0f)
        {
            return;
        }
        Touched.Add(HeldSpheres[Slot]);
        HeldSpheres[Slot].w = -1.0f;
        --HeldCount;
        SlotBits[Slot >> 5u] &= ~(1u << (Slot & 31u));
    }

    void FSunShadowCache::MarkChanged(uint32 Slot)
    {
        if (ChangedFrame[Slot] == 0u)
        {
            Settling.Add(Slot);
        }
        ChangedFrame[Slot] = Frame;
    }

    void FSunShadowCache::HoldAll(const FScenePrimitiveSet& Primitives)
    {
        using namespace SunShadowCacheInternal;

        const uint32 SlotCount = Primitives.GetRetainedSlotCount();
        const FInstanceCullEntry* Entries = Primitives.GetRetainedCullEntries();
        const FInstanceStatic*    Statics = Primitives.GetRetainedStatic();

        SlotBits.Init(0u, (int32)RenderUtils::GetGroupCount(SlotCount, 32u));
        HeldSpheres.Init(FVector4(0.0f, 0.0f, 0.0f, -1.0f), (int32)SlotCount);
        HeldCount = 0u;
        ChangedFrame.Init(0u, (int32)SlotCount);
        bSwayCandidate.Init(0u, (int32)SlotCount);
        Settling.Reset();
        SwayCandidates.Reset();

        for (uint32 Slot = 0; Slot < SlotCount; ++Slot)
        {
            AddSwayCandidate(Slot, Primitives);
            if (IsStill(Entries[Slot], Statics[Slot], kSwayHoldScale))
            {
                Hold(Slot, Entries[Slot].SphereBounds);
            }
        }
        bTouchedAll = true;
    }

    void FSunShadowCache::Resize(uint32 SlotCount)
    {
        for (uint32 Slot = SlotCount; Slot < (uint32)HeldSpheres.Num(); ++Slot)
        {
            Release(Slot);
        }
        const auto Fit = [](auto& Array, uint32 Count, const auto& Fill)
        {
            Array.SetNum((int32)Count, Fill);
            // A trimmed slot range gives its memory back rather than holding the session's peak.
            if ((uint32)Array.Max() > Count * 2u)
            {
                Array.Shrink();
            }
        };
        Fit(HeldSpheres, SlotCount, FVector4(0.0f, 0.0f, 0.0f, -1.0f));
        Fit(ChangedFrame, SlotCount, 0u);
        Fit(bSwayCandidate, SlotCount, (uint8)0u);
        Fit(SlotBits, RenderUtils::GetGroupCount(SlotCount, 32u), 0u);
    }

    void FSunShadowCache::SettleChanged(const FScenePrimitiveSet& Primitives)
    {
        using namespace SunShadowCacheInternal;

        const uint32 SlotCount = Primitives.GetRetainedSlotCount();
        const FInstanceCullEntry* Entries = Primitives.GetRetainedCullEntries();
        const FInstanceStatic*    Statics = Primitives.GetRetainedStatic();

        int32 Kept = 0;
        for (const uint32 Slot : Settling)
        {
            if (Slot >= SlotCount || ChangedFrame[Slot] == 0u)
            {
                continue;
            }
            if (Frame - ChangedFrame[Slot] < kSettleFrames)
            {
                Settling[Kept++] = Slot;
                continue;
            }

            ChangedFrame[Slot] = 0u;
            AddSwayCandidate(Slot, Primitives);
            if (IsStill(Entries[Slot], Statics[Slot], kSwayHoldScale))
            {
                Hold(Slot, Entries[Slot].SphereBounds);
                Touched.Add(Entries[Slot].SphereBounds);
            }
        }
        Settling.SetNum(Kept);
    }

    void FSunShadowCache::Update(const FSlotChanges& Changes, const FScenePrimitiveSet& Primitives, bool bHasSunShadow, const FVector3& InCamera)
    {
        Camera = InCamera;
        // Zero marks a settled slot, so the count skips it on wrap.
        Frame = Frame + 1u == 0u ? 1u : Frame + 1u;

        if (!bHasSunShadow || !bEnabled)
        {
            if (bActive)
            {
                Reset();
            }
            return;
        }

        const uint32 SlotCount = Primitives.GetRetainedSlotCount();
        if (!bActive || Changes.bFull)
        {
            HoldAll(Primitives);
            bActive = true;
        }
        else
        {
            Resize(SlotCount);

            const auto Change = [this, SlotCount](uint32 Slot)
            {
                if (Slot < SlotCount)
                {
                    Release(Slot);
                    MarkChanged(Slot);
                }
            };
            for (const uint32 Slot : Changes.Dirty)       { Change(Slot); }
            for (const uint32 Slot : Changes.DirtyStatic) { Change(Slot); }
            for (const uint32 Slot : Changes.Deactivated) { Change(Slot); }

            // A streamed mover's CPU sphere trails the GPU, so every held or settling slot in its block counts as moved.
            for (const uint32 Block : Changes.StreamedBlocks)
            {
                const uint32 End = Math::Min((Block + 1u) * kInstanceCullBlockSize, SlotCount);
                for (uint32 Slot = Block * kInstanceCullBlockSize; Slot < End; ++Slot)
                {
                    if (HeldSpheres[Slot].w >= 0.0f || ChangedFrame[Slot] != 0u)
                    {
                        Change(Slot);
                    }
                }
            }

            UpdateSwayCandidates(Primitives);
            SettleChanged(Primitives);
        }
    }

    void FSunShadowCache::Reset()
    {
        SlotBits.Reset();
        HeldSpheres.Reset();
        HeldCount = 0u;
        ChangedFrame.Reset();
        Settling.Reset();
        SwayCandidates.Reset();
        bSwayCandidate.Reset();
        Touched.Reset();
        bTouchedAll = true;
        bActive = false;
    }
}
