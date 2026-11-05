#include "RuntimePCH.h"
#include "ScenePrimitiveSet.h"
#include "ScenePrimitiveSetInternal.h"

#include "Containers/Algorithm.h"
#include "TaskSystem/Fork/ForkRuntime.h"

namespace Lumina
{
    void FScenePrimitiveSet::ApplyMovedTransforms(const FSyncPools& Pools, TSpan<const ECS::FEntity> Moved)
    {
        LUMINA_PROFILE_SECTION("Sync/Apply/Moved");

        // Movers whose move-time records were thrown away go round again with the rest.
        if (!MoveReplay.IsEmpty())
        {
            MoveReplay.Append(Moved);
            Moved = TSpan<const ECS::FEntity>(MoveReplay.GetData(), MoveReplay.Num());
        }
        const bool bReplayed = !MoveReplay.IsEmpty();

        const uint32 Count = (uint32)Moved.Num();
        const uint32 NumSlots = Jobs::IsRuntimeRunning() ? Jobs::GetNumThreadSlots() : 0u;
        const bool bStream = Count >= ScenePrimitiveSetInternal::kMinStreamedMoves && NumSlots > 1 && StreamCount + Count <= StreamTarget.Capacity
                          && StreamTarget.Records != nullptr && StreamTarget.Slots != nullptr;
        if (bStream)
        {
            StreamMovedTransforms(Pools, Moved);
        }
        else if (Count > 0)
        {
            ApplyMovedThroughCPU(Pools, Moved);
        }
        else
        {
            // The catch-up reads this frame's claims, so a frame the movers covered alone still opens a pass.
            BeginMovedClaims();
        }
        MoveReplay.Reset();

        Algo::Sort(StreamedBlocks);
        StreamedBlocks.SetNum(static_cast<int32>(Algo::Unique(StreamedBlocks) - StreamedBlocks.begin()));

        // The movers count each entity once, but the drained list can repeat one, so only a frame they streamed alone can trust the count.
        const bool bNoneStopped = bStreamListsExact && ContinuingStreams == StreamedLastFrame.Count;
        bStreamListsExact = !bStream && !bReplayed;
        ContinuingStreams = 0;
        CatchUpStoppedStreams(Pools, MovedClaimStamp, bNoneStopped);
    }

    uint32 FScenePrimitiveSet::BeginMovedClaims()
    {
        // Zero is the never-claimed value, so a wrapped stamp clears the claims rather than reusing them.
        if (++MovedClaimStamp == 0u)
        {
            EntityRecords.ForEach([](FEntityRecord& Record) { Record.MovedClaim = 0u; });
            MovedClaimStamp = 1u;
        }
        return MovedClaimStamp;
    }

    void FScenePrimitiveSet::ApplyMovedThroughCPU(const FSyncPools& Pools, TSpan<const ECS::FEntity> Moved)
    {
        const uint32 Count = (uint32)Moved.Num();
        SyncStats.SyncEntityCalls += Count;

        const uint32 Stamp = BeginMovedClaims();

        const auto ApplyOne = [this, &Pools, Stamp](ECS::FEntity Entity, TVector<uint32>* OutDirty)
        {
            if (Entity == ECS::NullEntity || HasCoalescedRecord(Entity))
            {
                return;
            }
            // An entity with no record has nothing drawn to move.
            FEntityRecord* Record = EntityRecords.Find(Entity);
            if (Record == nullptr || std::atomic_ref<uint32>(Record->MovedClaim).exchange(Stamp, std::memory_order_relaxed) == Stamp)
            {
                return;
            }
            ApplyEntityTransform(Pools, Entity, ScenePrimitiveSetInternal::kMovedSources, OutDirty);
        };

        const uint32 NumSlots = Jobs::IsRuntimeRunning() ? Jobs::GetNumThreadSlots() : 0u;
        if (Count < ScenePrimitiveSetInternal::kParallelMoveThreshold || NumSlots <= 1)
        {
            for (ECS::FEntity Entity : Moved)
            {
                ApplyOne(Entity, nullptr);
            }
            return;
        }

        SyncStats.RefreshInstanceCalls += Count;

        ResetParallelBuckets();
        Jobs::ParallelFor(Count, [&ApplyOne, Moved, this](const Jobs::FParallelRange& Range)
        {
            TVector<uint32>& Bucket = BucketFor(Range.Thread).Slots;
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                ApplyOne(Moved[i], &Bucket);
            }
        }, 64);

