#pragma once

#include "Containers/Span.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

#include <atomic>

namespace Lumina
{
    class FScenePrimitiveSet;

    // Which retained slots the sun's static shadow pages hold, and the spheres where a held caster changed so the pages under them draw again.
    class FSunShadowCache
    {
    public:
        // What one frame changed in the retained slots, as the retained upload saw it.
        struct FSlotChanges
        {
            TSpan<const uint32> Dirty;
            TSpan<const uint32> DirtyStatic;
            TSpan<const uint32> Deactivated;
            // Blocks of kInstanceCullBlockSize slots holding a mover that skipped the CPU copies.
            TSpan<const uint32> StreamedBlocks;
            bool                bFull = false;
        };

        // Set by the sun's extract ahead of this frame's update.
        void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }

        // Camera is where the GPU measures each instance's sway-off distance from, so a swaying caster far past it holds still.
        void Update(const FSlotChanges& Changes, const FScenePrimitiveSet& Primitives, bool bHasSunShadow, const FVector3& Camera);

        // The spheres whose static shadow went stale since the last call, or everything when the whole cache was rebuilt.
        void TakeTouches(TVector<FVector4>& OutSpheres, bool& bOutAll)
        {
            OutSpheres = Move(Touched);
            Touched.Reset();
            bOutAll = bTouchedAll || bInvalidateRequested.exchange(false, std::memory_order_relaxed);
            bTouchedAll = false;
        }

        // From the render thread, when static pages were drawn while a caster's pipeline was still compiling.
        void RequestInvalidateAll() { bInvalidateRequested.store(true, std::memory_order_relaxed); }

        bool IsActive() const { return bActive; }
        uint32 GetHeldCount() const { return HeldCount; }

        // One bit per retained slot the cache holds, the split every shadow level's static and dynamic views cull against.
        const TVector<uint32>& GetSlotBits() const { return SlotBits; }

        void Reset();

    private:
        static bool IsCacheable(const FInstanceCullEntry& Entry);
        // A caster whose only motion is its material's sway, which the GPU turns off past the instance's disable distance.
        static bool IsSwayCandidate(const FInstanceCullEntry& Entry, const FInstanceStatic& Static);
        bool IsStill(const FInstanceCullEntry& Entry, const FInstanceStatic& Static, float DistanceScale) const;
        void AddSwayCandidate(uint32 Slot, const FScenePrimitiveSet& Primitives);
        // Sends a held swayer that came near back to the per-frame pages, and a far one toward the cache.
        void UpdateSwayCandidates(const FScenePrimitiveSet& Primitives);

        void HoldAll(const FScenePrimitiveSet& Primitives);
        void Resize(uint32 SlotCount);
        void Hold(uint32 Slot, const FVector4& Sphere);
        void Release(uint32 Slot);
        void MarkChanged(uint32 Slot);
        void SettleChanged(const FScenePrimitiveSet& Primitives);

        TVector<uint32>   SlotBits;
        // The sphere each held slot was drawn at, so a slot that moves clears the pages holding its old shadow.
        TVector<FVector4> HeldSpheres;
        // The frame a slot last changed, 0 while it is held or ineligible.
        TVector<uint32>   ChangedFrame;
        TVector<uint32>   Settling;
        TVector<uint32>   SwayCandidates;
        TVector<uint8>    bSwayCandidate;
        FVector3          Camera = FVector3(0.0f);

        TVector<FVector4>   Touched;
        bool                bTouchedAll = false;
        std::atomic<bool>   bInvalidateRequested{ false };

        uint32              Frame = 0;
        uint32              HeldCount = 0;
        bool                bEnabled = false;
        bool                bActive = false;
    };
}
