#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    // Routes this frame's transform + component changes into the persistent primitive table.
    void FDefaultSceneRenderer::SyncScenePrimitives()
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = World->GetEntityRegistry();

        // The bounds build reads the retained slots this sync rewrites.
        JoinInstanceBlockBounds();

        ScenePrimitives.SetForegroundView(ExtractFrame->ViewVolume, ExtractFrame->ViewLayers);

        ++TransformStreamSerial;
        MovedTransformScratch.Reset();
        {
            LUMINA_PROFILE_SECTION("Drain Moved Transforms");
            ECS::Utils::DrainMovedTransforms(Registry, MovedTransformConsumer, MovedTransformScratch);
        }
        // An armed stream already holds what the movers wrote, and takes the drained rest after it.
        if (!ScenePrimitives.IsMoveStreamArmed())
        {
            ScenePrimitives.SetTransformStream(ReserveTransformStream(TransformStreamSerial, (uint32)MovedTransformScratch.Num()));
        }
        ScenePrimitives.Sync(*World, TSpan<const ECS::FEntity>(MovedTransformScratch.GetData(), MovedTransformScratch.Num()));

        PublishRetainedUpload();
        ArmTransformStream(Registry);
        LaunchInstanceBlockBounds();
    }

    void FDefaultSceneRenderer::ArmTransformStream(ECS::FRegistry& Registry)
    {
        // Below this many movers a frame, the drained list costs less than a slice of write-combined memory per renderer.
        constexpr uint32 kMinArmedMoves = 1024;

        const uint32 Moved = ScenePrimitives.GetMoveTimeRecords() + (uint32)MovedTransformScratch.Num();
        const FTransformStreamTarget Target = Moved >= kMinArmedMoves
            ? ReserveTransformStream(TransformStreamSerial + 1, Moved + Moved / 4u)
            : FTransformStreamTarget{};
        if (Target.Records == nullptr)
        {
            ScenePrimitives.SetTransformStream({});
            return;
        }
        ScenePrimitives.ArmMoveStream(Registry, Target, MovedTransformConsumer);
    }

    FTransformStreamTarget FDefaultSceneRenderer::ReserveTransformStream(uint64 Serial, uint32 NumMoved)
    {
        if (NumMoved == 0u || TryRender() == nullptr)
        {
            return {};
        }

        // Its last frame is at least kFramesInFlight extracts old, whose fence the loop already waited on.
        TransformStreamSlot = (uint32)(Serial % kTransformStreamSlices);
        FSceneBuffer& Stream = TransformStreamRing[TransformStreamSlot];

        constexpr uint64 BytesPerMove = sizeof(FTransformStreamRecord) + sizeof(uint32);
        const uint64 Needed = (uint64)NumMoved * BytesPerMove;
        if (Stream.Size < Needed || Stream.Cpu == nullptr)
        {
            const uint64 Bytes = Stream.SlackedBytes(Needed);
            // A small CPU-visible aperture without ReBAR cannot spare it, so those moves take the CPU path.
            if (Bytes > RHI::GetCPUWriteSliceCap(kTransformStreamSlices))
            {
                return {};
            }
            Stream.Release();
            const RHI::FGPUAllocation Allocation = RHI::Malloc(Bytes, alignof(FTransformStreamRecord), RHI::EMemoryType::CPUWrite);
            if (!Allocation || Allocation.Cpu == nullptr)
            {
                return {};
            }
            RHI::SetDebugName(Allocation.Gpu, Stream.DebugName);
            Stream.Adopt(Allocation);
        }

        const uint64 Capacity = Stream.Size / BytesPerMove;
        FTransformStreamTarget Target;
        Target.Records  = reinterpret_cast<FTransformStreamRecord*>(Stream.Cpu);
        Target.Slots    = reinterpret_cast<uint32*>(Stream.Cpu + Capacity * sizeof(FTransformStreamRecord));
        Target.Capacity = (uint32)Math::Min<uint64>(Capacity, 0xFFFFFFFFull);
        return Target;
    }

    // Collects what changed in the retained scene, before the render phase uploads it.
    void FDefaultSceneRenderer::PublishRetainedUpload()
    {
        LUMINA_PROFILE_SCOPE();
        FFrameData::FGeometry::FRetainedUpload& Out = ExtractFrame->Geometry.RetainedUpload;

        // Ahead of everything below, since a base that moves marks its slot static-dirty.
        ScenePrimitives.AssignMeshletVisibility();
        Out.MeshletVisibilityWords = ScenePrimitives.GetMeshletVisibilityExtent();
        Out.LargestLODMeshlets     = ScenePrimitives.GetLargestLODMeshletTotal();
        Out.GrassMeshlets          = ScenePrimitives.GetGrassMeshletDemand();
        Out.GrassSlotCount         = ScenePrimitives.GetGrassSlotCount();

        const uint32 SlotCount = ScenePrimitives.GetRetainedSlotCount();
        Out.SlotCount = SlotCount;

        Out.DirtySlots.Reset();
        Out.DirtyStaticSlots.Reset();
        Out.DeactivatedSlots.Reset();

        const uint32 DeviceCapacity = RetainedDeviceCapacity.load(std::memory_order_acquire);
        // Growth keeps the device contents, so only a device holding nothing valid needs everything again.
        const bool bDeviceEmpty = DeviceCapacity == 0;
        Out.bFull       = ScenePrimitives.NeedsFullInstanceUpload() || bDeviceEmpty
                       || FScenePrimitiveSet::ShouldResendAllInstances(ScenePrimitives.GetDirtyInstanceSlots().Num(), SlotCount);
        Out.bFullStatic = ScenePrimitives.NeedsFullStaticUpload() || bDeviceEmpty
                       || static_cast<size_t>(ScenePrimitives.GetDirtyStaticSlots().Num() * 4) >= (SIZE_T)SlotCount;

        if (!Out.bFull || !Out.bFullStatic)
        {
            // A bitmap over the slot range yields the sorted, unique list in linear time, where a sort did not.
            DirtySlotBits.SetNum(IntCastChecked<int32>(RenderUtils::GetGroupCount(SlotCount, 64u)));
            auto Collect = [this, SlotCount](const TVector<uint32>& In, TVector<uint32>& OutList)
            {
                // A few slots sort faster than a walk over every bitmap word.
                constexpr SIZE_T SortedWordRatio = 16;
                if (In.Num() * SortedWordRatio < static_cast<size_t>(DirtySlotBits.Num()))
                {
                    for (uint32 DirtySlot : In)
                    {
                        if (DirtySlot < SlotCount)
                        {
                            OutList.Add(DirtySlot);
                        }
                    }
                    Algo::Sort(OutList);
                    OutList.SetNum(static_cast<int32>(Algo::Unique(OutList) - OutList.begin()));
                    return;
                }

                for (uint32 DirtySlot : In)
                {
                    if (DirtySlot < SlotCount)   // a slot freed after being marked is simply dropped
                    {
                        DirtySlotBits[DirtySlot >> 6] |= 1ull << (DirtySlot & 63u);
                    }
                }
                for (int32 Word = 0; Word < DirtySlotBits.Num(); ++Word)
                {
                    uint64 Bits = DirtySlotBits[Word];
                    if (Bits == 0)
                    {
                        continue;
                    }
                    DirtySlotBits[Word] = 0;
                    while (Bits != 0)
                    {
                        OutList.Add((Word * 64u) + (uint32)std::countr_zero(Bits));
                        Bits &= Bits - 1u;
                    }
                }
            };

            if (!Out.bFull)
            {
                Collect(ScenePrimitives.GetDirtyInstanceSlots(), Out.DirtySlots);
                if (FScenePrimitiveSet::ShouldResendAllInstances(Out.DirtySlots.Num(), SlotCount))
                {
                    Out.bFull = true;
                    Out.DirtySlots.Reset();
                }
            }
            if (!Out.bFullStatic)
            {
                Collect(ScenePrimitives.GetDirtyStaticSlots(), Out.DirtyStaticSlots);
                if (static_cast<size_t>(Out.DirtyStaticSlots.Num() * 4) >= (SIZE_T)SlotCount)
                {
                    Out.bFullStatic = true;
                    Out.DirtyStaticSlots.Reset();
                }
            }
        }

        // A full re-send already carries the cleared bits.
        if (!Out.bFull)
        {
            for (uint32 Slot : ScenePrimitives.GetDeactivatedSlots())
            {
                if (Slot < SlotCount)
                {
                    Out.DeactivatedSlots.Add(Slot);
                }
            }
        }

        // Records sit at the front of the slot's buffer and their slots after room for every record it can hold.
        Out.StreamCount = ScenePrimitives.GetTransformStreamCount();
        Out.StreamRecords = {};
        Out.StreamSlots = {};
        if (Out.StreamCount > 0)
        {
            const FSceneBuffer& Stream = TransformStreamRing[TransformStreamSlot];
            const uint64 Capacity = Stream.Size / (sizeof(FTransformStreamRecord) + sizeof(uint32));
            Out.StreamRecords = Stream.Sub(0, (uint64)Out.StreamCount * sizeof(FTransformStreamRecord));
            Out.StreamSlots   = Stream.Sub(Capacity * sizeof(FTransformStreamRecord), (uint64)Out.StreamCount * sizeof(uint32));
        }

        // Interned; a full instance re-send also re-sends these, since a replaced device buffer loses them.
        Out.SurfaceDescCount        = ScenePrimitives.GetSurfaceDescCount();
        Out.bSurfaceDescsChanged    = ScenePrimitives.AreSurfaceDescsDirty() || Out.bFull || Out.bFullStatic;
        Out.MaxSurfaceDescMeshlets  = ScenePrimitives.GetMaxSurfaceDescMeshlets();

        // Channel consumed, since the decisions above are now owned by this frame.
        ScenePrimitives.ClearDirtyInstanceSlots();
        ScenePrimitives.ClearFullInstanceUpload();
        ScenePrimitives.ClearSurfaceDescsDirty();
    }
}
