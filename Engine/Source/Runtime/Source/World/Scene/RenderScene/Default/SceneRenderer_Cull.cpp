#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "World/Scene/RenderScene/SceneCullMath.h"

#include <bit>

namespace Lumina
{
    namespace SceneRendererCullInternal
    {
        #if !defined(LE_SHIPPING)
        TConsoleVar<float> CVarCullDebugCapacityScale("r.Cull.DebugCapacityScale", 1.0f,
            "Scales the meshlet draw and block lists below demand, to exercise the coarse-LOD overflow fallback.");
        #endif

        TConsoleVar<int32> CVarCullMeshletVisibility("r.Cull.MeshletVisibility", 1,
            "0 decides the occlusion phase per instance instead of per meshlet, for an A/B against the meshlet bits.");

        TConsoleVar<bool> CVarSplitMeshletPhases(
            "r.SplitMeshletPhases",
            true,
            "Build late-phase meshlet blocks after the pyramid, only for instances it leaves visible, instead of culling every block twice.");

        TConsoleVar<bool> CVarInstanceBlockCull(
            "r.InstanceBlockCull",
            true,
            "Skip whole groups of 64 retained instances the camera is farther from than any of them can draw.");

        // Vulkan guarantees at least this on maxComputeWorkGroupCount[1], and the bounds dispatch uses y per slot.
        constexpr uint32 kMaxSkinnedBoundsDispatchY = 65535u;

        bool IsDebugCapacityScaled()
        {
            #if !defined(LE_SHIPPING)
            const float Scale = CVarCullDebugCapacityScale.GetValue();
            return Scale > 0.0f && Scale < 1.0f;
            #else
            return false;
            #endif
        }

        // Below 1 only in development, where it forces the overflow path that a real scene rarely reaches.
        uint32 ApplyDebugCapacityScale(uint32 Wanted)
        {
            #if !defined(LE_SHIPPING)
            if (IsDebugCapacityScaled())
            {
                return Math::Max(1u, (uint32)((float)Wanted * CVarCullDebugCapacityScale.GetValue()));
            }
            #endif
            return Wanted;
        }

        struct FSceneListFloor
        {
            uint32 Blocks   = 0;
            uint32 Meshlets = 0;
        };

        // A forest of a billion finest-level meshlets would floor the lists past device memory, so the floor stops here and the readback grows the rest.
        constexpr uint64 kSceneFloorBudgetBytes = 256ull << 20;
        constexpr uint64 kSceneFloorMaxEntries  = kSceneFloorBudgetBytes / (sizeof(uint32) * 2u);

        // The most either list may hold, past which the overflow fallback takes over instead of a device allocation that cannot land.
        constexpr uint64 kListBudgetBytes   = 1024ull << 20;
        constexpr uint64 kListBudgetEntries = kListBudgetBytes / (sizeof(uint32) * 2u);

        // Every view appending every slot at its largest LOD, the most a frame can append before its readback lands.
        FSceneListFloor SceneListFloorOf(const FDefaultSceneRenderer::FFrameData& Frame)
        {
            const auto&  Upload = Frame.Geometry.RetainedUpload;
            const uint64 Views  = Math::Max<uint64>(Frame.Views.CullViews.Num(), 1u);
            // A two-phase view reserves its blocks twice, matching CullInstances.
            uint64 BlockViews = Views;
            for (const FCullView& View : Frame.Views.CullViews)
            {
                BlockViews += (GetCullViewFlags(View) & ECullViewFlags::MeshletHiZ) != 0u ? 1u : 0u;
            }
            return { (uint32)Math::Min<uint64>((Upload.MeshletVisibilityWords + Upload.GrassMeshlets.Words) * BlockViews, kSceneFloorMaxEntries),
                     (uint32)Math::Min<uint64>((Upload.LargestLODMeshlets + Upload.GrassMeshlets.Meshlets) * Views, kSceneFloorMaxEntries) };
        }

        // The readback lags the frame by the ring depth, so a moving camera outgrows an exact fit before it is seen.
        uint32 WithHeadroom(uint32 Demand)
        {
            return (uint32)Math::Min<uint64>((uint64)Demand + Demand / 4u, 0xFFFFFFFFull);
        }

        // Below this the visible-instance rings hold every retained slot, so no instance is ever refused one.
        uint64 ExactVisibleBoundBytes()
        {
            static const uint64 Resolved = []
            {
                const uint64 MiB = Math::Max(DeviceLocalVRAMMiB() * 2ull / 100ull, 64ull);
                LOG_INFO("RenderScene: exact visible-instance bound auto-set to {} MiB, 2% of {} MiB device-local VRAM.",
                         MiB, DeviceLocalVRAMMiB());
                return MiB << 20;
            }();

            return Resolved;
        }

        // Every ring slot's visible instances plus their per-view ranges.
        uint64 VisibleInstanceRingBytes(uint64 Instances, uint64 Views)
        {
            return Instances * (sizeof(FGPUInstance) + Views * sizeof(FUIntVector2)) * RHI::kFramesInFlight;
        }

        bool OverlapsAny(const TVector<FUIntVector2>& Ranges, uint32 First, uint32 End)
        {
            for (const FUIntVector2& Range : Ranges)
            {
                if (First < Range.x + Range.y && Range.x < End)
                {
                    return true;
                }
            }
            return false;
        }

        // Contiguous slots are contiguous in the source arrays, so each run is one copy.
        void CollectSlotRuns(const TVector<uint32>& Slots, TVector<FUIntVector2>& Runs)
        {
            Runs.Reset();
            const int32 NumDirty = Slots.Num();
            for (int32 i = 0; i < NumDirty; )
            {
                int32 j = i + 1;
                while (j < NumDirty && Slots[j] == Slots[j - 1] + 1u)
                {
                    ++j;
                }
                Runs.Add(FUIntVector2{ Slots[i], (uint32)(j - i) });
                i = j;
            }
        }
    }

    // Every instance in a block the camera can reach is still tested one by one, so the skip never changes what is drawn.
    void FDefaultSceneRenderer::LaunchInstanceBlockBounds()
    {
        LUMINA_PROFILE_SCOPE();

        // A launch whose render never consumed it left the device copy behind the CPU one.
        if (bBlockBoundsLaunched)
        {
            bInstanceBlockBoundsValid = false;
            bBlockBoundsLaunched = false;
        }

        const FFrameData::FGeometry::FRetainedUpload& Upload = ExtractFrame->Geometry.RetainedUpload;
        const FInstanceCullEntry* Entries = ScenePrimitives.GetRetainedCullEntries();
        const uint32 NumSlots  = Upload.SlotCount;
        const uint32 NumBlocks = RenderUtils::GetGroupCount(NumSlots, kInstanceCullBlockSize);
        if (NumBlocks == 0u || Entries == nullptr)
        {
            return;
        }

        ScenePrimitives.GetGPUWrittenSlotRanges(GPUWrittenSlotRanges);

        bBlockBoundsRebuildAll = Upload.bFull || !bInstanceBlockBoundsValid || static_cast<uint32>(InstanceBlockBounds.Num()) != NumBlocks;
        BlockBoundsNumBlocks   = NumBlocks;

        if (static_cast<uint32>(InstanceBlockHasDF.Num()) != NumBlocks)
        {
            InstanceBlockHasDF.Init(0u, NumBlocks);
            bDistanceFieldBlocksDirty = true;
        }

        DirtyInstanceBlocks.Reset();
        const TVector<uint32>* StreamedBlocks = &ScenePrimitives.GetStreamedBlocks();
        if (bBlockBoundsRebuildAll)
        {
            InstanceBlockBounds.SetNum(NumBlocks);
        }
        else
        {
            // DirtySlots arrives sorted and unique, so its blocks come out sorted and only repeat back to back.
            for (uint32 Slot : Upload.DirtySlots)
            {
                const uint32 Block = Slot / kInstanceCullBlockSize;
                if (Slot < NumSlots && (DirtyInstanceBlocks.IsEmpty() || DirtyInstanceBlocks.Last() != Block))
                {
                    DirtyInstanceBlocks.Add(Block);
                }
            }

            // Both lists are sorted, so the merge keeps the run upload below working on one sorted, unique list.
            if (!StreamedBlocks->IsEmpty())
            {
                BlockMergeScratch.Reset();
                int32 Dirty = 0;
                int32 Streamed = 0;
                while (Dirty < DirtyInstanceBlocks.Num() || Streamed < StreamedBlocks->Num())
                {
                    const uint32 Next = Streamed >= StreamedBlocks->Num() ? DirtyInstanceBlocks[Dirty]
                                      : Dirty >= DirtyInstanceBlocks.Num() ? (*StreamedBlocks)[Streamed]
                                      : Math::Min(DirtyInstanceBlocks[Dirty], (*StreamedBlocks)[Streamed]);
                    Dirty    += (Dirty < DirtyInstanceBlocks.Num() && DirtyInstanceBlocks[Dirty] == static_cast<unsigned int>(Next)) ? 1u : 0u;
                    Streamed += (Streamed < StreamedBlocks->Num() && (*StreamedBlocks)[Streamed] == static_cast<unsigned int>(Next)) ? 1u : 0u;
                    if (Next < NumBlocks)
                    {
                        BlockMergeScratch.Add(Next);
                    }
                }
                swap(DirtyInstanceBlocks, BlockMergeScratch);
            }
        }

        const uint32 NumWork = bBlockBoundsRebuildAll ? NumBlocks : (uint32)DirtyInstanceBlocks.Num();
        bBlockBoundsLaunched = true;
        if (NumWork == 0u)
        {
            return;
        }

        constexpr uint32 BlocksPerTask = 256;
        const uint32 NumTasks = RenderUtils::GetGroupCount(NumWork, BlocksPerTask);
        const bool bAll = bBlockBoundsRebuildAll;
        bBlockBoundsRunning = true;
        const auto ComputeBlockBounds = [this, Entries, NumSlots, NumWork, bAll, StreamedBlocks](const Jobs::FParallelRange& Range)
        {
            for (uint32 TaskIndex = Range.Start; TaskIndex < Range.End; ++TaskIndex)
            {
                const uint32 WorkEnd = Math::Min((TaskIndex + 1u) * BlocksPerTask, NumWork);
                for (uint32 Work = TaskIndex * BlocksPerTask; Work < WorkEnd; ++Work)
                {
                    const uint32 Block = bAll ? Work : DirtyInstanceBlocks[Work];
                    const uint32 First = Block * kInstanceCullBlockSize;
                    const uint32 Last  = Math::Min(First + kInstanceCullBlockSize, NumSlots);
                    const bool bStreamed = !StreamedBlocks->IsEmpty() && Algo::BinarySearch(StreamedBlocks->begin(), StreamedBlocks->end(), Block);
                    InstanceBlockBounds[Block] = SceneCull::ComputeInstanceBlockBounds(Entries, First, Last,
                                                                                      bStreamed || SceneRendererCullInternal::OverlapsAny(GPUWrittenSlotRanges, First, Last));

                    const uint8 bHasDF = SceneCull::BlockHasDistanceFieldCaster(Entries, First, Last) ? 1u : 0u;
                    if (InstanceBlockHasDF[Block] != bHasDF)
                    {
                        InstanceBlockHasDF[Block] = bHasDF;
                        bBlockDFChanged.store(true, std::memory_order_relaxed);
                    }
                }
            }
        };
        InstanceBlockBoundsDone.Reset();
        Jobs::Spawn(Jobs::EJobPriority::Normal, InstanceBlockBoundsDone, [NumTasks, ComputeBlockBounds]() -> Jobs::TTask<void>
        {
            co_await Jobs::Call(Jobs::ForEach(NumTasks, ComputeBlockBounds, 1));
        });
    }

    void FDefaultSceneRenderer::ScatterTransformStream(RHI::FCmdListH CL, const FFrameData::FGeometry::FRetainedUpload& Upload)
    {
        static const FShaderH StreamCS = FShaderLibrary::Get("RetainedTransformStream.slang");
        if (Upload.StreamCount == 0u || !StreamCS || !Upload.StreamRecords || !Upload.StreamSlots)
        {
            return;
        }

        // Mirror of FPushConstants in RetainedTransformStream.slang.
        struct FTransformStreamArgs
        {
            RHI::TGPUSpan<FTransformStreamRecord> Records;
            RHI::TGPUSpan<uint32>                 Slots;
            RHI::TGPUSpan<FTransform3x4>          RetainedTransforms;
            RHI::TGPUSpan<FInstanceCullEntry>     RetainedCullEntries;
        };

        FTransformStreamArgs Args;
        Args.Records             = Upload.StreamRecords;
        Args.Slots               = Upload.StreamSlots;
        Args.RetainedTransforms  = { RetainedTransformBuffer, Upload.SlotCount };
        Args.RetainedCullEntries = { RetainedCullEntryBuffer, Upload.SlotCount };

        // Last frame read these in every stage, and the copies just recorded wrote them.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::AllCommands, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        const FUIntVector2 Groups = RenderUtils::FoldGroupCount(RenderUtils::GetGroupCount(Upload.StreamCount, kTransformStreamGroupSize));
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(StreamCS));
        RHI::CmdDispatch(CL, MakeArgs(Args), Groups.x, Groups.y, 1u);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::AllCommands, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::JoinInstanceBlockBounds()
    {
        if (bBlockBoundsRunning)
        {
            LUMINA_PROFILE_SECTION("Wait Instance Block Bounds");
            InstanceBlockBoundsDone.Wait();
            bBlockBoundsRunning = false;
        }
        if (bBlockDFChanged.exchange(false, std::memory_order_relaxed))
        {
            bDistanceFieldBlocksDirty = true;
        }
    }

