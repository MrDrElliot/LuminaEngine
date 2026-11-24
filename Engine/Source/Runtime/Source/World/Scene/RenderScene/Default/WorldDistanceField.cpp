#include "RuntimePCH.h"
#include "WorldDistanceField.h"

#include "Containers/Algorithm.h"
#include "Memory/Memcpy.h"
#include "Memory/Memory.h"
#include "TaskSystem/TaskSystem.h"

#include <cmath>

namespace Lumina
{
    namespace WorldDistanceFieldInternal
    {
        // A level recenters once the camera is this many bricks off center, so walking along a brick edge does not refill a slab every frame.
        constexpr int32 kRecenterBricks = 2;
        // A sphere this large is a broken transform rather than a caster, and its brick range would cover every level whole.
        constexpr float kMaxCasterRadius = 1.0e5f;
        // Spheres per marking task, enough that a chunk's masks cost less than its spheres.
        constexpr uint32 kTouchChunk = 512u;

        int32 PositiveMod(int32 Value, int32 Divisor)
        {
            const int32 Remainder = Value % Divisor;
            return Remainder < 0 ? Remainder + Divisor : Remainder;
        }

        FIntVector3 BrickAt(const FVector3& Position, float BrickWorld)
        {
            return FIntVector3((int32)std::floor(Position.x / BrickWorld), (int32)std::floor(Position.y / BrickWorld), (int32)std::floor(Position.z / BrickWorld));
        }

        FVector3 BrickCorner(const FIntVector3& Brick, float BrickWorld)
        {
            return FVector3((float)Brick.x, (float)Brick.y, (float)Brick.z) * BrickWorld;
        }

        bool IsUsableSphere(const FVector4& Sphere)
        {
            return std::isfinite(Sphere.x) && std::isfinite(Sphere.y) && std::isfinite(Sphere.z) && std::isfinite(Sphere.w)
                && Sphere.w > 0.0f && Sphere.w < kMaxCasterRadius;
        }
    }

    bool FWorldDistanceField::IsCaster(const FInstanceCullEntry& Entry)
    {
        const EInstanceFlags Flags = (EInstanceFlags)((Entry.DrawIDAndFlags >> 16u) & 0xFFFFu);
        return EnumHasAllFlags(Flags, EInstanceFlags::Active | EInstanceFlags::HasGeometry | EInstanceFlags::CastShadow | EInstanceFlags::HasDistanceField)
            && !EnumHasAnyFlags(Flags, EInstanceFlags::Skinned)
            && WorldDistanceFieldInternal::IsUsableSphere(Entry.SphereBounds);
    }

    uint32 FWorldDistanceField::SlotOf(const FIntVector3& Brick)
    {
        using namespace WorldDistanceFieldInternal;
        constexpr int32 Axis = (int32)kBricksPerAxis;
        return (uint32)(PositiveMod(Brick.x, Axis) + PositiveMod(Brick.y, Axis) * Axis + PositiveMod(Brick.z, Axis) * Axis * Axis);
    }

    bool FWorldDistanceField::IsBrickValid(uint32 Level, const FIntVector3& Brick) const
    {
        const FLevel& Lv = Levels[Level];
        if (!Lv.bPlaced)
        {
            return false;
        }
        const uint32 Slot = SlotOf(Brick);
        return Lv.SlotBrick[Slot] == Brick && Lv.State[Slot] == EBrickState::Valid;
    }

    void FWorldDistanceField::Reset()
    {
        for (FLevel& Lv : Levels)
        {
            Lv = FLevel();
        }
        MergedSpheres.Reset();
        bTrackAll = true;
    }