        MergeParallelDirtySlots();
    }

    bool FScenePrimitiveSet::StreamEntityTransform(const FSyncPools& Pools, ECS::FEntity Entity, uint32 Record, FDirtySlotBucket& Bucket)
    {
        const FStreamCacheEntry Entry = LookupStreamCacheEntry(Entity);
        if (Entry.Kind == EStreamKind::CPUPath)
        {
            return false;
        }
        const STransformComponent* Transform = Entry.Kind == EStreamKind::Stream ? Pools.Transform->TryGet(Entity) : nullptr;
        if (Transform == nullptr)
        {
            StreamTarget.Slots[Record] = kNoStreamSlot;
            return true;
        }

        ScenePrimitiveSetInternal::StreamRecord(StreamTarget.Records[Record], ScenePrimitiveSetInternal::MakeStreamRecord(Transform->GetWorldMatrixCached(), Entry.LocalCenter, Entry.Radius));
        StreamTarget.Slots[Record] = Entry.Slot;

        // A streamable entity is linked, so its record exists.
        FEntityRecord& Stamp = *EntityRecords.Find(Entity);
        Stamp.MoveFrame  = StreamFrame;
        Stamp.MoveRecord = Record;
        Bucket.Streamed.Add(Entity);
        const uint32 Block = Entry.Slot / kInstanceCullBlockSize;
        if (Bucket.StreamedBlocks.IsEmpty() || Bucket.StreamedBlocks.Last() != Block)
        {
            Bucket.StreamedBlocks.Add(Block);
        }
        return true;
    }

    // One instance slot of a plain mesh in place streams, since several sources, surfaces, bones or a foreground remap need the CPU path.
    void FScenePrimitiveSet::BuildStreamCacheEntry(FEntityRecord& Record) const
    {
        Record.StreamGeneration = StreamCacheGeneration;
        Record.StreamSlot       = kNoStreamSlot;

        uint32 Index = Constants::kIndexNoneU32;
        for (const uint32 Link : Record.Links)
        {
            if (Link == Constants::kIndexNoneU32)
            {
                continue;
            }
            if (Index != Constants::kIndexNoneU32)
            {
                return;
            }
            Index = Link;
        }
        if (Index == Constants::kIndexNoneU32)
        {
            return;
        }
        const FScenePrimitive& Prim = Primitives[Index];
        if (Prim.SurfaceCount != 1u || Prim.Source == EPrimitiveSource::SkeletalMesh || IsRemappedForeground(Prim))
        {
            return;
        }
        const uint32 Slot = Bindings[Prim.BindingBase].InstanceSlot;
        if (Slot >= (uint32)RetainedCullEntries.Num())
        {
            return;
        }

        Record.StreamSlot   = Slot;
        Record.StreamCenter = Prim.LocalCenter;
        Record.StreamRadius = Prim.LocalRadius * Math::Max(Prim.BoundsScale, 1.0f);
    }

    FScenePrimitiveSet::FStreamCacheEntry FScenePrimitiveSet::LookupStreamCacheEntry(ECS::FEntity Entity)
    {
        FStreamCacheEntry Entry;
        FEntityRecord* Record = EntityRecords.Find(Entity);
        if (Record == nullptr || Record->Entity != Entity.Value)
        {
            return Entry;
        }

        if (Record->StreamGeneration != StreamCacheGeneration)
        {
            BuildStreamCacheEntry(*Record);
        }

        Entry.Kind        = Record->StreamSlot == kNoStreamSlot ? EStreamKind::CPUPath : EStreamKind::Stream;
        Entry.Slot        = Record->StreamSlot;
        Entry.LocalCenter = Record->StreamCenter;
        Entry.Radius      = Record->StreamRadius;
        return Entry;
    }

    void FScenePrimitiveSet::OpenStreamFrame()
    {
        // Zero is never a frame, so a wrapped counter clears the records rather than matching an old stamp.
        if (++StreamFrame == 0u)
        {
            EntityRecords.ForEach([](FEntityRecord& Record)
            {
                Record.MoveFrame  = 0u;
                Record.MoveRecord = kNoStreamSlot;
            });
            StreamFrame = 1u;
        }
        bStreamFrameOpen = true;
    }

    void FScenePrimitiveSet::ArmMoveStream(ECS::FRegistry& Registry, const FTransformStreamTarget& Target, uint32 InMovedConsumer)
    {
        DisarmMoveStream(Registry);
        StreamTarget = Target;
        if (Target.Records == nullptr || Target.Slots == nullptr || Target.Capacity == 0 || !Jobs::IsRuntimeRunning())
        {
            return;
        }

        if (static_cast<uint32>(MoveLanes.Num()) < Jobs::GetNumThreadSlots())
        {
            MoveLanes.SetNum(Jobs::GetNumThreadSlots());
        }
        OpenStreamFrame();
        MoveStreamNext.store(0u, std::memory_order_relaxed);

        MoveWriter.Write    = &FScenePrimitiveSet::WriteFlatMove;
        MoveWriter.Context  = this;
        MoveWriter.Consumer = InMovedConsumer;
        bMoveStreamArmed = ECS::Utils::SetFlatMoveWriter(Registry, &MoveWriter);
    }

    void FScenePrimitiveSet::DisarmMoveStream(ECS::FRegistry& Registry)
    {
        if (!bMoveStreamArmed)
        {
            return;
        }
        ECS::Utils::ClearFlatMoveWriter(Registry, &MoveWriter);
        bMoveStreamArmed = false;
        for (FMoveStreamLane& Lane : MoveLanes)
        {
            Lane.Next = Lane.End = 0;
            Lane.Streamed.Reset();
            Lane.Blocks.Reset();
            Lane.Rewrites.Reset();
        }
        StreamTarget = {};
    }

    bool FScenePrimitiveSet::WriteFlatMove(void* Context, ECS::FEntity Entity, const FTransform& World)
    {
        return static_cast<FScenePrimitiveSet*>(Context)->WriteMove(Entity, World);
    }

    // Runs on job threads while systems move entities; the record pages are only grown inside Sync, so they hold still here.
    bool FScenePrimitiveSet::WriteMove(ECS::FEntity Entity, const FTransform& World)
    {
        const uint32 LaneIndex = Jobs::GetThreadSlot();
        if (LaneIndex >= (uint32)MoveLanes.Num())
        {
            return false;
        }
        FEntityRecord* Stamp = EntityRecords.Find(Entity);
        if (Stamp == nullptr || Stamp->Entity != Entity.Value)
        {
            return true;
        }
        FMoveStreamLane& Lane = MoveLanes[LaneIndex];

        if (Stamp->MoveFrame == StreamFrame)
        {
            if ((Stamp->MoveRecord & kMoveRewriteQueued) == 0u)
            {
                Stamp->MoveRecord |= kMoveRewriteQueued;
                Lane.Rewrites.Add(Entity);
            }
            return true;
        }

        const FStreamCacheEntry Entry = LookupStreamCacheEntry(Entity);
        if (Entry.Kind != EStreamKind::Stream)
        {
            return Entry.Kind == EStreamKind::Skip;
        }

        if (Lane.Next == Lane.End)
        {
            const uint32 Begin = MoveStreamNext.fetch_add(kMoveStreamChunk, std::memory_order_relaxed);
            if (Begin >= StreamTarget.Capacity)
            {
                return false;
            }
            Lane.Next = Begin;
            Lane.End  = Math::Min(Begin + kMoveStreamChunk, StreamTarget.Capacity);
        }
        // No fence, since the locked operation that ends every job flushes a worker's write-combining buffers.
        const uint32 Record = Lane.Next++;
        ScenePrimitiveSetInternal::StreamRecord(StreamTarget.Records[Record], ScenePrimitiveSetInternal::MakeStreamRecord(World.GetMatrix(), Entry.LocalCenter, Entry.Radius));
        StreamTarget.Slots[Record] = Entry.Slot;

        // Zero is no frame, so a record cleared by a wrapped counter never reads as continuing.
        Lane.Continuing += (Stamp->MoveFrame != 0u && Stamp->MoveFrame + 1u == StreamFrame) ? 1u : 0u;
        Stamp->MoveFrame  = StreamFrame;
        Stamp->MoveRecord = Record;
        Lane.Streamed.Add(Entity);
        const uint32 Block = Entry.Slot / kInstanceCullBlockSize;
        if (Lane.Blocks.IsEmpty() || Lane.Blocks.Last() != Block)
        {
            Lane.Blocks.Add(Block);
        }
        return true;
    }

    uint32 FScenePrimitiveSet::SealMoveStream(ECS::FRegistry& Registry)
    {
        if (!bMoveStreamArmed)
        {
            return 0u;
        }

        LUMINA_PROFILE_SECTION("Sync/Seal Move Stream");
        ECS::Utils::ClearFlatMoveWriter(Registry, &MoveWriter);
        bMoveStreamArmed = false;

        const uint32 Used = Math::Min(MoveStreamNext.load(std::memory_order_relaxed), StreamTarget.Capacity);
        bool bRewrites = false;
        for (FMoveStreamLane& Lane : MoveLanes)
        {
            ContinuingStreams += Lane.Continuing;
            Lane.Continuing = 0;
            // The rest of the lane's last chunk was claimed but never written.
            for (uint32 Record = Lane.Next; Record < Lane.End; ++Record)
            {
                StreamTarget.Slots[Record] = kNoStreamSlot;
            }
            Lane.Next = Lane.End = 0;

            StreamedThisFrame.Take(Lane.Streamed);
            StreamedBlocks.Append(Lane.Blocks);
            Lane.Blocks.Reset();
            bRewrites |= !Lane.Rewrites.IsEmpty();
        }

        if (bRewrites)
        {
            const FSyncPools Pools(Registry);
            for (FMoveStreamLane& Lane : MoveLanes)
            {
                for (ECS::FEntity Entity : Lane.Rewrites)
                {
                    FEntityRecord& Stamp = *EntityRecords.Find(Entity);
                    Stamp.MoveRecord &= ~kMoveRewriteQueued;
                    const STransformComponent* Transform = Pools.Transform->TryGet(Entity);
                    if (Transform != nullptr && Stamp.MoveRecord < Used)
                    {
                        ScenePrimitiveSetInternal::StreamRecord(StreamTarget.Records[Stamp.MoveRecord],
                            ScenePrimitiveSetInternal::MakeStreamRecord(Transform->GetWorldMatrixCached(), Stamp.StreamCenter, Stamp.StreamRadius));
                    }
                }
                Lane.Rewrites.Reset();
            }
        }
        _mm_sfence();

        if (!bDiscardMoveStream)
        {
            return Used;
        }

        // Records past the frame's count never reach the GPU, so dropping them only needs their movers replayed.
        bDiscardMoveStream = false;
        bStreamListsExact  = false;
        MoveReplay.Reset();
        for (uint32 List = 0; List < StreamedThisFrame.Used; ++List)
        {
            MoveReplay.Append(StreamedThisFrame.Lists[List]);
        }
        for (ECS::FEntity Entity : MoveReplay)
        {
            EntityRecords.Find(Entity)->MoveRecord = kNoStreamSlot;
        }
        StreamedThisFrame.Clear();
        StreamedBlocks.Reset();
        return 0u;
    }

    void FScenePrimitiveSet::KillMoveRecord(ECS::FEntity Entity)
    {
        FEntityRecord* Stamp = EntityRecords.Find(Entity);
        if (Stamp == nullptr || Stamp->MoveFrame != StreamFrame)
        {
            return;
        }
        const uint32 Record = Stamp->MoveRecord & ~kMoveRewriteQueued;
        if (Record < StreamTarget.Capacity && StreamTarget.Slots != nullptr)
        {
            StreamTarget.Slots[Record] = kNoStreamSlot;
        }
        Stamp->MoveRecord = kNoStreamSlot;
    }

    void FScenePrimitiveSet::InvalidateStreamCache(ECS::FEntity Entity)
    {
        if (FEntityRecord* Record = EntityRecords.Find(Entity))
        {
            Record->StreamGeneration = 0u;
        }
    }

    void FScenePrimitiveSet::InvalidateAllStreamCache()
    {
        // Zero marks an empty entry, so a wrapped generation clears the records instead.
        if (++StreamCacheGeneration == 0u)
        {
            EntityRecords.ForEach([](FEntityRecord& Record) { Record.StreamGeneration = 0u; });
            StreamCacheGeneration = 1u;
        }
    }

    void FScenePrimitiveSet::StreamMovedTransforms(const FSyncPools& Pools, TSpan<const ECS::FEntity> Moved)
    {
        LUMINA_PROFILE_SECTION("Sync/Apply/Stream");

        const uint32 Count = (uint32)Moved.Num();
        SyncStats.SyncEntityCalls += Count;

        const uint32 Stamp = BeginMovedClaims();

        ResetParallelBuckets();
        for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            Bucket.Streamed.Reset();
            Bucket.StreamedBlocks.Reset();
        }

        // Record i belongs to Moved[i], so a duplicate mover streams twice harmlessly and only the CPU path claims, since a locked exchange drains write-combining.
        const auto StreamOne = [this, &Pools, Stamp](ECS::FEntity Entity, uint32 Record, FDirtySlotBucket& Bucket)
        {
            if (Entity != ECS::NullEntity && !HasCoalescedRecord(Entity))
            {
                if (StreamEntityTransform(Pools, Entity, Record, Bucket))
                {
                    return;
                }
                FEntityRecord* Claim = EntityRecords.Find(Entity);
                if (Claim != nullptr && std::atomic_ref<uint32>(Claim->MovedClaim).exchange(Stamp, std::memory_order_relaxed) != Stamp)
                {
                    ApplyEntityTransform(Pools, Entity, ScenePrimitiveSetInternal::kMovedSources, &Bucket.Slots);
                }
            }
            StreamTarget.Slots[Record] = kNoStreamSlot;
        };

        const uint32 Base = StreamCount;
        Jobs::ParallelFor(Count, [&StreamOne, Moved, Base, this](const Jobs::FParallelRange& Range)
        {
            FDirtySlotBucket& Bucket = BucketFor(Range.Thread);
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                StreamOne(Moved[i], Base + i, Bucket);
            }
            _mm_sfence();
        }, 64);

        MergeParallelDirtySlots();

        for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
        {
            StreamedThisFrame.Take(Bucket.Streamed);
            StreamedBlocks.Append(Bucket.StreamedBlocks);
        }

        StreamCount = Base + Count;
    }

    void FScenePrimitiveSet::CatchUpStoppedStreams(const FSyncPools& Pools, uint32 MovedStamp, bool bNoneStopped)
    {
        StoppedScratch.Reset();
        if (!StreamedLastFrame.IsEmpty() && !bNoneStopped)
        {
            LUMINA_PROFILE_SECTION("Sync/Apply/CatchUp");

            ResetParallelBuckets();
            for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
            {
                Bucket.Stopped.Reset();
            }

            // Streamed again, or moved through the CPU path this frame, means its copies are already handled.
            const uint32 Frame = StreamFrame;
            Jobs::ParallelForOrSerial(StreamedLastFrame.Used, 1, [&](const Jobs::FParallelRange& Range)
            {
                TVector<ECS::FEntity>& Stopped = BucketFor(Range.Thread).Stopped;
                for (uint32 List = Range.Start; List < Range.End; ++List)
                {
                    for (const ECS::FEntity Entity : StreamedLastFrame.Lists[List])
                    {
                        const FEntityRecord* Record = EntityRecords.Find(Entity);
                        if (Record != nullptr && Record->MoveFrame != Frame && Record->MovedClaim != MovedStamp)
                        {
                            Stopped.Add(Entity);
                        }
                    }
                }
            });
            for (FDirtySlotBucket& Bucket : ParallelDirtySlots)
            {
                StoppedScratch.Append(Bucket.Stopped);
            }
        }

        std::swap(StreamedLastFrame, StreamedThisFrame);
        StreamedThisFrame.Clear();

        if (!StoppedScratch.IsEmpty())
        {
            ApplyMovedThroughCPU(Pools, TSpan<const ECS::FEntity>(StoppedScratch.GetData(), StoppedScratch.Num()));
        }
    }

    void FScenePrimitiveSet::ResetStreamState()
    {
        MoveReplay.Reset();
        ContinuingStreams = 0;
        bStreamListsExact = false;
        InvalidateAllStreamCache();
        StreamedLastFrame.Clear();
        StreamedThisFrame.Clear();
        StreamedBlocks.Reset();
        StreamCount = 0;
    }
}