    void FDefaultSceneRenderer::FinishInstanceBlockBounds(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SCOPE();

        JoinInstanceBlockBounds();
        if (!bBlockBoundsLaunched)
        {
            bInstanceBlockBoundsValid = false;
            return;
        }
        bBlockBoundsLaunched = false;

        const uint32 NumBlocks   = BlockBoundsNumBlocks;
        const bool   bRebuildAll = bBlockBoundsRebuildAll;
        ReserveBuffer(CL, InstanceBlockBoundsBuffer, (uint64)NumBlocks * sizeof(FInstanceBlockBounds),
                      /*bAllowShrink*/ bRebuildAll, /*bPreserveContents*/ !bRebuildAll);
        if (InstanceBlockBoundsBuffer.CapacityOf<FInstanceBlockBounds>() < NumBlocks)
        {
            bInstanceBlockBoundsValid = false;
            return;
        }

        // The bounds stay untouched until the next extract, which comes after the fills are joined.
        if (bRebuildAll)
        {
            StageWrite(InstanceBlockBoundsBuffer.Gpu, InstanceBlockBounds.GetData(), (uint64)NumBlocks * sizeof(FInstanceBlockBounds),
                       /*bFillBeforeSubmit*/ true);
        }
        else
        {
            // Consecutive dirty blocks go up as one write.
            for (int32 i = 0; i < DirtyInstanceBlocks.Num();)
            {
                const uint32 RunStart = DirtyInstanceBlocks[i];
                uint32 RunEnd = RunStart;
                while (i < DirtyInstanceBlocks.Num() && DirtyInstanceBlocks[i] == RunEnd)
                {
                    ++RunEnd;
                    ++i;
                }
                StageWrite(InstanceBlockBoundsBuffer.Gpu + (uint64)RunStart * sizeof(FInstanceBlockBounds),
                           &InstanceBlockBounds[RunStart], (uint64)(RunEnd - RunStart) * sizeof(FInstanceBlockBounds),
                           /*bFillBeforeSubmit*/ true);
            }
        }

        if (bDistanceFieldBlocksDirty)
        {
            DistanceFieldBlocks.Reset();
            for (uint32 Block = 0; Block < NumBlocks; ++Block)
            {
                if (InstanceBlockHasDF[Block] != 0u)
                {
                    DistanceFieldBlocks.Add(Block);
                }
            }
            bDistanceFieldBlocksDirty = false;
            if (!DistanceFieldBlocks.IsEmpty())
            {
                const uint64 Bytes = (uint64)DistanceFieldBlocks.Num() * sizeof(uint32);
                ReserveBuffer(CL, SunDFBlockListBuffer, Bytes, /*bAllowShrink*/ true, /*bPreserveContents*/ false);
                if (SunDFBlockListBuffer.Size >= Bytes)
                {
                    StageWrite(SunDFBlockListBuffer.Gpu, DistanceFieldBlocks.GetData(), Bytes);
                }
                else
                {
                    // Still dirty, so the sun shadow cull walks every slot rather than a list the buffer cannot hold.
                    bDistanceFieldBlocksDirty = true;
                }
            }
        }

        FlushStagedWrites(CL);
        bInstanceBlockBoundsValid = true;
    }

    void FDefaultSceneRenderer::UploadBoneArena(RHI::FCmdListH CL, const FFrameData& Frame)
    {
        const TVector<FBoneTransform>& Mirror = Frame.Geometry.BonesData;

        const uint32 WantedCount = Math::Min(Frame.Geometry.BoneCount, (uint32)Mirror.Num());
        if (WantedCount == 0)
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Upload Bone Arena");

        static_assert(RHI::kFramesInFlight == 2, "UploadBoneArena tracks the missed upload of one other slot.");

        // Every copy below is a deferred fill, since it is PCIe-bound and the frame can record while it runs.
        FSceneBuffer& Arena = BoneArenaRing[CurrentFrameSlot];
        const RHI::GPUPtr PrevArena = Arena.Gpu;
        // Sized by residency, so it tracks the meshes the cull keeps rather than every skeleton alive.
        ReserveBuffer(CL, Arena, (SIZE_T)WantedCount * sizeof(FBoneTransform));
        if (!Arena)
        {
            return;
        }

        // A grow that failed keeps the smaller arena, and BoneNum is clamped to the same count.
        const uint32 ArenaCount = Math::Min(WantedCount, Arena.CapacityOf<FBoneTransform>());
        const SIZE_T ArenaBytes = (SIZE_T)ArenaCount * sizeof(FBoneTransform);

        // This slot's arena missed the uploads made into the other slots since it last synced, which only the latest one's ranges describe.
        const uint64 Serial       = ++BoneUploadSerial;
        const uint64 SyncedSerial = BoneArenaSyncedSerial[CurrentFrameSlot];
        BoneArenaSyncedSerial[CurrentFrameSlot] = Serial;

        const TVector<FUIntVector2>& FrameRanges = Frame.Geometry.BoneUploadRanges;
        const bool bMissedLastUpload = SyncedSerial + 2u == Serial;

        // A reallocation drops every resident pose, and so does an arena older than the ranges kept, so the dirty set no longer describes it.
        if (Arena.Gpu != PrevArena || (SyncedSerial + 1u != Serial && !bMissedLastUpload))
        {
            LastBoneUploadRanges.Reset();
            LastBoneUploadRanges.Append(FrameRanges.begin(), FrameRanges.end());
            StageWrite(Arena.Gpu, Mirror.GetData(), ArenaBytes, /*bFillBeforeSubmit*/ true);
            FlushStagedWrites(CL);
            return;
        }

        // Chunks rather than a sorted range list, since a crowd dirties tens of thousands of slices a frame.
        constexpr uint32 kChunkShift = 6;
        const uint32 NumChunks = (ArenaCount + (1u << kChunkShift) - 1u) >> kChunkShift;

        TVector<uint64>& Dirty = BoneDirtyChunks;
        Dirty.Init(0ull, RenderUtils::GetGroupCount(NumChunks, 64u));

        bool bAnyDirty = false;
        const auto MarkRanges = [&](const TVector<FUIntVector2>& Ranges)
        {
            for (const FUIntVector2& Range : Ranges)
            {
                const uint32 End = Math::Min(Range.x + Range.y, ArenaCount);
                if (Range.x >= End)
                {
                    continue;
                }
                for (uint32 Chunk = Range.x >> kChunkShift; Chunk <= ((End - 1u) >> kChunkShift); ++Chunk)
                {
                    Dirty[Chunk >> 6u] |= 1ull << (Chunk & 63u);
                }
                bAnyDirty = true;
            }
        };

        MarkRanges(FrameRanges);
        if (bMissedLastUpload)
        {
            MarkRanges(LastBoneUploadRanges);
        }
        LastBoneUploadRanges.Reset();
        LastBoneUploadRanges.Append(FrameRanges.begin(), FrameRanges.end());

        if (!bAnyDirty)
        {
            return;   // every pose the gather kept is already resident
        }

        // Merging across a small gap re-sends slices that were already resident, which beats a second copy.
        constexpr uint32 kMergeGapChunks = 1024u >> kChunkShift;

        const auto Flush = [&](uint32 StartChunk, uint32 EndChunk)
        {
            const uint32 Start = StartChunk << kChunkShift;
            const uint32 End   = Math::Min(EndChunk << kChunkShift, ArenaCount);
            if (Start < End)
            {
                StageWrite(Arena.Gpu + (uint64)Start * sizeof(FBoneTransform), Mirror.GetData() + Start,
                           (uint64)(End - Start) * sizeof(FBoneTransform), /*bFillBeforeSubmit*/ true);
            }
        };

        uint32 RunStart = Constants::kIndexNoneU32;
        uint32 RunEnd   = 0;
        for (int32 Word = 0; Word < Dirty.Num(); ++Word)
        {
            for (uint64 Bits = Dirty[Word]; Bits != 0ull; Bits &= Bits - 1ull)
            {
                const uint32 Chunk = Word * 64u + (uint32)Math::CountTrailingZeros64(Bits);
                if (RunStart != Constants::kIndexNoneU32 && Chunk > RunEnd + kMergeGapChunks)
                {
                    Flush(RunStart, RunEnd);
                    RunStart = Constants::kIndexNoneU32;
                }
                if (RunStart == Constants::kIndexNoneU32)
                {
                    RunStart = Chunk;
                }
                RunEnd = Chunk + 1u;
            }
        }
        Flush(RunStart, RunEnd);
        FlushStagedWrites(CL);
    }