    void FWorldDistanceField::Place(uint32 Level, const FVector3& Camera)
    {
        using namespace WorldDistanceFieldInternal;

        FLevel& Lv = Levels[Level];
        constexpr int32 Axis = (int32)kBricksPerAxis;
        const FIntVector3 Center = BrickAt(Camera, BrickWorldSize(Level));
        const FIntVector3 Half(Axis / 2);

        if (Lv.bPlaced)
        {
            const FIntVector3 Offset = Center - (Lv.OriginBrick + Half);
            if (Math::Max(Math::Abs(Offset.x), Math::Max(Math::Abs(Offset.y), Math::Abs(Offset.z))) < kRecenterBricks)
            {
                return;
            }
        }
        else
        {
            Lv.SlotBrick.SetNum(kBricksPerLevel, FIntVector3(INT32_MIN));
            Lv.State.SetNum(kBricksPerLevel, EBrickState::Invalid);
            Lv.bPlaced = true;
        }

        // Each slot holds whichever brick of the new window wraps onto it, so only the bricks that scrolled in lose their distances.
        Lv.OriginBrick = Center - Half;
        Lv.Pending = 0u;
        for (int32 Z = 0; Z < Axis; ++Z)
        {
            for (int32 Y = 0; Y < Axis; ++Y)
            {
                for (int32 X = 0; X < Axis; ++X)
                {
                    const FIntVector3 Brick(Lv.OriginBrick.x + PositiveMod(X - Lv.OriginBrick.x, Axis),
                                            Lv.OriginBrick.y + PositiveMod(Y - Lv.OriginBrick.y, Axis),
                                            Lv.OriginBrick.z + PositiveMod(Z - Lv.OriginBrick.z, Axis));
                    const uint32 Slot = (uint32)(X + Y * Axis + Z * Axis * Axis);
                    if (Lv.SlotBrick[Slot] != Brick)
                    {
                        Lv.SlotBrick[Slot] = Brick;
                        Lv.State[Slot]     = EBrickState::Invalid;
                    }
                    Lv.Pending += Lv.State[Slot] != EBrickState::Valid ? 1u : 0u;
                }
            }
        }
    }

    void FWorldDistanceField::MaskBox(uint32 Level, const FVector3& Min, const FVector3& Max, uint64* Mask) const
    {
        using namespace WorldDistanceFieldInternal;

        const FLevel& Lv = Levels[Level];
        // A voxel within the band of a surface stored its distance, and trilinear reads reach one voxel further.
        const float Reach = Band(Level) + VoxelSize(Level);
        const float BrickWorld = BrickWorldSize(Level);
        const FIntVector3 Last = Lv.OriginBrick + FIntVector3((int32)kBricksPerAxis - 1);
        const FIntVector3 Lo = Math::Max(BrickAt(Min - FVector3(Reach), BrickWorld), Lv.OriginBrick);
        const FIntVector3 Hi = Math::Min(BrickAt(Max + FVector3(Reach), BrickWorld), Last);
        // Clamped to the window, where every brick owns its slot, so a slot needs no check that it holds this brick.
        for (int32 Z = Lo.z; Z <= Hi.z; ++Z)
        {
            for (int32 Y = Lo.y; Y <= Hi.y; ++Y)
            {
                for (int32 X = Lo.x; X <= Hi.x; ++X)
                {
                    const uint32 Slot = SlotOf(FIntVector3(X, Y, Z));
                    Mask[Slot >> 6u] |= 1ull << (Slot & 63u);
                }
            }
        }
    }

    void FWorldDistanceField::TouchBox(const FVector3& Min, const FVector3& Max)
    {
        Touches.Add(FVector4((Min + Max) * 0.5f, Math::Length(Max - Min) * 0.5f));
    }

    void FWorldDistanceField::Retrack(uint32 Slot, TSpan<const FInstanceCullEntry> Entries, bool bMoveOnly)
    {
        if (Slot >= (uint32)MergedSpheres.Num())
        {
            MergedSpheres.SetNum(Math::Max((uint32)Entries.Num(), Slot + 1u), FVector4(0.0f));
        }
        FVector4& Merged = MergedSpheres[Slot];
        const bool bCaster = Slot < (uint32)Entries.Num() && IsCaster(Entries[Slot]);
        const FVector4 Current = bCaster ? Entries[Slot].SphereBounds : FVector4(0.0f);
        if (bMoveOnly && bCaster && Merged.w > 0.0f)
        {
            const float Drift = Math::Length(FVector3(Current) - FVector3(Merged)) + Math::Abs(Current.w - Merged.w);
            if (Drift < kMoveToleranceVoxels * VoxelSize(0))
            {
                return;
            }
        }
        if (Merged.w > 0.0f)
        {
            Touches.Add(Merged);
        }
        Merged = Current;
        if (bCaster)
        {
            Touches.Add(Current);
        }
    }

