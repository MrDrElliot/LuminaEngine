#pragma once

#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/StaticArray.h"
#include "SunShadowCache.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

#include <atomic>

namespace Lumina
{
    // Mirror of FWorldDFBrick in WorldDistanceFieldUpdate.slang, one 8x8x8 brick the GPU writes this frame.
    struct FWorldDFBrickGPU
    {
        int32       X = 0;
        int32       Y = 0;
        int32       Z = 0;
        // The level in the low byte, and kWorldDFComposite set when the brick samples its casters rather than clearing to empty.
        uint32      LevelAndMode = 0u;
    };
    static_assert(sizeof(FWorldDFBrickGPU) == 16);

    // Every static distance field caster merged into one camera-centered clipmap, so a trace samples one field instead of every caster it passes.
    class RUNTIME_API FWorldDistanceField
    {
    public:
        static constexpr uint32 kLevels         = 7u;
        static constexpr uint32 kResolution     = 128u;
        static constexpr uint32 kBrickSize      = 8u;
        static constexpr uint32 kBricksPerAxis  = kResolution / kBrickSize;
        static constexpr uint32 kBricksPerLevel = kBricksPerAxis * kBricksPerAxis * kBricksPerAxis;
        static constexpr float  kFirstVoxelSize = 0.25f;
        // Stored distances stop here, which is as far as one trace step can jump.
        static constexpr float  kBandVoxels     = 4.0f;
        static constexpr uint32 kCompositeBudget = 768u;
        static constexpr uint32 kWorldDFComposite = 1u << 8u;
        // A caster under half a voxel of a level only blurs it, so neither the merge nor the stale marking counts it there.
        static constexpr float  kMinCasterVoxels = 0.5f;
        // A mover is marked again once it drifts this share of the finest voxel from where it was merged, which bounds the field's error.
        static constexpr float  kMoveToleranceVoxels = 0.25f;

        static float VoxelSize(uint32 Level) { return kFirstVoxelSize * (float)(1u << Level); }
        static float MinCasterRadius(uint32 Level) { return VoxelSize(Level) * kMinCasterVoxels; }
        static float Band(uint32 Level) { return VoxelSize(Level) * kBandVoxels; }
        static float BrickWorldSize(uint32 Level) { return VoxelSize(Level) * (float)kBrickSize; }

        // What the GPU does with the field this frame.
        struct FFrame
        {
            TVector<FWorldDFBrickGPU> Bricks;
            // Each level's minimum corner then its voxel size, laid out as the shader reads them.
            float  LevelBoxes[kLevels][4] = {};
            // The one level whose bricks sample casters this frame, kLevels when none does.
            uint32 CompositeLevel = kLevels;
            // Every brick is cleared or written this frame, so whatever the textures held before no longer shows.
            bool   bFresh = false;
        };

        // Follows the camera, drops what changed, and picks this frame's bricks. Entries are the retained cull entries the changes index.
        void Update(const FVector3& Camera, const FSunShadowCache::FSlotChanges& Changes, TSpan<const FInstanceCullEntry> Entries, FFrame& Out);

        // Forgets everything written, for a field whose textures were just freed.
        void Reset();

        // Marks the bricks near a world box stale, for a surface outside the retained slots such as a terrain whose heights changed.
        void TouchBox(const FVector3& Min, const FVector3& Max);

        // From the render thread, when it made new textures or could not write a frame's bricks, so the next update starts over.
        void RequestReset() { bResetRequested.store(true, std::memory_order_relaxed); }

        // A brick of a level, by its world brick index, is filled and current.
        bool IsBrickValid(uint32 Level, const FIntVector3& Brick) const;
        uint32 GetPendingCount(uint32 Level) const { return Levels[Level].Pending; }
        FIntVector3 GetOriginBrick(uint32 Level) const { return Levels[Level].OriginBrick; }

    private:
        enum class EBrickState : uint8
        {
            // Holds another place's distances, or nothing yet.
            Invalid,
            // Cleared to empty space, waiting for its casters.
            Cleared,
            // Holds distances from before a caster near it changed.
            Stale,
            Valid,
        };

        struct FLevel
        {
            FIntVector3          OriginBrick = FIntVector3(0);
            TVector<FIntVector3> SlotBrick;
            TVector<EBrickState> State;
            uint32               Pending = 0u;
            uint32               Waiting = 0u;
            bool                 bPlaced = false;
        };

        static constexpr uint32 kMaskWords = kBricksPerLevel / 64u;

        static bool IsCaster(const FInstanceCullEntry& Entry);
        static uint32 SlotOf(const FIntVector3& Brick);

        void Place(uint32 Level, const FVector3& Camera);
        // Sets the bit of every slot of Level within reach of the box, Mask being that level's kMaskWords words.
        void MaskBox(uint32 Level, const FVector3& Min, const FVector3& Max, uint64* Mask) const;
        // Queues the spheres a slot's change uncovers, its old one and its new one, and records the new one as merged.
        void Retrack(uint32 Slot, TSpan<const FInstanceCullEntry> Entries, bool bMoveOnly);
        // Marks the queued spheres' bricks stale, chunks of them in parallel into masks that are then merged.
        void ApplyTouches();
        uint32 PickCompositeLevel();

        TArray<FLevel, kLevels> Levels;
        // The sphere each retained slot was last merged at, radius zero for a slot that is not a caster.
        TVector<FVector4>       MergedSpheres;
        // Bricks waiting to sample their casters, keyed by distance to the camera.
        TVector<uint64>         Candidates;
        // This update's spheres whose bricks go stale, and each chunk's per-level slot masks while they are marked.
        TVector<FVector4>       Touches;
        TVector<uint64>         TouchMasks;
        // Set until the first update records where every caster is.
        bool                    bTrackAll = true;
        std::atomic<bool>       bResetRequested{ false };
    };

    // Mirror of FWorldDistanceField in WorldDistanceField.slang, plain floats since the shader packs the boxes with no padding.
    struct FWorldDistanceFieldGPU
    {
        float  LevelBoxes[FWorldDistanceField::kLevels][4];
        uint32 Levels[FWorldDistanceField::kLevels];
        uint32 NumLevels;
    };
    static_assert(sizeof(FWorldDistanceFieldGPU) == 144);
}