    void FDefaultSceneRenderer::UploadSkinnedFrameData(RHI::FCmdListH CL, FFrameData& Frame)
    {
        TVector<FSkinnedFrameData>& Data  = Frame.Geometry.SkinnedFrameData;
        const TVector<uint32>&      Slots = Frame.Geometry.SkinnedIndices;   // skinned indices, not retained slots

        // Minted here rather than reused from MeshletDrawTag, which wraps at 4095 and can alias.
        CurrentSkinnedFrameTag++;

        SkinWorkCapacity      = 0;
        SkinnedSlotsUploaded  = 0;
        SkinnedBoundsMaxRange = 0;

        if (Data.IsEmpty() || Slots.IsEmpty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Upload Skinned Frame Data");

        // A grow that failed leaves last frame's tags on the GPU, so the cull rejects every skinned slot this frame.
        ReserveBuffer(CL, SkinnedFrameDataBuffer, Data.Num() * sizeof(FSkinnedFrameData));
        ReserveBuffer(CL, SkinnedSlotListBuffer, Slots.Num() * sizeof(uint32));
        if (SkinnedFrameDataBuffer.CapacityOf<FSkinnedFrameData>() < (uint32)Data.Num()
            || SkinnedSlotListBuffer.CapacityOf<uint32>() < (uint32)Slots.Num())
        {
            return;
        }

        WriteBuffer(CL, SkinnedSlotListBuffer.Gpu, Slots.GetData(), Slots.Num() * sizeof(uint32));
        SkinnedSlotsUploaded = (uint32)Slots.Num();

        //~ Bounds arena layout, assigned before the upload below carries the bases to the GPU.
        uint64 BoundsTotal = 0;

        for (uint32 Slot : Slots)
        {
            if (Slot >= (uint32)Data.Num())
            {
                continue;
            }

            const uint32 Len = Data[Slot].SurfaceMeshletCount;

            SkinnedBoundsMaxRange = Math::Max(SkinnedBoundsMaxRange, Len);
            BoundsTotal += Len;
        }

        SkinWorkCapacity = (uint32)Math::Min<uint64>(BoundsTotal, Constants::kIndexNoneU32 - 1u);

        ReserveBuffer(CL, SkinnedMeshletBoundsBuffer, Math::Max<SIZE_T>(sizeof(FMeshletSphere), (SIZE_T)BoundsTotal * sizeof(FMeshletSphere)));

        ReserveBuffer(CL, SkinnedMeshletConeBuffer, Math::Max<SIZE_T>(sizeof(FMeshletCone), (SIZE_T)BoundsTotal * sizeof(FMeshletCone)));

        // The smaller of the two, since one base indexes both and a slot must fit in each.
        const uint32 SphereCap = SkinnedMeshletBoundsBuffer
            ? SkinnedMeshletBoundsBuffer.CapacityOf<FMeshletSphere>()
            : 0u;
        const uint32 ConeCap = SkinnedMeshletConeBuffer
            ? SkinnedMeshletConeBuffer.CapacityOf<FMeshletCone>()
            : 0u;

        SkinnedMeshletBoundsCapacity = Math::Min(SphereCap, ConeCap);

        // Stamped on the render thread, since the merge ran against a different FFrameData.
        uint32 BoundsCursor  = 0;
        uint32 DispatchIndex = 0;

        for (uint32 Slot : Slots)
        {
            if (Slot >= (uint32)Data.Num())
            {
                continue;
            }

            // Position in this list is the bounds dispatch's y index, which cannot exceed the grid limit.
            const bool bDispatchable = DispatchIndex < SceneRendererCullInternal::kMaxSkinnedBoundsDispatchY;
            DispatchIndex++;

            FSkinnedFrameData& D = Data[Slot];
            D.FrameTag = CurrentSkinnedFrameTag;

            const uint32 Begin = D.SurfaceMeshletOffset;
            const uint32 Len   = D.SurfaceMeshletCount;

            // A fold that lands on the no-bounds marker would read as unbounded, so that slice starts one entry later.
            const uint32 Cursor = (BoundsCursor - Begin == kNoSkinnedBounds) ? BoundsCursor + 1u : BoundsCursor;

            // All or nothing, because a partly written slice leaves the cull rejecting against stale data.
            if (Len == 0u || !bDispatchable || (uint64)Cursor + Len > SkinnedMeshletBoundsCapacity)
            {
                D.SkinnedBoundsBase = kNoSkinnedBounds;
                continue;
            }

            // Folded so the shader indexes by a mesh-global meshlet index, same trick as the pre-skin slice.
            D.SkinnedBoundsBase = Cursor - Begin;
            BoundsCursor = Cursor + Len;
        }

        // A merged gap is still staged and copied, so this trades one copy region against ~1 KiB of bytes.
        constexpr uint32 kSkinnedMergeGap = 24;

        const uint32 Count = (uint32)Data.Num();

        // Only the gathered slots, coalesced, and a bitmap over the slot range orders them in linear time where a sort did not.
        TVector<uint64>& SlotBits = SkinnedSlotBits;
        SlotBits.Init(0ull, IntCastChecked<int32>(((SIZE_T)Count + 63u) / 64u));
        for (uint32 Slot : Slots)
        {
            if (Slot < Count)
            {
                SlotBits[Slot >> 6] |= 1ull << (Slot & 63u);
            }
        }

        TVector<FUIntVector2>& Runs = SkinnedRunScratch;
        Runs.Reset();

        const auto AddRun = [&](uint32 Start, uint32 End)
        {
            End = Math::Min(End, Count);
            if (Start < End)
            {
                Runs.Emplace(Start, End - Start);
            }
        };

        bool   bHaveRun = false;
        uint32 RunStart = 0;
        uint32 RunEnd   = 0;
        for (int32 Word = 0; Word < SlotBits.Num(); ++Word)
        {
            uint64 Bits = SlotBits[Word];
            while (Bits != 0)
            {
                const uint32 Slot = (Word * 64u) + (uint32)std::countr_zero(Bits);
                Bits &= Bits - 1u;
                if (bHaveRun && Slot <= RunEnd + kSkinnedMergeGap)
                {
                    RunEnd = Slot + 1u;
                    continue;
                }
                if (bHaveRun)
                {
                    AddRun(RunStart, RunEnd);
                }
                RunStart = Slot;
                RunEnd   = Slot + 1u;
                bHaveRun = true;
            }
        }
        if (bHaveRun)
        {
            AddRun(RunStart, RunEnd);
        }

        WriteBufferRuns(CL, SkinnedFrameDataBuffer.Gpu, Data.GetData(), sizeof(FSkinnedFrameData), Runs, /*bFillBeforeSubmit*/ true);

        uint64 StagedSlots = 0;
        for (const FUIntVector2& Run : Runs)
        {
            StagedSlots += Run.y;
        }
        LUMINA_PROFILE_VALUE("Skinning/FrameDataRegions", Runs.Num());
        LUMINA_PROFILE_VALUE("Skinning/FrameDataKiB", StagedSlots * sizeof(FSkinnedFrameData) / 1024);
    }

    void FDefaultSceneRenderer::SkinnedMeshletBoundsPass(RHI::FCmdListH CL, const FFrameData& Frame)
    {
        const uint32 NumSkinned = Math::Min((uint32)Frame.Geometry.SkinnedIndices.Num(), SkinnedSlotsUploaded);
        if (NumSkinned == 0 || SkinnedBoundsMaxRange == 0 || SkinnedMeshletBoundsCapacity == 0
            || RetainedStaticCapacity == 0)
        {
            return;
        }

        static const FShaderH BoundsShader = FShaderLibrary::Get("SkinnedMeshletBounds.slang");
        if (!BoundsShader || !SkinnedSlotListBuffer || !SkinnedFrameDataBuffer
            || !RetainedStaticBuffer || !SkinnedMeshletBoundsBuffer || !SkinnedMeshletConeBuffer)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Skinned Meshlet Bounds", tracy::Color::SkyBlue3);
        SCENE_GPU_SCOPE(CL, "Skinned Meshlet Bounds");

        struct FSkinnedBoundsPC
        {
            uint32 MaxRange;
            uint32 _Pad;
            RHI::TGPUSpan<uint32>            SlotList;
            RHI::TGPUSpan<FSkinnedFrameData> SkinnedFrameData;
            RHI::TGPUSpan<FInstanceStatic>   RetainedStatic;
            RHI::TGPUSpan<FMeshletSphere>    OutBounds;
            RHI::TGPUSpan<FMeshletCone>      OutCones;
        } PC = {};
        static_assert(sizeof(FSkinnedBoundsPC) == 88, "FSkinnedBoundsPC must match SkinnedMeshletBounds.slang.");

        PC.MaxRange         = SkinnedBoundsMaxRange;
        PC.SlotList         = { SkinnedSlotListBuffer, NumSkinned };
        PC.SkinnedFrameData = { SkinnedFrameDataBuffer };
        PC.RetainedStatic   = { RetainedStaticBuffer, RetainedStaticCapacity };
        PC.OutBounds        = { SkinnedMeshletBoundsBuffer, SkinnedMeshletBoundsCapacity };
        PC.OutCones         = { SkinnedMeshletConeBuffer, SkinnedMeshletBoundsCapacity };

        constexpr uint32 kBoundsGroupSize = 64;
        const uint32 GroupsX = RenderUtils::GetGroupCount(SkinnedBoundsMaxRange, kBoundsGroupSize);

        // Slots past the grid limit were given kNoSkinnedBounds above, so dropping them here loses nothing.
        const uint32 GroupsY = Math::Min(NumSkinned, SceneRendererCullInternal::kMaxSkinnedBoundsDispatchY);

        DispatchCompute(CL, BoundsShader, PC, GroupsX, GroupsY, 1u);

        // Read by the cull, which is the next thing to run.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::CompileDrawCommands_Render(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Render Scene");

        FFrameData& Frame = *RenderFrame;
        FSceneGlobalData& SceneGlobalData = Frame.SceneGlobalData;
        SceneGlobalData.CullData.MeshletDrawTag = (MeshletDrawTagCounter++ % 4095u) + 1u;

        PlanCullCapacities(CL, Frame);

        SceneGlobalData.CullData.MeshletDrawListCapacity = DrawListCapacity;
        SceneGlobalData.CullData.InstanceNum             = FrameVisibleInstanceCapacity;

        UploadBoneArena(CL, Frame);
        UploadSkinnedFrameData(CL, Frame);
        // The arena's size, not a per-frame total, since it bounds an INDEX a stable base points into.
        SceneGlobalData.CullData.BoneNum = Math::Min(Frame.Geometry.BoneCount, BoneArenaRing[CurrentFrameSlot].CapacityOf<FBoneTransform>());

        {
            LUMINA_PROFILE_SECTION_COLORED("Write Scene Buffers", tracy::Color::OrangeRed3);

            UpdateIBLDirty(Frame);
            UploadLightData(Frame);
            PublishSceneRoot(CL, Frame);
            DispatchGPUSceneCull(CL, Frame);
        }

        // A frame with no retained slots never reaches the launch in the cull, and its bone fills still belong on the workers.
        LaunchDeferredStageFills();
    }

    void FDefaultSceneRenderer::PlanCullCapacities(RHI::FCmdListH CL, const FFrameData& Frame)
    {
        // Only the GPU knows the demand; an undersized frame drops blocks to inline skinning and corrects.
        const uint32 PreSkinWanted = Math::Min(PreSkinDemand.Observe(LastPreSkinRequested), GetMaxPreSkinnedVertices());

        UpdateMeshletBoundFeedback(CurrentFrameSlot);

        const bool bAfterCameraCut = TrackCameraCut(Frame);
        const bool bSceneGrew      = TrackSceneGrowth(Frame);

        DrawListPeak  = Math::Max(DrawListPeak, LastDrawListRequired);
        BlockListPeak = Math::Max(BlockListPeak, LastBlocksRequested);
        VisiblePeak   = Math::Max(VisiblePeak, LastVisibleInstances);

        // Only until the readback has seen the new scene or view, so a large world does not hold its worst case every frame.
        const SceneRendererCullInternal::FSceneListFloor Floor = (bAfterCameraCut || bSceneGrew)
            ? SceneRendererCullInternal::SceneListFloorOf(Frame) : SceneRendererCullInternal::FSceneListFloor{};

        // Windowed like the other GPU-fed sizes, so the raw readback's lag does not collapse it.
        const uint32 DrawListWindowed = Math::Max(SceneRendererCullInternal::WithHeadroom(DrawListDemand.Observe(LastDrawListRequired)), Floor.Meshlets);
        // Bounded, since a scene that asks for more than this draws its overflow coarser rather than taking the device down.
        const uint32 DrawListWanted   = Math::Min(SceneRendererCullInternal::ApplyDebugCapacityScale(bAfterCameraCut ? Math::Max(DrawListWindowed, DrawListPeak) : DrawListWindowed),
                                                  (uint32)SceneRendererCullInternal::kListBudgetEntries);

        ReservePreSkinBuffers(CL, PreSkinWanted);
        ReserveMeshletDrawBuffers(CL, Frame, DrawListWanted);
        ReserveVisibleInstanceBuffers(CL, Frame, bAfterCameraCut);
        ReserveMeshletBlockList(CL, bAfterCameraCut, Floor.Blocks);
        LogCullOverflow();
    }

    bool FDefaultSceneRenderer::TrackSceneGrowth(const FFrameData& Frame)
    {
        // The extent moves only when a slot outgrows the free lists or the set resets, so steady churn leaves it alone.
        const auto&  Upload          = Frame.Geometry.RetainedUpload;
        const uint32 VisibilityWords = Upload.MeshletVisibilityWords;
        // Grass blocks hold no visibility words, so a terrain whose grass arrives after the meshes would not move the extent.
        if (VisibilityWords != LastSceneVisibilityWords || Upload.GrassSlotCount != LastSceneGrassSlots)
        {
            if (SceneFloorFramesLeft == 0u)
            {
                LOG_INFO("RenderScene: meshlet lists floored at the scene, {} visibility words and {} largest-LOD meshlets, plus {} words and {} meshlets for {} grass blades, across {} cull views.",
                         VisibilityWords, Upload.LargestLODMeshlets, Upload.GrassMeshlets.Words, Upload.GrassMeshlets.Meshlets, Upload.GrassSlotCount, Frame.Views.CullViews.Num());
            }
            LastSceneVisibilityWords = VisibilityWords;
            LastSceneGrassSlots      = Upload.GrassSlotCount;
            SceneFloorFramesLeft     = RHI::kFramesInFlight + 2u;
        }
        const bool bGrew = SceneFloorFramesLeft > 0u;
        SceneFloorFramesLeft = bGrew ? SceneFloorFramesLeft - 1u : 0u;
        return bGrew;
    }

    bool FDefaultSceneRenderer::TrackCameraCut(const FFrameData& Frame)
    {
        // A cut lands somewhere the lagging demand has never seen, so the lists hold the session peak until it catches up.
        constexpr float CameraCutDistance = 10.0f;
        const auto& CullViews = Frame.Views.CullViews;
        if (!CullViews.IsEmpty())
        {
            const FVector3 Origin = FVector3(CullViews[0].ViewOriginAndFlags);
            if (bHasLastCullOrigin && Math::Length(Origin - LastCullOrigin) > CameraCutDistance)
            {
                CameraCutFramesLeft = RHI::kFramesInFlight + 2u;
            }
            LastCullOrigin     = Origin;
            bHasLastCullOrigin = true;
        }
        const bool bAfterCameraCut = CameraCutFramesLeft > 0u;
        CameraCutFramesLeft = bAfterCameraCut ? CameraCutFramesLeft - 1u : 0u;
        return bAfterCameraCut;
    }

    void FDefaultSceneRenderer::ReservePreSkinBuffers(RHI::FCmdListH CL, uint32 PreSkinWanted)
    {
        const SIZE_T PreSkinnedSize = Math::Max<SIZE_T>(sizeof(FPreSkinnedVertex),
                                      (SIZE_T)Math::Max<uint32>(PreSkinWanted, 1u) * sizeof(FPreSkinnedVertex));

        ReserveBuffer(CL, PreSkinnedVerticesBuffer, PreSkinnedSize);
        PreSkinnedVertexCapacity = (uint32)Math::Min<uint64>(
            PreSkinnedVerticesBuffer.Size / sizeof(FPreSkinnedVertex), 0xFFFFFFFFull);

        // Only motion vectors read the previous pose, so without them the skinning pass skips it entirely.
        PreSkinnedPrevCapacity = 0;
        if (IsVelocityWanted())
        {
            ReserveBuffer(CL, PreSkinnedPrevPositionsBuffer, (SIZE_T)Math::Max(PreSkinnedVertexCapacity, 1u) * sizeof(FPrevSkinnedPosition));
            PreSkinnedPrevCapacity = (uint32)Math::Min<uint64>(PreSkinnedPrevPositionsBuffer.Size / sizeof(FPrevSkinnedPosition), PreSkinnedVertexCapacity);
        }
    }

    void FDefaultSceneRenderer::ReserveMeshletDrawBuffers(RHI::FCmdListH CL, const FFrameData& Frame, uint32 DrawListWanted)
    {
        const uint8  Slot         = CurrentFrameSlot;
        const uint32 NumCullViews = (uint32)Frame.Views.CullViews.Num();
        const uint32 NumDraws     = Frame.Views.NumDrawsPerView;

        const SIZE_T MeshletDrawListSize = Math::Max<SIZE_T>(
            sizeof(uint32) * 2,
            (SIZE_T)DrawListWanted * sizeof(uint32) * 2);
        ReserveBuffer(CL, MeshletDrawListRing[Slot], MeshletDrawListSize);
        DrawListCapacity = MeshletDrawListRing[Slot].CapacityOf<FUIntVector2>();
        if (SceneRendererCullInternal::IsDebugCapacityScaled())
        {
            DrawListCapacity = Math::Min(DrawListCapacity, DrawListWanted);
        }

        // Must mirror MeshletCullPass' bucket count exactly, which clamps both operands to 1.
        const SIZE_T NumArgSlots = (SIZE_T)Math::Max(NumCullViews, 1u) * (SIZE_T)Math::Max(NumDraws, 1u);

        // Read after the resize, because last frame's value describes the other ring slot's buffer.
        const uint32 MaxGroups = MeshletsPerSubDraw();
        const uint32 WantedSubDraws = (DrawListCapacity == 0u) ? 1u : (((DrawListCapacity - 1u) / MaxGroups) + 1u);
        const SIZE_T NumBucketSlices = NumArgSlots * (SIZE_T)kMeshletSliceCount;
        // One spare view of slots, since a multi-view shadow draw's strided range runs a whole view past its last read.
        const SIZE_T SpareViewSlices  = (SIZE_T)Math::Max(NumDraws, 1u) * (SIZE_T)kMeshletSliceCount;
        const SIZE_T SlicesPerSubDraw = NumBucketSlices + SpareViewSlices;
        const SIZE_T MeshDrawArgsSize = Math::Max<SIZE_T>(
            sizeof(FMeshletDrawArgs),
            SlicesPerSubDraw * (SIZE_T)WantedSubDraws * sizeof(FMeshletDrawArgs));

        ReserveBuffer(CL, MeshDrawArgsRing[Slot], MeshDrawArgsSize);
        const uint32 AllocatedArgSlots = MeshDrawArgsRing[Slot].CapacityOf<FMeshletDrawArgs>();
        MeshSubDrawsPerSlice = (uint32)Math::Min<SIZE_T>(WantedSubDraws, AllocatedArgSlots / SlicesPerSubDraw);

        if (MeshSubDrawsPerSlice > LoggedSubDrawPeak)
        {
            LoggedSubDrawPeak = MeshSubDrawsPerSlice;
            LOG_INFO("Meshlet sub-draws per slice: {} (draw-list capacity {}, mesh workgroups per draw {}).",
                     MeshSubDrawsPerSlice, DrawListCapacity, MaxGroups);
        }
    }

    void FDefaultSceneRenderer::ReserveVisibleInstanceBuffers(RHI::FCmdListH CL, const FFrameData& Frame, bool bAfterCameraCut)
    {
        const uint8  Slot         = CurrentFrameSlot;
        const uint32 NumCullViews = (uint32)Frame.Views.CullViews.Num();

        // Windowed peak, not the last readback: that count lags kFramesInFlight and collapses the allocation when the camera sees something empty.
        const uint32 VisibleWindowed = SceneRendererCullInternal::WithHeadroom(VisibleInstanceDemand.Observe(LastVisibleInstances));
        const uint32 VisibleDemand   = bAfterCameraCut ? Math::Max(VisibleWindowed, VisiblePeak) : VisibleWindowed;

        // A level load or mass spawn outruns the lagging demand, so the growth since that readback is reserved too.
        const uint32 SlotCount       = Frame.Geometry.RetainedUpload.SlotCount;
        const uint32 SlotCountBefore = RecentSlotCounts[RecentSlotCursor];
        RecentSlotCounts[RecentSlotCursor] = SlotCount;
        RecentSlotCursor = (RecentSlotCursor + 1u) % (uint32)std::size(RecentSlotCounts);
        const uint32 SlotGrowth = SlotCount > SlotCountBefore ? SlotCount - SlotCountBefore : 0u;

        uint32 VisibleCapacityWanted = Math::Max(VisibleDemand, 4096u) + SlotGrowth;

        const uint32 VisibleCapacityMax = Math::Max(SlotCount, 1u);
        const uint32 RangeViews         = Math::Max(NumCullViews, 1u);

        if (LastVisibleInstances == 0u
            || SceneRendererCullInternal::VisibleInstanceRingBytes(VisibleCapacityMax, RangeViews) <= SceneRendererCullInternal::ExactVisibleBoundBytes())
        {
            VisibleCapacityWanted = VisibleCapacityMax;
        }

        VisibleCapacityWanted = Math::Min(VisibleCapacityWanted, VisibleCapacityMax);

        ReserveBuffer(CL, VisibleInstanceRing[Slot], (SIZE_T)VisibleCapacityWanted * sizeof(FGPUInstance));

        // From the allocation, not the request, so a failed grow cannot hand the cull room it lacks and the grow's slack is kept.
        FrameVisibleInstanceCapacity = (uint32)Math::Min<uint64>(
            VisibleInstanceRing[Slot].Size / sizeof(FGPUInstance), VisibleCapacityMax);

        const SIZE_T InstanceViewRangeSize = Math::Max<SIZE_T>(
            sizeof(uint32) * 2,
            (SIZE_T)FrameVisibleInstanceCapacity * (SIZE_T)RangeViews * sizeof(uint32) * 2);

        ReserveBuffer(CL, InstanceViewRangeRing[Slot], InstanceViewRangeSize);

        // Each visible instance owns a range per view, so a range list that failed to grow caps the instances instead.
        FrameVisibleInstanceCapacity = Math::Min(FrameVisibleInstanceCapacity,
                                                 InstanceViewRangeRing[Slot].CapacityOf<FUIntVector2>() / RangeViews);

        // Every retained slot can become a proxy, so the list never has to report demand. The second half takes the late phase's survivors.
        ReserveBuffer(CL, ProxyListRing[Slot], Math::Max<SIZE_T>(1u, SlotCount) * sizeof(uint32) * 2u);
        // Two draws, then the occlusion cull's dispatch.
        ReserveBuffer(CL, ProxyDrawArgsRing[Slot], sizeof(RHI::FDrawIndirectArguments) * 2u + sizeof(RHI::FDispatchIndirectArguments));
        ProxyListCapacity = ProxyListRing[Slot] ? ProxyListRing[Slot].CapacityOf<uint32>() / 2u : 0u;
    }

    void FDefaultSceneRenderer::ReserveMeshletBlockList(RHI::FCmdListH CL, bool bAfterCameraCut, uint32 SceneListFloor)
    {
        // Only the GPU knows how many blocks were appended, and its counter lags the frames in flight.
        const uint32 BlockListWindowed = Math::Max(SceneRendererCullInternal::WithHeadroom(BlockListDemand.Observe(LastBlocksRequested)), SceneListFloor);
        const uint32 BlockListWanted   = Math::Min(SceneRendererCullInternal::ApplyDebugCapacityScale(bAfterCameraCut ? Math::Max(BlockListWindowed, BlockListPeak) : BlockListWindowed),
                                                   (uint32)SceneRendererCullInternal::kListBudgetEntries);

        const SIZE_T MeshletBlockSize = Math::Max<SIZE_T>(
            sizeof(uint32) * 2,
            (SIZE_T)Math::Max<uint32>(BlockListWanted, 1u) * sizeof(uint32) * 2);
        ReserveBuffer(CL, MeshletBlockRing[CurrentFrameSlot], MeshletBlockSize);
        BlockListCapacity = MeshletBlockRing[CurrentFrameSlot].CapacityOf<FUIntVector2>();
        if (SceneRendererCullInternal::IsDebugCapacityScaled())
        {
            BlockListCapacity = Math::Min(BlockListCapacity, BlockListWanted);
        }
    }

    void FDefaultSceneRenderer::LogCullOverflow()
    {
        // Demand over the cap, not the readback's lag, since only the latter corrects itself.
        const bool bPreSkinCapped = LastPreSkinOverflowed && LastPreSkinRequested > GetMaxPreSkinnedVertices();
        if (!LastVisibleOverflowed && !LastDrawListOverflowed && !LastBlocksOverflowed && !bPreSkinCapped)
        {
            return;
        }
        if ((CullOverflowLogCounter++ % 600u) != 0u)
        {
            return;
        }

        // Requirement is from kFramesInFlight ago, capacity is from now, so print both sides.
        const auto LogOverflow = [](const char* What, uint32 Needed, uint32 GrownTo)
        {
            // At the budget the list can grow no further, so the overflow is the scene's to fix rather than a frame's to wait out.
            if (GrownTo >= (uint32)SceneRendererCullInternal::kListBudgetEntries)
            {
                LOG_WARN("RenderScene: {} is at its {} MiB budget with {} needed. The scene has more finest-level meshlets than the list can hold, "
                         "so instances past the budget drop each frame. Rebuild its meshes with cluster LODs and voxel levels (Mesh.RebuildVirtualGeometry) "
                         "or enable far proxies.",
                         What, SceneRendererCullInternal::kListBudgetBytes >> 20, Needed);
                return;
            }
            LOG_WARN("RenderScene: {} overflowed -- {} needed, capacity now {}. Instances that did not fit "
                     "drew at their coarsest LOD that frame, or not at all without one; this clears once the larger allocation cycles in.",
                     What, Needed, GrownTo);
        };

        if (LastVisibleOverflowed)
        {
            LOG_WARN("RenderScene: visible-instance buffer overflowed, {} needed, capacity now {}. Instances that did not fit "
                     "were skipped in every view that frame; this clears once the larger allocation cycles in.",
                     LastVisibleInstances, FrameVisibleInstanceCapacity);
        }
        if (LastDrawListOverflowed) { LogOverflow("meshlet draw list",       LastDrawListRequired, DrawListCapacity); }
        if (LastBlocksOverflowed)   { LogOverflow("meshlet block list",      LastBlocksRequested,  BlockListCapacity); }
        if (bPreSkinCapped)
        {
            const uint64 WantedMiB = ((uint64)LastPreSkinRequested * sizeof(FPreSkinnedVertex)) >> 20;
            LOG_WARN("RenderScene: pre-skinned vertex budget spent. {} vertices wanted ({} MiB) "
                     "against a {} MiB cap, so the surplus blends bones in every pass instead of "
                     "once. Raise Renderer Settings > Skinning > Pre Skinned Vertex Budget, or cut "
                     "skinned LOD detail.",
                     LastPreSkinRequested, WantedMiB,
                     ((uint64)GetMaxPreSkinnedVertices() * sizeof(FPreSkinnedVertex)) >> 20);
        }
    }

    bool FDefaultSceneRenderer::TakeIBLSourceChange(FIBLBakeSource& Source, const FFrameData& Frame, float SunCosThreshold, bool bResolutionChanged)
    {
        if (!FrameFlags.bHasEnvironment)
        {
            Source.bValid = false;
            return false;
        }

        const auto& LightData = Frame.Lighting.LightData;
        const FEnvironmentParams& Params = Frame.Volumetrics.EnvironmentParams;
        const int32 MapID   = Frame.Volumetrics.EnvironmentMapID;
        const bool  bHasSun = LightData.bHasSun != 0;

        // Directions are compared only between two suns, since a sunless frame leaves the direction zero.
        const bool bSunChanged = Source.bHasSun != bHasSun
            || (bHasSun && Math::Dot(Source.SunDirection, LightData.SkySunDirection) < SunCosThreshold);
        const bool bChanged = !Source.bValid || bSunChanged || bResolutionChanged || Source.MapID != MapID
            || Memory::Memcmp(&Source.Params, &Params, sizeof(FEnvironmentParams)) != 0;
        if (bChanged)
        {
            Source.Params       = Params;
            Source.MapID        = MapID;
            Source.SunDirection = LightData.SkySunDirection;
            Source.bHasSun      = bHasSun;
            Source.bValid       = true;
        }
        return bChanged;
    }

    void FDefaultSceneRenderer::UpdateIBLDirty(FFrameData& Frame)
    {
        const bool bResolutionChanged = Frame.Volumetrics.IBLResolution != LastExtractedIBLResolution;
        LastExtractedIBLResolution = Frame.Volumetrics.IBLResolution;

        // A day cycle moves the sun every frame, and a sub-quarter-degree step is invisible in reflections but costs a whole cube bake.
        constexpr float SkyCubeSunCosThreshold     = 0.99999f;
        constexpr float ConvolutionSunCosThreshold = 0.99996f;
        if (TakeIBLSourceChange(SkyCubeBakeSource, Frame, SkyCubeSunCosThreshold, bResolutionChanged))
        {
            Frame.Volumetrics.bIBLDirty = true;
        }
        if (TakeIBLSourceChange(ConvolutionBakeSource, Frame, ConvolutionSunCosThreshold, bResolutionChanged))
        {
            Frame.Volumetrics.bIBLConvolutionDirty = true;
        }
    }

    void FDefaultSceneRenderer::UploadLightData(FFrameData& Frame)
    {
        auto& LightData = Frame.Lighting.LightData;

        // Only the live prefix of each array reaches the ring; the header carries where they landed.
        LightData.Lights  = RHI::CopyTransientArray(Frame.Lighting.Lights.GetData(),  NumLiveLights);
        LightData.Shadows = RHI::CopyTransientArray(Frame.Lighting.Shadows.GetData(), NumLiveShadows);
        const uint32 NumLightFunctions = (uint32)Frame.Lighting.LightFunctionDraws.Num();
        if (NumLightFunctions != 0 && !LightFunctionAtlas)
        {
            RHI::FTextureDesc AtlasDesc;
            AtlasDesc.Type      = RHI::ETextureType::Tex2D;
            AtlasDesc.Dimension = FUIntVector3(LIGHT_FUNCTION_ATLAS_TILES * LIGHT_FUNCTION_TILE_SIZE,
                                               LIGHT_FUNCTION_ATLAS_TILES * LIGHT_FUNCTION_TILE_SIZE, 1);
            AtlasDesc.Format    = EFormat::RGBA8_UNORM;
            AtlasDesc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::ColorAttachment;
            LightFunctionAtlas  = CreateSceneImage(AtlasDesc);
        }
        LightData.LightFunctions     = RHI::CopyTransientArray(Frame.Lighting.LightFunctions.GetData(), NumLightFunctions);
        LightData.LightFunctionAtlas = LightFunctionAtlas ? (uint32)LightFunctionAtlas.GetResourceID() : 0u;
        SceneBindings.Lights = RHI::CopyTransient(LightData);
    }

    void FDefaultSceneRenderer::PublishSceneRoot(RHI::FCmdListH CL, FFrameData& Frame)
    {
        FSceneGlobalData& SceneGlobalData = Frame.SceneGlobalData;

        SceneRootShared = FSceneRoot{};
        if (VisibleInstanceRing[CurrentFrameSlot])
        {
            SceneBindings.Instances = VisibleInstanceRing[CurrentFrameSlot].Gpu;
        }
        // Persistent and slot-addressed, so this is just the arena's address.
        if (BoneArenaRing[CurrentFrameSlot])
        {
            SceneRootShared.Bones = { GetBoneArena() };
        }

        // Last frame's snapshot, so this publishes what SnapshotMotionState left at the end of it.
        if (bPrevMotionStateValid && PrevRetainedTransformBuffer)
        {
            SceneRootShared.PrevRetainedTransforms = { PrevRetainedTransformBuffer };

            // The other slot's arena still holds last frame's poses when it took the upload before this one.
            const uint32 PrevSlot = (CurrentFrameSlot + RHI::kFramesInFlight - 1u) % RHI::kFramesInFlight;
            if (BoneArenaRing[PrevSlot] && BoneArenaSyncedSerial[PrevSlot] + 1u == BoneArenaSyncedSerial[CurrentFrameSlot])
            {
                SceneRootShared.PrevBones = { BoneArenaRing[PrevSlot] };
            }
        }

        const auto& BillboardInstances = Frame.Primitives.BillboardInstances;
        if (!BillboardInstances.IsEmpty())
        {
            SceneRootShared.Billboards = RHI::CopyTransientArray(BillboardInstances.GetData(), BillboardInstances.Num());
        }
        const auto& CullViews = Frame.Views.CullViews;
        if (!CullViews.IsEmpty())
        {
            SceneRootShared.CullViews = RHI::CopyTransientArray(CullViews.GetData(), CullViews.Num());
        }
        if (!Frame.Primitives.WidgetInstances.IsEmpty())
        {
            SceneRootShared.Widgets = RHI::CopyTransientArray(Frame.Primitives.WidgetInstances.GetData(), Frame.Primitives.WidgetInstances.Num());
        }

        // Splines are small and bounded, so the shared transient ring is the right home.
        NumActiveSplines       = (uint32)Frame.Splines.Splines.Num();
        SplineBufferSpan       = {};
        SplinePointBufferSpan  = {};
        SplineSampleBufferSpan = {};
        if (NumActiveSplines > 0)
        {
            SplineBufferSpan = RHI::CopyTransientArray(Frame.Splines.Splines.GetData(), Frame.Splines.Splines.Num());
            if (!Frame.Splines.Points.IsEmpty())
            {
                SplinePointBufferSpan = RHI::CopyTransientArray(Frame.Splines.Points.GetData(), Frame.Splines.Points.Num());
            }
            if (!Frame.Splines.Samples.IsEmpty())
            {
                SplineSampleBufferSpan = RHI::CopyTransientArray(Frame.Splines.Samples.GetData(), Frame.Splines.Samples.Num());
            }
        }

        NumActiveProbes  = (uint32)Frame.ReflectionProbes.Probes.Num();
        ProbeBufferSpan  = {};
        if (NumActiveProbes > 0)
        {
            InitReflectionProbeTargets();
            ProbeBufferSpan = RHI::CopyTransientArray(Frame.ReflectionProbes.Probes.GetData(), Frame.ReflectionProbes.Probes.Num());
        }

        SceneRootShared.Materials            = Render().GetMaterialManager().GetMaterialTable();
        SceneRootShared.Collections          = Render().GetCollectionManager().GetSpan();
        SceneRootShared.MeshletDrawList      = { GetMeshletDrawList(), DrawListCapacity };
        SceneRootShared.PreSkinnedVertices   = { GetPreSkinnedVerticesBuffer(), PreSkinnedVertexCapacity };
        SceneRootShared.PreSkinnedPrevPositions = { PreSkinnedPrevPositionsBuffer, PreSkinnedPrevCapacity };
        // Spheres and cones are indexed by one base, so each carries the same capacity.
        SceneRootShared.SkinnedMeshletBounds = { SkinnedMeshletBoundsBuffer, SkinnedMeshletBoundsCapacity };
        SceneRootShared.SkinnedMeshletCones  = { SkinnedMeshletConeBuffer, SkinnedMeshletBoundsCapacity };

        PublishViewImageIndices(Frame);
        PublishFogGlobals(SceneGlobalData);
        PublishWeatherGlobals(SceneGlobalData);

        SetSceneRoot(CL, *CurrentView, RHI::CopyTransient(SceneGlobalData));
    }

    void FDefaultSceneRenderer::PublishViewImageIndices(FFrameData& Frame)
    {
        FSceneGlobalData& SceneGlobalData = Frame.SceneGlobalData;
        const auto& Images = CurrentView->Images;

        if (IsGTAOEnabled())
        {
            SceneGlobalData.GTAOSettings.AOTextureIndex = (uint32)Images[(int)ENamedImage::GTAOBlur].GetResourceID();
        }
        if (Images[(int)ENamedImage::SunFarShadowMask].IsValid() && WantsSunFarShadowMask())
        {
            SceneGlobalData.GTAOSettings.SunShadowMaskIndex = (uint32)Images[(int)ENamedImage::SunFarShadowMask].GetResourceID();
        }
        if (IsScreenSpaceGITraced())
        {
            SceneGlobalData.GTAOSettings.IndirectDiffuseIndex = (uint32)Images[(int)ENamedImage::ScreenSpaceGI].GetResourceID();
        }

        SceneGlobalData.SceneDepthIndex = (uint32)Images[(int)ENamedImage::DepthAttachment].GetResourceID();

        const bool bDecals = !Frame.Primitives.DecalExtracts.IsEmpty();
        SceneGlobalData.DBufferAIndex = bDecals ? (uint32)Images[(int)ENamedImage::DBufferA].GetResourceID() : Constants::kIndexNoneU32;
        SceneGlobalData.DBufferBIndex = bDecals ? (uint32)Images[(int)ENamedImage::DBufferB].GetResourceID() : Constants::kIndexNoneU32;
        SceneGlobalData.DBufferCIndex = bDecals ? (uint32)Images[(int)ENamedImage::DBufferC].GetResourceID() : Constants::kIndexNoneU32;
        SceneGlobalData.DBufferDIndex = bDecals ? (uint32)Images[(int)ENamedImage::DBufferD].GetResourceID() : Constants::kIndexNoneU32;
    }

    void FDefaultSceneRenderer::SkinningPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        // Two blocks per gathered skinned SLOT, the surface LOD and the coarser shadow LOD.
        const uint32 NumSkinned = Math::Min((uint32)Frame.Geometry.SkinnedIndices.Num(), SkinnedSlotsUploaded);
        if (NumSkinned == 0)
        {
            return;
        }

        // The GPU scope is opened by the caller, not here.
        LUMINA_PROFILE_SECTION_COLORED("Skinning Pass", tracy::Color::SkyBlue);

        static const FShaderH WorkShader = FShaderLibrary::Get("BuildSkinWork.slang");
        static const FShaderH SkinShader = FShaderLibrary::Get("Skinning.slang");
        static const FShaderH EntryShader = FShaderLibrary::Get("SkinWorkEntries.slang");
        if (!SkinShader || !WorkShader || !EntryShader)
        {
            return;
        }

        const uint8 Slot = CurrentFrameSlot;

        ReserveBuffer(CL, SkinWorkBaseRing[Slot], (uint64)NumSkinned * sizeof(uint32));
        ReserveBuffer(CL, SkinWorkEntryRing[Slot], (uint64)Math::Max(SkinWorkCapacity, 1u) * sizeof(uint32));
        if (SkinWorkBaseRing[Slot].CapacityOf<uint32>() < NumSkinned
            || !SkinWorkEntryRing[Slot] || !GetSkinDispatchArgs() || !GetPreSkinnedVerticesBuffer()
            || !SkinnedSlotListBuffer || !SkinnedFrameDataBuffer || !RetainedStaticBuffer)
        {
            return;
        }

        const uint32 WorkEntryCount = Math::Min(SkinWorkCapacity, SkinWorkEntryRing[Slot].CapacityOf<uint32>());

        //~ Lay out the dispatch as one workgroup per instance and meshlet, enumerated on the GPU.
        {
            SCENE_GPU_SCOPE(CL, "Build Skin Work");

            struct FBuildSkinWorkPC
            {
                RHI::TGPUSpan<uint32>            SlotList;
                RHI::TGPUSpan<FSkinnedFrameData> SkinnedData;
                RHI::TGPUSpan<uint32>            OutWorkBase;
                RHI::TGPUSpan<RHI::FDispatchIndirectArguments> OutDispatchArgs;
                RHI::TGPUSpan<uint32>            WorkEntry;
            } WPC = {};
            static_assert(sizeof(FBuildSkinWorkPC) == 80, "FBuildSkinWorkPC must match BuildSkinWork.slang.");

            WPC.SlotList        = { SkinnedSlotListBuffer, NumSkinned };
            WPC.SkinnedData     = { SkinnedFrameDataBuffer };
            WPC.OutWorkBase     = { SkinWorkBaseRing[Slot], NumSkinned };
            WPC.OutDispatchArgs = { GetSkinDispatchArgs() };
            WPC.WorkEntry       = { SkinWorkEntryRing[Slot], WorkEntryCount };

            DispatchCompute(CL, WorkShader, WPC, 1u, 1u, 1u);

            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);
        }