    void FWorldDistanceField::ApplyTouches()
    {
        LUMINA_PROFILE_SCOPE();
        using namespace WorldDistanceFieldInternal;

        const uint32 NumTouches = (uint32)Touches.Num();
        if (NumTouches == 0u || !Levels[0].bPlaced)
        {
            Touches.Reset();
            return;
        }

        const uint32 NumChunks = (NumTouches + kTouchChunk - 1u) / kTouchChunk;
        const uint32 ChunkWords = kLevels * kMaskWords;
        TouchMasks.SetNum(NumChunks * ChunkWords);
        Jobs::ParallelForOrSerial(NumChunks, 2u, [this, NumTouches, ChunkWords](uint32 Chunk)
        {
            uint64* Masks = TouchMasks.GetData() + (size_t)Chunk * ChunkWords;
            Memory::Memzero(Masks, ChunkWords * sizeof(uint64));
            const uint32 End = Math::Min(NumTouches, (Chunk + 1u) * kTouchChunk);
            for (uint32 Index = Chunk * kTouchChunk; Index < End; ++Index)
            {
                const FVector4& Sphere = Touches[Index];
                const FVector3 Center(Sphere.x, Sphere.y, Sphere.z);
                for (uint32 Level = 0; Level < kLevels && Sphere.w >= MinCasterRadius(Level); ++Level)
                {
                    MaskBox(Level, Center - FVector3(Sphere.w), Center + FVector3(Sphere.w), Masks + Level * kMaskWords);
                }
            }
        });

        // Merged per level in parallel, each level owning its own states and count.
        Jobs::ParallelFor(kLevels, [this, NumChunks, ChunkWords](uint32 Level)
        {
            FLevel& Lv = Levels[Level];
            for (uint32 Word = 0; Word < kMaskWords; ++Word)
            {
                uint64 Bits = 0u;
                for (uint32 Chunk = 0; Chunk < NumChunks; ++Chunk)
                {
                    Bits |= TouchMasks[(size_t)Chunk * ChunkWords + Level * kMaskWords + Word];
                }
                while (Bits != 0u)
                {
                    const uint32 Slot = Word * 64u + (uint32)Math::CountTrailingZeros64(Bits);
                    Bits &= Bits - 1u;
                    if (Lv.State[Slot] == EBrickState::Valid)
                    {
                        Lv.State[Slot] = EBrickState::Stale;
                        ++Lv.Pending;
                    }
                }
            }
        });
        Touches.Reset();
    }

    uint32 FWorldDistanceField::PickCompositeLevel()
    {
        // The level that waited longest, the finer one on a tie, so a mover churning one level cannot starve the rest.
        uint32 Best = kLevels;
        for (uint32 Level = 0; Level < kLevels; ++Level)
        {
            if (Levels[Level].Pending > 0u && (Best == kLevels || Levels[Level].Waiting > Levels[Best].Waiting))
            {
                Best = Level;
            }
        }
        for (uint32 Level = 0; Level < kLevels; ++Level)
        {
            if (Levels[Level].Pending > 0u && Level != Best)
            {
                ++Levels[Level].Waiting;
            }
        }
        if (Best < kLevels)
        {
            Levels[Best].Waiting = 0u;
        }
        return Best;
    }