        //~ Stamp each work index with its entry, one thread per entry so the stores spread over the whole GPU.
        {
            SCENE_GPU_SCOPE(CL, "Skin Work Entries");

            struct FSkinWorkEntriesPC
            {
                RHI::TGPUSpan<uint32>            SlotList;
                RHI::TGPUSpan<FSkinnedFrameData> SkinnedData;
                RHI::TGPUSpan<uint32>            WorkBase;
                RHI::TGPUSpan<uint32>            OutWorkEntry;
            } EPC = {};
            static_assert(sizeof(FSkinWorkEntriesPC) == 64, "FSkinWorkEntriesPC must match SkinWorkEntries.slang.");

            EPC.SlotList     = { SkinnedSlotListBuffer, NumSkinned };
            EPC.SkinnedData  = { SkinnedFrameDataBuffer };
            EPC.WorkBase     = { SkinWorkBaseRing[Slot], NumSkinned };
            EPC.OutWorkEntry = { SkinWorkEntryRing[Slot], WorkEntryCount };

            const FUIntVector2 Groups = RenderUtils::FoldGroupCount(RenderUtils::GetGroupCount(NumSkinned, 64u));
            DispatchCompute(CL, EntryShader, EPC, Groups.x, Groups.y, 1u);

            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead);
        }

        struct FSkinningPushConstants
        {
            RHI::TGPUSpan<uint32>             WorkBase;
            RHI::TGPUSpan<uint32>             WorkEntry;
            RHI::TGPUSpan<uint32>             SlotList;
            RHI::TGPUSpan<FSkinnedFrameData>  SkinnedData;
            RHI::TGPUSpan<FInstanceStatic>    RetainedStatic;
            RHI::TGPUSpan<FPreSkinnedVertex>  OutVertices;
            RHI::TGPUSpan<FPrevSkinnedPosition> OutPrevPositions;
        } PC = {};
        static_assert(sizeof(FSkinningPushConstants) == 112, "FSkinningPushConstants must match Skinning.slang.");

        PC.WorkBase       = { SkinWorkBaseRing[Slot], NumSkinned };
        PC.WorkEntry      = { SkinWorkEntryRing[Slot], WorkEntryCount };
        PC.SlotList       = { SkinnedSlotListBuffer, NumSkinned };
        PC.SkinnedData    = { SkinnedFrameDataBuffer };
        PC.RetainedStatic = { RetainedStaticBuffer };
        PC.OutVertices    = { GetPreSkinnedVerticesBuffer() };
        PC.OutPrevPositions = { PreSkinnedPrevPositionsBuffer, PreSkinnedPrevCapacity };

        {
            SCENE_GPU_SCOPE(CL, "Skin Vertices");
            DispatchComputeIndirect(CL, SkinShader, PC, GetSkinDispatchArgs());
        }

        // Pre-skinned vertices feed every draw VS.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::MeshShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndexRead);
    }

    bool FDefaultSceneRenderer::BuildDepthPyramid(RHI::FCmdListH CL, const FSceneImage& Source, const FSceneImage& Pyramid, bool bReduceMax,
                                                  uint32 SpdCounterIndex, uint32 SourceScale)
    {
        static const FShaderH ComputeShader = FShaderLibrary::Get("DepthPyramidSPD.slang");
        if (!ComputeShader)
        {
            return false;
        }

        const RHI::FGPURange SpdCounter = GetSpdCounter(SpdCounterIndex);

        const uint32 PyramidW = Pyramid.GetSizeX();
        const uint32 PyramidH = Pyramid.GetSizeY();
        const uint32 MipCount = Pyramid.GetNumMips();

        constexpr uint32 SpdMaxMips = 12;
        const uint32 NumMips = Math::Min(MipCount, SpdMaxMips);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::FragmentTests, RHI::EAccessFlags::ColorWrite | RHI::EAccessFlags::DepthStencilWrite,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        struct FSpdPushConstants
        {
            uint32 PyramidSize[2];
            uint32 NumMips;
            uint32 NumWorkGroups;
            float  InvPyramidSize[2];
            uint32 SrcDepthIndex;
            uint32 ReduceMax;
            RHI::TGPUSpan<uint32> AtomicCounter;
            uint32 MipUAV[SpdMaxMips];
            float  InvSrcSize[2];
            uint32 SrcScale;
        } PC = {};

        constexpr uint32 SpdTileSize = 32;
        const uint32 DispatchX = RenderUtils::GetGroupCount(PyramidW, SpdTileSize);
        const uint32 DispatchY = RenderUtils::GetGroupCount(PyramidH, SpdTileSize);
        const uint32 TotalGroups = DispatchX * DispatchY;

        PC.PyramidSize[0]     = PyramidW;
        PC.PyramidSize[1]     = PyramidH;
        PC.NumMips            = NumMips;
        PC.NumWorkGroups      = TotalGroups;
        PC.InvPyramidSize[0]  = 1.0f / (float)PyramidW;
        PC.InvPyramidSize[1]  = 1.0f / (float)PyramidH;
        const int32 SrcDepthSlot = Source.GetResourceID();
        if (SrcDepthSlot < 0)
        {
            LOG_ERROR_ONCE("Depth pyramid: source image has no sampled heap slot; skipping. Sampling it would "
                           "index the texture heap with 0xFFFFFFFF and fault the device.");
            return false;
        }

        PC.SrcDepthIndex      = (uint32)SrcDepthSlot;
        PC.ReduceMax          = bReduceMax ? 1u : 0u;
        PC.AtomicCounter      = { SpdCounter };
        PC.InvSrcSize[0]      = 1.0f / (float)Math::Max(Source.GetSizeX(), 1u);
        PC.InvSrcSize[1]      = 1.0f / (float)Math::Max(Source.GetSizeY(), 1u);
        PC.SrcScale           = SourceScale;
        for (uint32 i = 0; i < SpdMaxMips; ++i)
        {
            const uint32 SrcMip = (i < MipCount) ? i : 0u;
            const int32  Slot   = Pyramid.GetMipUAVIndex(SrcMip);
            if (Slot < 0)
            {
                LOG_ERROR_ONCE("Depth pyramid: mip {} has no storage heap slot; skipping the pass.", SrcMip);
                return false;
            }
            PC.MipUAV[i] = (uint32)Slot;
        }

        RHI::CmdDispatch(CL, MakeArgs(PC), DispatchX, DispatchY, 1);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        return true;
    }

    void FDefaultSceneRenderer::DepthPyramidPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        // Next frame's occlusion cull trusts the pyramid only if this frame rebuilt it; a frozen cull skips this pass and keeps the last one.
        if (DrawCommands.IsEmpty())
        {
            bDepthPyramidValid.store(false, std::memory_order_release);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Depth Pyramid Pass (SPD)", tracy::Color::Orange);

        // Reverse-Z scene depth, so MIN reduction keeps the farthest occluder in each footprint.
        const bool bBuilt = BuildDepthPyramid(CL,
            GetNamedImage(ENamedImage::DepthAttachment),
            GetNamedImage(ENamedImage::DepthPyramid),
            /*bReduceMax*/ false, /*SpdCounterIndex*/ 0u);
        bDepthPyramidValid.store(bBuilt, std::memory_order_release);
    }

    void FDefaultSceneRenderer::CascadePyramidPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& LightData   = Frame.Lighting.LightData;

        if (!LightData.bHasSun ||
            Frame.SceneGlobalData.CullData.bShadowOcclusionCull == 0u ||
            Frame.Geometry.DrawCommands.IsEmpty() ||
            Frame.Lighting.Lights[0].ShadowDataIndex == Constants::kIndexNone ||
            Frame.Views.CascadeViewBase == Constants::kIndexNoneU32)
        {
            bCascadePyramidValid.store(false, std::memory_order_release);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cascade Pyramid Pass (SPD)", tracy::Color::Orange3);

        const bool bBuilt = BuildDepthPyramid(CL,
            GetNamedImage(ENamedImage::Cascade),
            GetNamedImage(ENamedImage::CascadePyramid),
            /*bReduceMax*/ true, /*SpdCounterIndex*/ 1u, /*SourceScale*/ 4u);

        bCascadePyramidValid.store(bBuilt, std::memory_order_release);
    }

    void FDefaultSceneRenderer::UpdateMeshletBoundFeedback(uint8 Slot)
    {
        const RHI::FGPUAllocation& Readback = MeshletBoundReadback[Slot];
        if (Readback.Gpu == 0)
        {
            return;
        }
        static_assert(FDefaultSceneRenderer::kTotalsSlots >= 9, "Totals[8] is read below.");
        if (const uint32* Mapped = Readback.CpuAs<const uint32>())
        {
            LastVisibleInstances     = Mapped[0];
            LastVisibleOverflowed    = Mapped[1];
            LastDrawListRequired     = Math::Max(Mapped[2], Mapped[8]);
            LastDrawListOverflowed   = Mapped[3];
            LastBlocksRequested      = Mapped[4];
            LastBlocksOverflowed     = Mapped[5];
            LastPreSkinRequested     = Mapped[6];
            LastPreSkinOverflowed    = Mapped[7];
        }

        LUMINA_PROFILE_VALUE("Cull/VisibleInstances",  LastVisibleInstances);
        LUMINA_PROFILE_VALUE("Cull/DrawListEntries",   LastDrawListRequired);
        LUMINA_PROFILE_VALUE("Cull/MeshletBlocks",     LastBlocksRequested);
        LUMINA_PROFILE_VALUE("Cull/PreSkinVertices",   LastPreSkinRequested);
    }

    void FDefaultSceneRenderer::DispatchGPUSceneCull(RHI::FCmdListH CL, const FFrameData& Frame)
    {
        bCullBucketsValid = false;

        static const FShaderH CullInstancesShader = FShaderLibrary::Get("CullInstances.slang");
        static const FShaderH DrawPrefixShader = FShaderLibrary::Get("BuildDrawPrefix.slang");
        if (CullInstancesShader == nullptr || DrawPrefixShader == nullptr)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("GPU Scene Cull", tracy::Color::Magenta);
        SCENE_GPU_SCOPE(CL, "GPU Scene Cull");

        const FFrameData::FGeometry::FRetainedUpload& Upload = Frame.Geometry.RetainedUpload;

        const uint8  Slot          = CurrentFrameSlot;
        const uint32 RetainedSlots = Upload.SlotCount;
        const uint32 NumBatches    = Math::Max(Frame.Views.NumDrawsPerView, 1u);
        const uint32 NumCullViews  = (uint32)Frame.Views.CullViews.Num();
        LUMINA_PROFILE_VALUE("Cull/RetainedSlots", RetainedSlots);

        if (!TotalsZeroed[Slot] && GetTotals())
        {
            RHI::CmdMemset(CL, GetTotals(), 0u);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            TotalsZeroed[Slot] = true;
        }

        const bool bRetainedFits = UploadRetainedInstances(CL, Upload);
        UploadSurfaceDescs(CL, Upload);

        const SIZE_T ViewDrawEntries = (SIZE_T)Math::Max(NumCullViews, 1u) * (SIZE_T)NumBatches;
        // MeshDrawArgsRing is deliberately NOT resized here; CompileDrawCommands_Render sizes it.
        ReserveBuffer(CL, RenderBucketRing[Slot], ViewDrawEntries * sizeof(FRenderBucketGPU));

        // Every pass after this indexes the buckets by view and batch, so a list that failed to grow skips the frame's draws.
        bCullBucketsValid = (SIZE_T)RenderBucketRing[Slot].CapacityOf<FRenderBucketGPU>() >= ViewDrawEntries;
        if (bCullBucketsValid)
        {
            // Every field starts at zero, since CullInstances accumulates skinned batches too.
            RHI::CmdMemset(CL, { GetRenderBuckets().Gpu, ViewDrawEntries * sizeof(FRenderBucketGPU) }, 0u);
        }

        Barriers::TransferToCompute(CL);

        // Reads RetainedStatic, so it has to sit after that upload and after the barrier publishing it.
        SkinnedMeshletBoundsPass(CL, Frame);

        if (!bCullBucketsValid)
        {
            return;
        }

        // A retained buffer that failed to grow is shorter than the slot range the cull would walk.
        if (bRetainedFits && RetainedSlots > 0 && NumCullViews > 0 && Upload.SurfaceDescCount > 0 && VisibleInstanceRing[Slot])
        {
            DispatchCullInstances(CL, Frame, CullInstancesShader);
        }

        // Rebuilt because the retained arrays and the proxy list are only known now, after PublishSceneRoot took its copy.
        SetSceneRoot(CL, *CurrentView, SceneBindings.SceneData);

        DispatchDrawPrefix(CL, Frame, DrawPrefixShader);

        if (NumCullViews > 0u)
        {
            LUMINA_PROFILE_SECTION_COLORED("Build Meshlet Blocks", tracy::Color::Magenta2);
            SCENE_GPU_SCOPE(CL, "Build Meshlet Blocks");
            DispatchMeshletBlockBuild(CL, /*bLatePass*/ false);
        }

        // Every view's meshlets, culled once; everything after reads what this wrote.
        MeshletCullPass(CL, EMeshletSlice::Early);

        CopyCullTotalsForReadback(CL);
    }

    bool FDefaultSceneRenderer::UploadRetainedInstances(RHI::FCmdListH CL, const FFrameData::FGeometry::FRetainedUpload& Upload)
    {
        LUMINA_PROFILE_SECTION_COLORED("Retained Upload", tracy::Color::Magenta4);
        SCENE_GPU_SCOPE(CL, "Retained Upload");

        const uint32 RetainedSlots = Upload.SlotCount;

        // Only when something is sent, since waiting on every earlier stage stalls the frame head for nothing otherwise.
        const bool bUploadsThisFrame = Upload.bFull || Upload.bFullStatic || !Upload.DirtySlots.IsEmpty() || !Upload.DirtyStaticSlots.IsEmpty()
                                    || !Upload.DeactivatedSlots.IsEmpty();
        if (bUploadsThisFrame)
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::AllCommands, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
        }

        const SIZE_T CullBytes      = Math::Max<SIZE_T>(sizeof(FInstanceCullEntry), (SIZE_T)RetainedSlots * sizeof(FInstanceCullEntry));
        const SIZE_T TransformBytes = Math::Max<SIZE_T>(sizeof(FTransform3x4),      (SIZE_T)RetainedSlots * sizeof(FTransform3x4));
        const SIZE_T StaticBytes    = Math::Max<SIZE_T>(sizeof(FInstanceStatic),    (SIZE_T)RetainedSlots * sizeof(FInstanceStatic));

        // Zeroed on growth because a free slot IS zero; a preserve copy would clobber the full re-send that rewrites live slots ahead of this list.
        ReserveBuffer(CL, RetainedCullEntryBuffer, CullBytes,      /*bAllowShrink*/ Upload.bFull, /*bPreserveContents*/ !Upload.bFull,
                      Upload.bFull ? CullBytes : 0u);
        ReserveBuffer(CL, RetainedTransformBuffer, TransformBytes, /*bAllowShrink*/ Upload.bFull, /*bPreserveContents*/ !Upload.bFull,
                      Upload.bFull ? TransformBytes : 0u);
        ReserveBuffer(CL, RetainedStaticBuffer,    StaticBytes,    /*bAllowShrink*/ Upload.bFullStatic, /*bPreserveContents*/ !Upload.bFullStatic,
                      Upload.bFullStatic ? StaticBytes : 0u);
        const SIZE_T ProxyBytes = Math::Max<SIZE_T>(sizeof(FInstanceProxy), (SIZE_T)RetainedSlots * sizeof(FInstanceProxy));
        ReserveBuffer(CL, RetainedProxyBuffer,     ProxyBytes,     /*bAllowShrink*/ Upload.bFullStatic, /*bPreserveContents*/ !Upload.bFullStatic,
                      Upload.bFullStatic ? ProxyBytes : 0u);

        // Flipped so last frame's set stays readable all frame; both dispatches take their phase from it.
        InstanceVisibilityWriteIndex ^= 1u;
        ++InstanceVisibilityTag;

        const SIZE_T VisBytes = Math::Max<SIZE_T>(sizeof(uint32), (SIZE_T)RetainedSlots * sizeof(uint32));
        uint32 VisCapacity = 0xFFFFFFFFu;
        for (uint32 v = 0; v < 2u; ++v)
        {
            // Zeroed because zero is the one tag no frame ever stamps, so a new slot has no history.
            ReserveBuffer(CL, InstanceVisibilityBuffers[v], VisBytes);

            VisCapacity = Math::Min(VisCapacity, InstanceVisibilityBuffers[v]
                ? InstanceVisibilityBuffers[v].CapacityOf<uint32>()
                : 0u);
        }

        InstanceVisibilityCapacity = VisCapacity;

        // History is kept across a grow, since the zeroed tail is the only part with none.
        ReserveBuffer(CL, MeshletVisibilityBuffer, Math::Max<SIZE_T>(sizeof(uint32), (SIZE_T)Upload.MeshletVisibilityWords * sizeof(uint32)),
                      /*bAllowShrink*/ false, /*bPreserveContents*/ true);
        MeshletVisibilityCapacity = MeshletVisibilityBuffer ? MeshletVisibilityBuffer.CapacityOf<uint32>() : 0u;

        // A grow that failed leaves a buffer shorter than the slot range, and the writes below are unchecked.
        const bool bRetainedFits = RetainedCullEntryBuffer.CapacityOf<FInstanceCullEntry>() >= RetainedSlots
                                && RetainedTransformBuffer.CapacityOf<FTransform3x4>()   >= RetainedSlots
                                && RetainedStaticBuffer.CapacityOf<FInstanceStatic>()    >= RetainedSlots
                                && RetainedProxyBuffer.CapacityOf<FInstanceProxy>()      >= RetainedSlots;
        if (RetainedCullEntryBuffer && RetainedTransformBuffer && RetainedStaticBuffer && RetainedProxyBuffer && RetainedSlots > 0 && bRetainedFits)
        {
            WriteRetainedInstances(CL, Upload);
        }
        else
        {
            JoinInstanceBlockBounds();
            bBlockBoundsLaunched      = false;
            bInstanceBlockBoundsValid = false;
        }

        const uint32 CullCap      = RetainedCullEntryBuffer.CapacityOf<FInstanceCullEntry>();
        const uint32 TransformCap = RetainedTransformBuffer.CapacityOf<FTransform3x4>();
        const uint32 StaticCap    = RetainedStaticBuffer.CapacityOf<FInstanceStatic>();
        // Zero when the slots did not fit, so the dirty slots skipped above come back as a full upload.
        RetainedDeviceCapacity.store(bRetainedFits ? Math::Min(CullCap, Math::Min(TransformCap, StaticCap)) : 0u,
                                     std::memory_order_release);
        RetainedStaticCapacity = StaticCap;

        // Published after the grow, so the proxy pass reads the arrays the cull reads this frame and not a retired copy.
        const uint32 ProxyCap = RetainedProxyBuffer.CapacityOf<FInstanceProxy>();
        SceneRootShared.RetainedTransforms = bRetainedFits ? RHI::TGPUSpan<FTransform3x4>{ RetainedTransformBuffer, Math::Min(RetainedSlots, TransformCap) } : RHI::TGPUSpan<FTransform3x4>{};
        SceneRootShared.RetainedStatic     = bRetainedFits ? RHI::TGPUSpan<FInstanceStatic>{ RetainedStaticBuffer, Math::Min(RetainedSlots, StaticCap) } : RHI::TGPUSpan<FInstanceStatic>{};
        SceneRootShared.RetainedProxies    = bRetainedFits ? RHI::TGPUSpan<FInstanceProxy>{ RetainedProxyBuffer, Math::Min(RetainedSlots, ProxyCap) } : RHI::TGPUSpan<FInstanceProxy>{};
        return bRetainedFits;
    }

    void FDefaultSceneRenderer::WriteRetainedInstances(RHI::FCmdListH CL, const FFrameData::FGeometry::FRetainedUpload& Upload)
    {
        using namespace SceneRendererCullInternal;

        const uint32 RetainedSlots = Upload.SlotCount;
        const FInstanceCullEntry* SrcCullEntries = ScenePrimitives.GetRetainedCullEntries();
        const FTransform3x4*      SrcTransforms  = ScenePrimitives.GetRetainedTransforms();
        const FInstanceStatic*    SrcStatic      = ScenePrimitives.GetRetainedStatic();

        // Ahead of the writes, so a slot freed and reused this frame ends with its new entry.
        if (!Upload.bFull && !Upload.DeactivatedSlots.IsEmpty())
        {
            DeactivateRetainedSlots(CL, Upload.DeactivatedSlots);
        }

        if (Upload.bFull)
        {
            LUMINA_PROFILE_SECTION_COLORED("FULL resend", tracy::Color::Red3);

            StageWrite(RetainedCullEntryBuffer.Gpu,
                       SrcCullEntries, (SIZE_T)RetainedSlots * sizeof(FInstanceCullEntry), true);
            StageWrite(RetainedTransformBuffer.Gpu,
                       SrcTransforms, (SIZE_T)RetainedSlots * sizeof(FTransform3x4), true);
            FlushStagedWrites(CL);
        }
        else
        {
            // Both buffers share the slot list, so one run scan feeds both writes.
            CollectSlotRuns(Upload.DirtySlots, RetainedRunScratch);

            // A crowd dirties thousands of short runs, which cost more to record as copies than to scatter on the GPU.
            constexpr SIZE_T ScatterUploadMinSlots     = 1024;
            constexpr SIZE_T ScatterUploadMaxRunLength = 64;
            const int32 NumDirty = Upload.DirtySlots.Num();
            if (static_cast<size_t>(NumDirty) >= ScatterUploadMinSlots && RetainedRunScratch.Num() * ScatterUploadMaxRunLength > static_cast<size_t>(NumDirty))
            {
                const RHI::FGPURange Slots = RHI::CopyTransientArray(Upload.DirtySlots.GetData(), NumDirty);

                // The previous frame read these buffers in every stage, and the scatter overwrites them.
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::AllCommands, RHI::EAccessFlags::ShaderRead,
                    RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite);

                WriteBufferScatter(CL, RetainedCullEntryBuffer.Gpu, RetainedCullEntryBuffer.Size, SrcCullEntries,
                                   sizeof(FInstanceCullEntry), Slots, Upload.DirtySlots, /*bFillBeforeSubmit*/ true);
                WriteBufferScatter(CL, RetainedTransformBuffer.Gpu, RetainedTransformBuffer.Size, SrcTransforms,
                                   sizeof(FTransform3x4), Slots, Upload.DirtySlots, /*bFillBeforeSubmit*/ true);

                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                    RHI::EStageFlags::AllCommands, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            }
            else
            {
                WriteBufferRuns(CL, RetainedCullEntryBuffer.Gpu, SrcCullEntries, sizeof(FInstanceCullEntry), RetainedRunScratch, true);
                WriteBufferRuns(CL, RetainedTransformBuffer.Gpu, SrcTransforms,  sizeof(FTransform3x4),      RetainedRunScratch, true);
            }
        }

        ScatterTransformStream(CL, Upload);

        // The proxies share the static dirtiness, so one run scan feeds both writes.
        const FInstanceProxy* SrcProxies = ScenePrimitives.GetRetainedProxies();
        if (Upload.bFullStatic)
        {
            StageWrite(RetainedStaticBuffer.Gpu, SrcStatic, (SIZE_T)RetainedSlots * sizeof(FInstanceStatic), true);
            StageWrite(RetainedProxyBuffer.Gpu, SrcProxies, (SIZE_T)RetainedSlots * sizeof(FInstanceProxy), true);
            FlushStagedWrites(CL);
        }
        else
        {
            CollectSlotRuns(Upload.DirtyStaticSlots, RetainedRunScratch);
            WriteBufferRuns(CL, RetainedStaticBuffer.Gpu, SrcStatic, sizeof(FInstanceStatic), RetainedRunScratch, true);
            WriteBufferRuns(CL, RetainedProxyBuffer.Gpu, SrcProxies, sizeof(FInstanceProxy), RetainedRunScratch, true);
        }
        FinishInstanceBlockBounds(CL);
        LaunchDeferredStageFills();
    }

    void FDefaultSceneRenderer::UploadSurfaceDescs(RHI::FCmdListH CL, const FFrameData::FGeometry::FRetainedUpload& Upload)
    {
        // Surface descriptors are interned, so this moves only when a new distinct LOD table appears.
        const uint32 NumDescs = Upload.SurfaceDescCount;
        const SIZE_T DescBytes = Math::Max<SIZE_T>(sizeof(FSurfaceDescGPU), (SIZE_T)NumDescs * sizeof(FSurfaceDescGPU));
        const RHI::GPUPtr PrevDescs = SurfaceDescBuffer.Gpu;
        // Same reasoning as the retained buffers, since a reclaim drops descriptors this frame may not re-send.
        ReserveBuffer(CL, SurfaceDescBuffer, DescBytes, /*bAllowShrink*/ Upload.bSurfaceDescsChanged);

        if (SurfaceDescBuffer.Gpu != PrevDescs)
        {
            UploadedSurfaceDescs = 0;
        }

        const bool bNeedsDescWrite = SurfaceDescBuffer && NumDescs > 0
                                  && (Upload.bSurfaceDescsChanged || UploadedSurfaceDescs != NumDescs);
        if (!bNeedsDescWrite)
        {
            return;
        }

        // A list that failed to grow keeps what it holds, and the zero capacity forces a full re-send once it can.
        if (ScenePrimitives.GetSurfaceDescCount() == NumDescs && SurfaceDescBuffer.CapacityOf<FSurfaceDescGPU>() >= NumDescs)
        {
            WriteBuffer(CL, SurfaceDescBuffer.Gpu,
                        ScenePrimitives.GetSurfaceDescs(), (SIZE_T)NumDescs * sizeof(FSurfaceDescGPU));
            UploadedSurfaceDescs = NumDescs;
        }
        else
        {
            RetainedDeviceCapacity.store(0, std::memory_order_release);
        }
    }

    void FDefaultSceneRenderer::DispatchCullInstances(RHI::FCmdListH CL, const FFrameData& Frame, FShaderH CullInstancesShader)
    {
        LUMINA_PROFILE_SECTION_COLORED("Cull Instances", tracy::Color::Magenta2);
        SCENE_GPU_SCOPE(CL, "Cull Instances");

        const uint8  Slot          = CurrentFrameSlot;
        const uint32 RetainedSlots = Frame.Geometry.RetainedUpload.SlotCount;
        const uint32 NumBatches    = Math::Max(Frame.Views.NumDrawsPerView, 1u);
        const uint32 NumCullViews  = (uint32)Frame.Views.CullViews.Num();

        struct FCullInstancesPC
        {
            uint32 NumViews;
            uint32 NumBatches;
            uint32 bUseLODs;
            uint32 SkinFrameTag;
            RHI::TGPUSpan<FInstanceCullEntry> RetainedCullEntries;
            RHI::TGPUSpan<FTransform3x4>      RetainedTransforms;
            RHI::TGPUSpan<FInstanceStatic>    RetainedStatic;
            RHI::TGPUSpan<FSurfaceDescGPU>    SurfaceDescs;
            RHI::TGPUSpan<FGPUInstance>       OutInstances;
            RHI::TGPUSpan<uint32>             OutInstanceCount;
            RHI::TGPUSpan<FUIntVector2>       OutInstanceViewRanges;
            RHI::TGPUSpan<FRenderBucketGPU>   OutBuckets;
            RHI::TGPUSpan<uint32>             OutOverflowFlag;
            RHI::TGPUSpan<FSkinnedFrameData>  SkinnedFrameData;
            RHI::TGPUSpan<FPreSkinnedVertex>  PreSkinArena;
            RHI::TGPUSpan<uint32>             OutPreSkinCursor;
            RHI::TGPUSpan<FInstanceBlockBounds> BlockBounds;
            RHI::TGPUSpan<uint32>             OutProxyList;
            RHI::TGPUSpan<uint32>             OutProxyDrawArgs;
        };
        static_assert(sizeof(FCullInstancesPC) == 256, "FCullInstancesPC must match CullInstances.slang.");

        FCullInstancesPC PC = {};
        PC.NumViews               = NumCullViews;
        PC.NumBatches             = NumBatches;
        PC.bUseLODs               = FrameSettings.bUseLODs ? 1u : 0u;
        PC.SkinFrameTag           = CurrentSkinnedFrameTag;
        PC.RetainedCullEntries    = { RetainedCullEntryBuffer, RetainedSlots };
        PC.RetainedTransforms     = { RetainedTransformBuffer, RetainedSlots };
        PC.RetainedStatic         = { RetainedStaticBuffer, RetainedSlots };
        PC.SurfaceDescs           = { SurfaceDescBuffer, UploadedSurfaceDescs };
        PC.OutInstances           = { VisibleInstanceRing[Slot], FrameVisibleInstanceCapacity };
        PC.OutInstanceCount       = RHI::TGPUSpan<uint32>::FromAddress(GetCullCounters().Address, 1u);
        PC.OutInstanceViewRanges  = { GetInstanceViewRanges() };
        PC.OutBuckets             = { GetRenderBuckets(), NumCullViews * NumBatches };
        PC.OutOverflowFlag        = RHI::TGPUSpan<uint32>::FromAddress(GetCullCounters().Address + sizeof(uint32), 1u);
        PC.SkinnedFrameData       = { SkinnedFrameDataBuffer };
        PC.PreSkinArena           = { GetPreSkinnedVerticesBuffer(), PreSkinnedVertexCapacity };
        PC.OutPreSkinCursor       = RHI::TGPUSpan<uint32>::FromAddress(GetCullCounters().Address + sizeof(uint32) * 2, 1u);
        // An empty span turns the block skip off, which is the safe answer whenever the bounds are not current.
        PC.BlockBounds            = bInstanceBlockBoundsValid && SceneRendererCullInternal::CVarInstanceBlockCull.GetValue()
                                  ? RHI::TGPUSpan<FInstanceBlockBounds>{ InstanceBlockBoundsBuffer, (uint32)InstanceBlockBounds.Num() }
                                  : RHI::TGPUSpan<FInstanceBlockBounds>{};

        // An empty list turns the proxy path off in the shader, which is also what a list that failed to grow gets.
        const bool bProxies = ProxyListRing[Slot] && ProxyDrawArgsRing[Slot] && ProxyListCapacity > 0u && RetainedProxyBuffer;
        PC.OutProxyList           = bProxies ? RHI::TGPUSpan<uint32>{ ProxyListRing[Slot], ProxyListCapacity } : RHI::TGPUSpan<uint32>{};
        PC.OutProxyDrawArgs       = bProxies ? RHI::TGPUSpan<uint32>{ ProxyDrawArgsRing[Slot], 4u } : RHI::TGPUSpan<uint32>{};
        // A two-phase camera draws the second half, which its late occlusion cull fills, so every reader indexes the list that was drawn.
        SceneRootShared.ProxyList = bProxies ? RHI::TGPUSpan<uint32>::Slice(ProxyListRing[Slot], IsTwoPhaseView(Frame, 0u) ? ProxyListCapacity : 0u, ProxyListCapacity)
                                             : RHI::TGPUSpan<uint32>{};
        if (bProxies)
        {
            // Six corners per proxy, and each cull counts its list into the second word.
            const RHI::FDrawIndirectArguments ProxyArgs[2] = { { 6u, 0u, 0u, 0u }, { 6u, 0u, 0u, 0u } };
            WriteBuffer(CL, ProxyDrawArgsRing[Slot].Gpu, ProxyArgs, sizeof(ProxyArgs));
            Barriers::TransferToCompute(CL);
        }

        // Blades are appended into the retained block between its upload and the cull that reads it, so grass is just more instances downstream.
        GrassScatterPass(CL);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullInstancesShader));
        // CullInstances.slang undoes the fold with GroupID.y * MAX_DISPATCH_AXIS * LOCAL_SIZE_X.
        const FUIntVector2 Grid = RenderUtils::FoldGroupCount(RenderUtils::GetGroupCount(RetainedSlots, 64u));
        RHI::CmdDispatch(CL, MakeArgs(PC), Grid.x, Grid.y, 1u);

        Barriers::ComputeToGeometry(CL);
    }

    void FDefaultSceneRenderer::DispatchDrawPrefix(RHI::FCmdListH CL, const FFrameData& Frame, FShaderH DrawPrefixShader)
    {
        LUMINA_PROFILE_SECTION_COLORED("Build Draw Prefix", tracy::Color::Magenta3);
        SCENE_GPU_SCOPE(CL, "Build Draw Prefix");

        struct FBuildDrawPrefixPC
        {
            uint32 NumViews;
            uint32 NumDraws;
            RHI::TGPUSpan<FUIntVector2>      DrawList;
            RHI::TGPUSpan<FUIntVector2>      BlockList;
            RHI::TGPUSpan<FPreSkinnedVertex> PreSkinArena;
            RHI::TGPUSpan<FGPUInstance>      VisibleInstances;
            RHI::TGPUSpan<FRenderBucketGPU>  Buckets;
            RHI::TGPUSpan<uint32>            InstanceCount;
            RHI::TGPUSpan<uint32>            OutTotals;
            RHI::TGPUSpan<RHI::FDispatchIndirectArguments> OutBlockDispatchArgs;
        };
        static_assert(sizeof(FBuildDrawPrefixPC) == 136, "FBuildDrawPrefixPC must match BuildDrawPrefix.slang.");

        FBuildDrawPrefixPC PC = {};
        PC.NumViews             = Math::Max((uint32)Frame.Views.CullViews.Num(), 1u);
        PC.NumDraws             = Math::Max(Frame.Views.NumDrawsPerView, 1u);
        PC.DrawList             = { GetMeshletDrawList(), DrawListCapacity };
        PC.BlockList            = { GetMeshletBlocks(), BlockListCapacity };
        PC.PreSkinArena         = { GetPreSkinnedVerticesBuffer(), PreSkinnedVertexCapacity };
        PC.VisibleInstances     = { VisibleInstanceRing[CurrentFrameSlot], FrameVisibleInstanceCapacity };
        PC.Buckets              = { GetRenderBuckets() };
        PC.InstanceCount        = RHI::TGPUSpan<uint32>::FromAddress(GetCullCounters().Address, 4u);
        PC.OutTotals            = { GetTotals(), kTotalsSlots };
        PC.OutBlockDispatchArgs = { GetBlockDispatchArgs() };

        DispatchCompute(CL, DrawPrefixShader, PC, 1u, 1u, 1u);

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::MeshShader | RHI::EStageFlags::VertexShader |
            RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite | RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);
    }

    void FDefaultSceneRenderer::DispatchMeshletBlockBuild(RHI::FCmdListH CL, bool bLatePass, bool bCascadePhase)
    {
        static const FShaderH BlocksShader = FShaderLibrary::Get("BuildMeshletBlocks.slang");
        const uint32 NumCullViews = RenderFrame != nullptr ? (uint32)RenderFrame->Views.CullViews.Num() : 0u;
        if (!BlocksShader || NumCullViews == 0u || !bCullBucketsValid)
        {
            return;
        }

        struct FBuildMeshletBlocksPC
        {
            uint32 NumViews;
            uint32 NumBatches;
            RHI::TGPUSpan<uint32>           InstanceCount;
            RHI::TGPUSpan<FGPUInstance>     VisibleInstances;
            RHI::TGPUSpan<FUIntVector2>     InstanceViewRanges;
            RHI::TGPUSpan<FRenderBucketGPU> Buckets;
            RHI::TGPUSpan<FUIntVector2>     OutBlockList;
            RHI::TGPUSpan<FSurfaceDescGPU>  SurfaceDescs;
            uint32 BuildMode;
            uint32 VisibilityTag;
            RHI::TGPUSpan<uint32>           PrevVisibility;
            uint32 bDeferCascades;
            uint32 ReceiverMaskDim;
            int32  SunShadowDataIndex;
            uint32 _Pad0;
            RHI::TGPUSpan<uint32>           ReceiverMask;
        } BPC = {};
        static_assert(sizeof(FBuildMeshletBlocksPC) == 160, "FBuildMeshletBlocksPC must match BuildMeshletBlocks.slang.");

        BPC.NumViews           = NumCullViews;
        BPC.NumBatches         = Math::Max(RenderFrame->Views.NumDrawsPerView, 1u);
        BPC.InstanceCount      = RHI::TGPUSpan<uint32>::FromAddress(GetCullCounters().Address, 4u);
        BPC.VisibleInstances   = { VisibleInstanceRing[CurrentFrameSlot], FrameVisibleInstanceCapacity };
        BPC.InstanceViewRanges = { GetInstanceViewRanges() };
        BPC.Buckets            = { GetRenderBuckets() };
        BPC.OutBlockList       = { GetMeshletBlocks(), BlockListCapacity };
        BPC.SurfaceDescs       = { SurfaceDescBuffer, UploadedSurfaceDescs };
        // Latched by the early build, so a console change between the two builds cannot split one phase and not the other.
        if (!bLatePass && !bCascadePhase)
        {
            bSplitMeshletPhasesThisFrame = SceneRendererCullInternal::CVarSplitMeshletPhases.GetValue();
        }
        // The cascade build appends to the front like an unsplit early build, since cascades never take two phases.
        BPC.BuildMode          = bCascadePhase ? 2u : (bLatePass ? 1u : (bSplitMeshletPhasesThisFrame ? 0u : 2u));
        BPC.bDeferCascades     = bCascadePhase ? 2u : (bShadowReceiverCullThisFrame ? 1u : 0u);
        if (bCascadePhase && bReceiverMaskValid)
        {
            BPC.ReceiverMaskDim    = kReceiverMaskDim;
            BPC.ReceiverMask       = ReceiverMaskFinal;
            BPC.SunShadowDataIndex = RenderFrame->Lighting.Lights[0].ShadowDataIndex;
        }
        BPC.VisibilityTag      = InstanceVisibilityTag;
        BPC.PrevVisibility     = { GetInstanceVisibilityPrev(), InstanceVisibilityCapacity };

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BlocksShader));
        RHI::CmdDispatchIndirect(CL, MakeArgs(BPC), GetBlockDispatchArgs());

        // The block list has exactly one reader, and it is compute now in MeshletCullPass below.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::MeshletCullPass(RHI::FCmdListH CL, EMeshletSlice Slice, bool bCascadePhase)
    {
        static const FShaderH ArgsShader = FShaderLibrary::Get("BuildMeshletCullArgs.slang");
        static const FShaderH CullShader = FShaderLibrary::Get("MeshletCull.slang");
        if (ArgsShader == nullptr || CullShader == nullptr || RenderFrame == nullptr)
        {
            return;
        }

        const uint32 NumViews = (uint32)RenderFrame->Views.CullViews.Num();
        const uint32 NumDraws = Math::Max(RenderFrame->Views.NumDrawsPerView, 1u);
        if (NumViews == 0u || !bCullBucketsValid || !GetMeshletBlocks())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Meshlet Cull", tracy::Color::Magenta);
        SCENE_GPU_SCOPE(CL, "Meshlet Cull");

        // Instances last frame did not see get their blocks only now, and only if this frame's pyramid leaves them visible.
        if (Slice == EMeshletSlice::Late && bSplitMeshletPhasesThisFrame && !bCascadePhase)
        {
            SCENE_GPU_SCOPE(CL, "Build Late Meshlet Blocks");
            DispatchMeshletBlockBuild(CL, /*bLatePass*/ true);
        }

        // Near cascades get their blocks only now, and only for casters whose shadow reaches something the camera sees.
        if (bCascadePhase)
        {
            SCENE_GPU_SCOPE(CL, "Build Cascade Meshlet Blocks");
            DispatchMeshletBlockBuild(CL, /*bLatePass*/ false, /*bCascadePhase*/ true);
        }

        // Mirror of FMeshletCullDispatch in BuildMeshletCullArgs.slang, an indirect dispatch with the blocks each group takes after it.
        struct FMeshletCullDispatch
        {
            uint32 GroupX;
            uint32 GroupY;
            uint32 GroupZ;
            uint32 BlocksPerGroup;
        };
        static_assert(sizeof(FMeshletCullDispatch) == 16);

        struct FCullArgsPC
        {
            uint32 NumViews;
            uint32 NumDraws;
            uint32 Slice;
            uint32 bPost;
            uint32 MaxMeshGroups;
            uint32 SubDrawsPerSlice;
            uint32 bSplitPhases;
            uint32 bDeferCascades;
            uint32 VertexPathCorners;
            uint32 Pad0;
            uint32 Pad1;
            uint32 Pad2;
            RHI::TGPUSpan<FRenderBucketGPU> Buckets;
            RHI::TGPUSpan<uint32>           Totals;
            RHI::TGPUSpan<FMeshletCullDispatch> OutCullDispatchArgs;
            RHI::TGPUSpan<FMeshletDrawArgs> OutMeshDrawArgs;
        } APC = {};
        static_assert(sizeof(FCullArgsPC) == 112, "FCullArgsPC must match BuildMeshletCullArgs.slang.");

        APC.NumViews                = NumViews;
        APC.NumDraws                = NumDraws;
        APC.Slice                   = (uint32)Slice;
        APC.MaxMeshGroups           = MeshletsPerSubDraw();
        APC.VertexPathCorners       = RHI::SupportsMeshShaders() ? 0u : (uint32)MESHLET_MAX_TRIANGLES * 3u;
        APC.SubDrawsPerSlice        = MeshSubDrawsPerSlice;
        APC.bSplitPhases            = bSplitMeshletPhasesThisFrame ? 1u : 0u;
        APC.bDeferCascades          = bCascadePhase ? 2u : (bShadowReceiverCullThisFrame ? 1u : 0u);
        APC.Buckets             = { GetRenderBuckets() };
        APC.Totals              = { GetTotals(), kTotalsSlots };
        APC.OutCullDispatchArgs = { GetMeshletCullDispatchArgs() };
        APC.OutMeshDrawArgs     = { GetMeshDrawArgs() };

        // Serial prefix, so one group; the post pass below is per-bucket and takes a real grid.
        constexpr uint32 kArgsGroupSize = 64;
        const uint32 PostGroups = RenderUtils::GetGroupCount(NumViews * NumDraws, kArgsGroupSize);

        APC.bPost = 0u;
        DispatchCompute(CL, ArgsShader, APC, 1u, 1u, 1u);
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);

        struct FMeshletCullPC
        {
            uint32 NumViews;
            uint32 NumDraws;
            uint32 Slice;
            uint32 VisibilityTag;
            uint32 bSplitPhases;
            uint32 bDeferCascades;
            uint32 ReceiverMaskDim;
            int32  SunShadowDataIndex;
            RHI::TGPUSpan<FRenderBucketGPU> Buckets;
            RHI::TGPUSpan<FUIntVector2>     BlockList;
            RHI::TGPUSpan<uint32>           PrevVisibility;
            RHI::TGPUSpan<uint32>           OutVisibility;
            RHI::TGPUSpan<uint32>           MeshletVisibility;
            RHI::TGPUSpan<uint32>           ReceiverMask;
            RHI::TGPUSpan<FMeshletCullDispatch> CullDispatch;
            RHI::TGPUSpan<FSurfaceDescGPU>      SurfaceDescs;
        } CPC = {};
        static_assert(sizeof(FMeshletCullPC) == 160, "FMeshletCullPC must match MeshletCull.slang.");

        CPC.NumViews       = NumViews;
        CPC.NumDraws       = NumDraws;
        CPC.Slice          = (uint32)Slice;
        CPC.VisibilityTag  = InstanceVisibilityTag;
        CPC.bSplitPhases   = APC.bSplitPhases;
        CPC.Buckets        = { GetRenderBuckets() };
        CPC.BlockList      = { GetMeshletBlocks() };
        CPC.PrevVisibility = { GetInstanceVisibilityPrev(), InstanceVisibilityCapacity };
        CPC.OutVisibility  = { GetInstanceVisibilityWrite(), InstanceVisibilityCapacity };
        CPC.MeshletVisibility = { GetMeshletVisibility(), SceneRendererCullInternal::CVarCullMeshletVisibility.GetValue() != 0 ? MeshletVisibilityCapacity : 0u };
        CPC.bDeferCascades = APC.bDeferCascades;
        CPC.CullDispatch   = APC.OutCullDispatchArgs;
        CPC.SurfaceDescs   = { SurfaceDescBuffer, UploadedSurfaceDescs };
        if (bCascadePhase && bReceiverMaskValid)
        {
            CPC.ReceiverMaskDim    = kReceiverMaskDim;
            CPC.ReceiverMask       = ReceiverMaskFinal;
            CPC.SunShadowDataIndex = RenderFrame->Lighting.Lights[0].ShadowDataIndex;
        }

        DispatchComputeIndirect(CL, CullShader, CPC, GetMeshletCullDispatchArgs());
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::MeshShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::IndirectArguments,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);

        // Turn what it appended into the slice every draw indexes, and the counts they draw from.
        APC.bPost = 1u;
        DispatchCompute(CL, ArgsShader, APC, PostGroups, 1u, 1u);

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::MeshShader | RHI::EStageFlags::VertexShader |
            RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);
    }

    void FDefaultSceneRenderer::DrawMeshletBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch,
                                               const FMeshletPassContext& Ctx)
    {
        if (MeshSubDrawsPerSlice == 0u || !bCullBucketsValid)
        {
            return;
        }

        const uint32 NumDrawsPerView = RenderFrame->Views.NumDrawsPerView;
        const uint32 ArgIndex = Ctx.CullViewIndex * NumDrawsPerView + Batch.IndirectDrawOffset;
        const uint32 Slice    = (uint32)Ctx.Slice;

        struct FMeshletPassPush
        {
            RHI::TGPUSpan<FRenderBucketGPU> Buckets;
            uint32 ArgBase;
            uint32 Slice;
            uint32 MaxMeshGroups;
            uint32 CullViewIndex;
            int32  ShadowDataIndex;
            int32  ViewIndex;
            float  ViewportW;
            float  ViewportH;
            RHI::TGPUSpan<FShadowRasterViewGPU> ShadowViews;
            uint32 ArgViewStride;
            uint32 Pad0;
        } Push;
        static_assert(sizeof(FMeshletPassPush) == 72, "FMeshletPassPush must match FMeshletPassArgs in MeshletGeometry.slang.");

        Push.Buckets              = { GetRenderBuckets() };
        Push.ArgBase              = ArgIndex;
        Push.Slice                = Slice;
        Push.MaxMeshGroups        = MeshletsPerSubDraw();
        Push.CullViewIndex        = Ctx.CullViewIndex;
        Push.ShadowDataIndex      = Ctx.ShadowDataIndex;
        Push.ViewIndex            = Ctx.ShadowViewIndex;
        Push.ViewportW            = Ctx.ViewportW;
        Push.ViewportH            = Ctx.ViewportH;
        Push.ShadowViews          = Ctx.ShadowViews;
        Push.ArgViewStride        = NumDrawsPerView;
        Push.Pad0                 = 0;

        // One mesh workgroup or one vertex-path instance per surviving meshlet, so the grid is the survivor count.
        const uint32 SliceArgBase = (ArgIndex * kMeshletSliceCount + Slice) * MeshSubDrawsPerSlice;
        const RHI::FGPURange SliceArgs = GetMeshDrawArgs().Skip(SliceArgBase * sizeof(FMeshletDrawArgs));
        const bool bMeshShaders = RHI::SupportsMeshShaders();

        if (Ctx.ShadowViews.Count > 0u)
        {
            // An empty view's slot holds zero groups, so it costs the command processor a read and nothing more.
            ASSERT(MeshSubDrawsPerSlice == 1u && bMeshShaders);
            const uint32 ViewStride = NumDrawsPerView * kMeshletSliceCount * sizeof(FMeshletDrawArgs);
            RHI::CmdDrawMeshTasksIndirect(CL, MakeArgs(Push), SliceArgs, Ctx.ShadowViews.Count, ViewStride);
            return;
        }

        const RHI::FGPURange SubDrawCount = GetRenderBuckets().Skip(ArgIndex * sizeof(FRenderBucketGPU) + offsetof(FRenderBucketGPU, SubDrawCount) + Slice * sizeof(uint32));
        if (bMeshShaders)
        {
            RHI::CmdDrawMeshTasksIndirectCount(CL, MakeArgs(Push), SliceArgs, SubDrawCount, MeshSubDrawsPerSlice, sizeof(FMeshletDrawArgs));
        }
        else
        {
            RHI::CmdDrawIndirectCount(CL, MakeArgs(Push), SliceArgs, SubDrawCount, MeshSubDrawsPerSlice, sizeof(FMeshletDrawArgs));
        }
    }
}

namespace Lumina
{
    // After the last cull pass of the frame, so the survivors every phase appended are in what the CPU reads back.
    void FDefaultSceneRenderer::CopyCullTotalsForReadback(RHI::FCmdListH CL)
    {
        const uint8 Slot = CurrentFrameSlot;
        if (MeshletBoundReadback[Slot].Gpu == 0 || !GetTotals())
        {
            return;
        }
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead);
        RHI::CmdMemcpy(CL, { MeshletBoundReadback[Slot].Gpu, sizeof(uint32) * kTotalsSlots }, { GetTotals().Gpu, sizeof(uint32) * kTotalsSlots });
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite, RHI::EStageFlags::Host, RHI::EAccessFlags::HostRead);
    }
}