    void FWorldDistanceField::Update(const FVector3& Camera, const FSunShadowCache::FSlotChanges& Changes, TSpan<const FInstanceCullEntry> Entries, FFrame& Out)
    {
        LUMINA_PROFILE_SCOPE();
        using namespace WorldDistanceFieldInternal;

        if (bResetRequested.exchange(false, std::memory_order_relaxed))
        {
            Reset();
        }
        // Unplaced levels mark every brick invalid, which this update clears or fills.
        Out.bFresh = !Levels[0].bPlaced;

        for (uint32 Level = 0; Level < kLevels; ++Level)
        {
            Place(Level, Camera);
        }

        if (bTrackAll)
        {
            // Every brick samples its casters again, so only where they are now needs remembering.
            bTrackAll = false;
            MergedSpheres.SetNum((uint32)Entries.Num(), FVector4(0.0f));
            for (uint32 Slot = 0; Slot < (uint32)Entries.Num(); ++Slot)
            {
                MergedSpheres[Slot] = IsCaster(Entries[Slot]) ? Entries[Slot].SphereBounds : FVector4(0.0f);
            }
            for (FLevel& Lv : Levels)
            {
                for (EBrickState& State : Lv.State)
                {
                    Lv.Pending += State == EBrickState::Valid ? 1u : 0u;
                    State = State == EBrickState::Valid ? EBrickState::Stale : State;
                }
            }
        }
        else if (Changes.bFull)
        {
            // A full upload re-sends unchanged slots too, and restaling every brick for it each frame never lets the far ones catch up.
            const uint32 NumSlots = Math::Max((uint32)Entries.Num(), (uint32)MergedSpheres.Num());
            for (uint32 Slot = 0; Slot < NumSlots; ++Slot)
            {
                const FVector4 Current = (Slot < (uint32)Entries.Num() && IsCaster(Entries[Slot])) ? Entries[Slot].SphereBounds : FVector4(0.0f);
                const FVector4 Merged  = Slot < (uint32)MergedSpheres.Num() ? MergedSpheres[Slot] : FVector4(0.0f);
                if (Current != Merged)
                {
                    Retrack(Slot, Entries, false);
                }
            }
        }
        else
        {
            for (const TSpan<const uint32>& List : { Changes.Dirty, Changes.DirtyStatic, Changes.Deactivated })
            {
                for (uint32 Slot : List)
                {
                    Retrack(Slot, Entries, false);
                }
            }
            // A streamed mover's CPU sphere trails the GPU's by a frame, which the next frame's touch covers while it keeps moving.
            for (uint32 Block : Changes.StreamedBlocks)
            {
                const uint32 First = Block * kInstanceCullBlockSize;
                const uint32 End   = Math::Min(First + kInstanceCullBlockSize, (uint32)Entries.Num());
                for (uint32 Slot = First; Slot < End; ++Slot)
                {
                    if ((Slot < (uint32)MergedSpheres.Num() && MergedSpheres[Slot].w > 0.0f) || IsCaster(Entries[Slot]))
                    {
                        Retrack(Slot, Entries, true);
                    }
                }
            }
        }
        ApplyTouches();

        Out.Bricks.Reset();
        for (uint32 Level = 0; Level < kLevels; ++Level)
        {
            const FVector3 Min = BrickCorner(Levels[Level].OriginBrick, BrickWorldSize(Level));
            Out.LevelBoxes[Level][0] = Min.x;
            Out.LevelBoxes[Level][1] = Min.y;
            Out.LevelBoxes[Level][2] = Min.z;
            Out.LevelBoxes[Level][3] = VoxelSize(Level);
        }

        auto AddBrick = [&Out](const FIntVector3& Brick, uint32 Level, bool bComposite)
        {
            FWorldDFBrickGPU Gpu;
            Gpu.X = Brick.x;
            Gpu.Y = Brick.y;
            Gpu.Z = Brick.z;
            Gpu.LevelAndMode = Level | (bComposite ? kWorldDFComposite : 0u);
            Out.Bricks.Add(Gpu);
        };

        Out.CompositeLevel = PickCompositeLevel();
        if (Out.CompositeLevel < kLevels)
        {
            FLevel& Lv = Levels[Out.CompositeLevel];
            const float BrickWorld = BrickWorldSize(Out.CompositeLevel);

            // Nearest first, the distance's float bits in the high word so the keys sort as integers.
            Candidates.Reset();
            for (uint32 Slot = 0; Slot < kBricksPerLevel; ++Slot)
            {
                if (Lv.State[Slot] != EBrickState::Valid)
                {
                    const FVector3 ToCamera = BrickCorner(Lv.SlotBrick[Slot], BrickWorld) + FVector3(0.5f * BrickWorld) - Camera;
                    const float DistanceSq = Math::Dot(ToCamera, ToCamera);
                    uint32 Bits = 0u;
                    Memory::Memcpy(&Bits, &DistanceSq, sizeof(Bits));
                    Candidates.Add(((uint64)Bits << 32u) | Slot);
                }
            }
            Algo::Sort(Candidates.begin(), Candidates.end());

            const uint32 Count = Math::Min((uint32)Candidates.Num(), kCompositeBudget);
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                const uint32 Slot = (uint32)(Candidates[Index] & 0xFFFFFFFFu);
                AddBrick(Lv.SlotBrick[Slot], Out.CompositeLevel, true);
                Lv.State[Slot] = EBrickState::Valid;
                --Lv.Pending;
            }
        }

        // Whatever scrolled in and waits its turn reads as empty space rather than the distances of the place it wrapped from.
        for (uint32 Level = 0; Level < kLevels; ++Level)
        {
            FLevel& Lv = Levels[Level];
            if (Lv.Pending == 0u)
            {
                continue;
            }
            for (uint32 Slot = 0; Slot < kBricksPerLevel; ++Slot)
            {
                if (Lv.State[Slot] == EBrickState::Invalid)
                {
                    AddBrick(Lv.SlotBrick[Slot], Level, false);
                    Lv.State[Slot] = EBrickState::Cleared;
                }
            }
        }
    }
}
