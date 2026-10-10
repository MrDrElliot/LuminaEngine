#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "World/Scene/RenderScene/SceneCullMath.h"

#include <bit>

namespace Lumina
{
    #if !defined(LE_SHIPPING)
    static TConsoleVar<float> CVarCullDebugCapacityScale("r.Cull.DebugCapacityScale", 1.0f,
        "Scales the meshlet draw and block lists below demand, to exercise the coarse-LOD overflow fallback.");
    #endif

    static TConsoleVar<int32> CVarCullMeshletVisibility("r.Cull.MeshletVisibility", 1,
        "0 decides the occlusion phase per instance instead of per meshlet, for an A/B against the meshlet bits.");

    static bool IsDebugCapacityScaled()
    {
        #if !defined(LE_SHIPPING)
        const float Scale = CVarCullDebugCapacityScale.GetValue();
        return Scale > 0.0f && Scale < 1.0f;
        #else
        return false;
        #endif
    }

    // Below 1 only in development, where it forces the overflow path that a real scene rarely reaches.
    static uint32 ApplyDebugCapacityScale(uint32 Wanted)
    {
        #if !defined(LE_SHIPPING)
        if (IsDebugCapacityScaled())
        {
            return Math::Max(1u, (uint32)((float)Wanted * CVarCullDebugCapacityScale.GetValue()));
        }
        #endif
        return Wanted;
    }

    namespace
    {
        TConsoleVar<bool> CVarSplitMeshletPhases(
            "r.SplitMeshletPhases",
            true,
            "Build late-phase meshlet blocks after the pyramid, only for instances it leaves visible, instead of culling every block twice.");

        TConsoleVar<bool> CVarInstanceBlockCull(
            "r.InstanceBlockCull",
            true,
            "Skip whole groups of 64 retained instances the camera is farther from than any of them can draw.");
    }

    namespace
    {
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
        const uint32 NumBlocks = (NumSlots + kInstanceCullBlockSize - 1u) / kInstanceCullBlockSize;
        if (NumBlocks == 0u || Entries == nullptr)
        {
            return;
        }

        ScenePrimitives.GetGPUWrittenSlotRanges(GPUWrittenSlotRanges);

        bBlockBoundsRebuildAll = Upload.bFull || !bInstanceBlockBoundsValid || InstanceBlockBounds.size() != NumBlocks;
        BlockBoundsNumBlocks   = NumBlocks;

        if (InstanceBlockHasDF.size() != NumBlocks)
        {
            InstanceBlockHasDF.assign(NumBlocks, 0u);
            bDistanceFieldBlocksDirty = true;
        }

        DirtyInstanceBlocks.clear();
        if (bBlockBoundsRebuildAll)
        {
            InstanceBlockBounds.resize(NumBlocks);
        }
        else
        {
            // DirtySlots arrives sorted and unique, so its blocks come out sorted and only repeat back to back.
            for (uint32 Slot : Upload.DirtySlots)
            {
                const uint32 Block = Slot / kInstanceCullBlockSize;
                if (Slot < NumSlots && (DirtyInstanceBlocks.empty() || DirtyInstanceBlocks.back() != Block))
                {
                    DirtyInstanceBlocks.push_back(Block);
                }
            }
        }

        const uint32 NumWork = bBlockBoundsRebuildAll ? NumBlocks : (uint32)DirtyInstanceBlocks.size();
        bBlockBoundsLaunched = true;
        if (NumWork == 0u)
        {
            return;
        }

        constexpr uint32 BlocksPerTask = 256;
        const uint32 NumTasks = (NumWork + BlocksPerTask - 1u) / BlocksPerTask;
        const bool bAll = bBlockBoundsRebuildAll;
        InstanceBlockBoundsTask = Task::AsyncTask(NumTasks, 1, [this, Entries, NumSlots, NumWork, bAll](uint32 Start, uint32 End, uint32)
        {
            for (uint32 TaskIndex = Start; TaskIndex < End; ++TaskIndex)
            {
                const uint32 WorkEnd = Math::Min((TaskIndex + 1u) * BlocksPerTask, NumWork);
                for (uint32 Work = TaskIndex * BlocksPerTask; Work < WorkEnd; ++Work)
                {
                    const uint32 Block = bAll ? Work : DirtyInstanceBlocks[Work];
                    const uint32 First = Block * kInstanceCullBlockSize;
                    const uint32 Last  = Math::Min(First + kInstanceCullBlockSize, NumSlots);
                    InstanceBlockBounds[Block] = SceneCull::ComputeInstanceBlockBounds(Entries, First, Last, OverlapsAny(GPUWrittenSlotRanges, First, Last));

                    const uint8 bHasDF = SceneCull::BlockHasDistanceFieldCaster(Entries, First, Last) ? 1u : 0u;
                    if (InstanceBlockHasDF[Block] != bHasDF)
                    {
                        InstanceBlockHasDF[Block] = bHasDF;
                        bBlockDFChanged.store(true, std::memory_order_relaxed);
                    }
                }
            }
        }, ETaskPriority::Medium);
    }

    void FDefaultSceneRenderer::JoinInstanceBlockBounds()
    {
        if (InstanceBlockBoundsTask)
        {
            LUMINA_PROFILE_SECTION("Wait Instance Block Bounds");
            InstanceBlockBoundsTask->Wait();
            InstanceBlockBoundsTask = nullptr;
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
            StageWrite(InstanceBlockBoundsBuffer.Gpu, InstanceBlockBounds.data(), (uint64)NumBlocks * sizeof(FInstanceBlockBounds),
                       /*bFillBeforeSubmit*/ true);
        }
        else
        {
            // Consecutive dirty blocks go up as one write.
            for (SIZE_T i = 0; i < DirtyInstanceBlocks.size();)
            {
                const uint32 RunStart = DirtyInstanceBlocks[i];
                uint32 RunEnd = RunStart;
                while (i < DirtyInstanceBlocks.size() && DirtyInstanceBlocks[i] == RunEnd)
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
            DistanceFieldBlocks.clear();
            for (uint32 Block = 0; Block < NumBlocks; ++Block)
            {
                if (InstanceBlockHasDF[Block] != 0u)
                {
                    DistanceFieldBlocks.push_back(Block);
                }
            }
            if (!DistanceFieldBlocks.empty())
            {
                const uint64 Bytes = (uint64)DistanceFieldBlocks.size() * sizeof(uint32);
                ReserveBuffer(CL, SunDFBlockListBuffer, Bytes, /*bAllowShrink*/ true, /*bPreserveContents*/ false);
                if (SunDFBlockListBuffer.Size >= Bytes)
                {
                    StageWrite(SunDFBlockListBuffer.Gpu, DistanceFieldBlocks.data(), Bytes);
                }
            }
            bDistanceFieldBlocksDirty = false;
        }

        FlushStagedWrites(CL);
        bInstanceBlockBoundsValid = true;
    }

    void FDefaultSceneRenderer::UploadBoneArena(RHI::FCmdListH CL, const FFrameData& Frame)
    {
        const TVector<FBoneTransform>& Mirror = Frame.Geometry.BonesData;

        const uint32 ArenaCount = Math::Min(Frame.Geometry.BoneCount, (uint32)Mirror.size());
        if (ArenaCount == 0)
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Upload Bone Arena");

        // Sized by residency, so it tracks the meshes the cull keeps rather than every skeleton alive.
        const SIZE_T ArenaBytes = (SIZE_T)ArenaCount * sizeof(FBoneTransform);

        // Every copy below is a deferred fill, since it is PCIe-bound and the frame can record while it runs.
        const RHI::GPUPtr PrevArena = BoneArenaBuffer.Gpu;
        ReserveBuffer(CL, BoneArenaBuffer, ArenaBytes);
        if (!BoneArenaBuffer)
        {
            return;
        }

        // A reallocation drops every resident pose, so the dirty set no longer describes the buffer.
        if (BoneArenaBuffer.Gpu != PrevArena)
        {
            StageWrite(BoneArenaBuffer.Gpu, Mirror.data(), ArenaBytes, /*bFillBeforeSubmit*/ true);
            FlushStagedWrites(CL);
            return;
        }

        TVector<FUIntVector2>& Ranges = BoneUploadScratch;
        Ranges.assign(Frame.Geometry.BoneUploadRanges.begin(), Frame.Geometry.BoneUploadRanges.end());
        if (Ranges.empty())
        {
            return;   // every pose the gather kept is already resident
        }

        Algo::Sort(Ranges,
                    [](const FUIntVector2& A, const FUIntVector2& B) { return A.x < B.x; });

        // Merging across a small gap re-sends a slice that was already resident, which beats a second copy.
        constexpr uint32 kBoneMergeGap = 1024;

        const auto Flush = [&](uint32 Start, uint32 End)
        {
            End = Math::Min(End, ArenaCount);
            if (Start >= End)
            {
                return;
            }

            StageWrite(BoneArenaBuffer.Gpu + (uint64)Start * sizeof(FBoneTransform), Mirror.data() + Start,
                       (uint64)(End - Start) * sizeof(FBoneTransform), /*bFillBeforeSubmit*/ true);
        };

        uint32 RunStart = Ranges[0].x;
        uint32 RunEnd   = Ranges[0].x + Ranges[0].y;

        for (SIZE_T i = 1; i < Ranges.size(); ++i)
        {
            const uint32 Start = Ranges[i].x;
            const uint32 End   = Ranges[i].x + Ranges[i].y;

            if (Start <= RunEnd + kBoneMergeGap)
            {
                RunEnd = Math::Max(RunEnd, End);
                continue;
            }

            Flush(RunStart, RunEnd);
            RunStart = Start;
            RunEnd   = End;
        }
        Flush(RunStart, RunEnd);
        FlushStagedWrites(CL);
    }

    // Vulkan guarantees at least this on maxComputeWorkGroupCount[1], and the bounds dispatch uses y per slot.
    static constexpr uint32 kMaxSkinnedBoundsDispatchY = 65535u;

    void FDefaultSceneRenderer::UploadSkinnedFrameData(RHI::FCmdListH CL, FFrameData& Frame)
    {
        TVector<FSkinnedFrameData>& Data  = Frame.Geometry.SkinnedFrameData;
        const TVector<uint32>&      Slots = Frame.Geometry.SkinnedSlots;

        // Minted here rather than reused from MeshletDrawTag, which wraps at 4095 and can alias.
        CurrentSkinnedFrameTag++;

        if (Data.empty() || Slots.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Upload Skinned Frame Data");

        ReserveBuffer(CL, SkinnedFrameDataBuffer, Data.size() * sizeof(FSkinnedFrameData));
        ReserveBuffer(CL, SkinnedSlotListBuffer, Slots.size() * sizeof(uint32));
        if (!SkinnedFrameDataBuffer || !SkinnedSlotListBuffer)
        {
            return;
        }

        WriteBuffer(CL, SkinnedSlotListBuffer.Gpu, Slots.data(), Slots.size() * sizeof(uint32));

        //~ Bounds arena layout, assigned before the upload below carries the bases to the GPU.
        SkinnedBoundsMaxRange = 0;
        uint64 BoundsTotal    = 0;

        for (uint32 Slot : Slots)
        {
            if (Slot >= (uint32)Data.size())
            {
                continue;
            }

            const uint32 Len = Data[Slot].SurfaceMeshletCount;

            SkinnedBoundsMaxRange = Math::Max(SkinnedBoundsMaxRange, Len);
            BoundsTotal += Len;
        }

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
            if (Slot >= (uint32)Data.size())
            {
                continue;
            }

            // Position in this list is the bounds dispatch's y index, which cannot exceed the grid limit.
            const bool bDispatchable = DispatchIndex < kMaxSkinnedBoundsDispatchY;
            DispatchIndex++;

            FSkinnedFrameData& D = Data[Slot];
            D.FrameTag = CurrentSkinnedFrameTag;

            const uint32 Begin = D.SurfaceMeshletOffset;
            const uint32 Len   = D.SurfaceMeshletCount;

            // All or nothing, because a partly written slice leaves the cull rejecting against stale data.
            if (Len == 0u || !bDispatchable || (uint64)BoundsCursor + Len > SkinnedMeshletBoundsCapacity)
            {
                D.SkinnedBoundsBase = kNoSkinnedBounds;
                continue;
            }

            // Folded so the shader indexes by a mesh-global meshlet index, same trick as the pre-skin slice.
            D.SkinnedBoundsBase = BoundsCursor - Begin;
            BoundsCursor += Len;
        }

        // A merged gap is still staged and copied, so this trades one copy region against ~1 KiB of bytes.
        constexpr uint32 kSkinnedMergeGap = 24;

        const uint32 Count = (uint32)Data.size();

        // Only the gathered slots, coalesced, and a bitmap over the slot range orders them in linear time where a sort did not.
        TVector<uint64>& SlotBits = SkinnedSlotBits;
        SlotBits.assign(((SIZE_T)Count + 63u) / 64u, 0ull);
        for (uint32 Slot : Slots)
        {
            if (Slot < Count)
            {
                SlotBits[Slot >> 6] |= 1ull << (Slot & 63u);
            }
        }

        TVector<FUIntVector2>& Runs = SkinnedRunScratch;
        Runs.clear();

        const auto AddRun = [&](uint32 Start, uint32 End)
        {
            End = Math::Min(End, Count);
            if (Start < End)
            {
                Runs.push_back(FUIntVector2(Start, End - Start));
            }
        };

        bool   bHaveRun = false;
        uint32 RunStart = 0;
        uint32 RunEnd   = 0;
        for (SIZE_T Word = 0; Word < SlotBits.size(); ++Word)
        {
            uint64 Bits = SlotBits[Word];
            while (Bits != 0)
            {
                const uint32 Slot = (uint32)(Word * 64u) + (uint32)std::countr_zero(Bits);
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

        WriteBufferRuns(CL, SkinnedFrameDataBuffer.Gpu, Data.data(), sizeof(FSkinnedFrameData), Runs, /*bFillBeforeSubmit*/ true);

        uint64 StagedSlots = 0;
        for (const FUIntVector2& Run : Runs)
        {
            StagedSlots += Run.y;
        }
        LUMINA_PROFILE_VALUE("Skinning/FrameDataRegions", (int64)Runs.size());
        LUMINA_PROFILE_VALUE("Skinning/FrameDataKiB", (int64)(StagedSlots * sizeof(FSkinnedFrameData) / 1024));
    }

    void FDefaultSceneRenderer::SkinnedMeshletBoundsPass(RHI::FCmdListH CL, const FFrameData& Frame)
    {
        const uint32 NumSkinned = (uint32)Frame.Geometry.SkinnedSlots.size();
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
        const uint32 GroupsX = (SkinnedBoundsMaxRange + kBoundsGroupSize - 1u) / kBoundsGroupSize;

        // Slots past the grid limit were given kNoSkinnedBounds above, so dropping them here loses nothing.
        const uint32 GroupsY = Math::Min(NumSkinned, kMaxSkinnedBoundsDispatchY);

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
        auto& SceneGlobalData            = Frame.SceneGlobalData;
        const auto& CullViews            = Frame.Views.CullViews;
        auto& LightData                  = Frame.Lighting.LightData;
        const auto& EnvironmentParams    = Frame.Volumetrics.EnvironmentParams;
        const int32 EnvironmentMapID     = Frame.Volumetrics.EnvironmentMapID;
        const auto& BillboardInstances   = Frame.Primitives.BillboardInstances;
        const uint32 NumDrawsPerView     = Frame.Views.NumDrawsPerView;
        bool& bIBLDirty                  = Frame.Volumetrics.bIBLDirty;
        bool& bIBLConvolutionDirty       = Frame.Volumetrics.bIBLConvolutionDirty;

        const uint32 NumCullViews                  = (uint32)CullViews.size();
        const uint32 NumDraws                      = NumDrawsPerView;
        SceneGlobalData.CullData.MeshletDrawTag    = (MeshletDrawTagCounter++ % 4095u) + 1u;

        // Only the GPU knows the demand; an undersized frame drops blocks to inline skinning and corrects.
        const uint32 PreSkinBudget        = GetMaxPreSkinnedVertices();
        const uint32 PreSkinWanted        = Math::Min(PreSkinDemand.Observe(LastPreSkinRequested), PreSkinBudget);
        const SIZE_T PreSkinnedSize       = Math::Max<SIZE_T>(sizeof(FPreSkinnedVertex),
                                            (SIZE_T)Math::Max<uint32>(PreSkinWanted, 1u) * sizeof(FPreSkinnedVertex));

        UpdateMeshletBoundFeedback(CurrentFrameSlot);

        // A cut lands somewhere the lagging demand has never seen, so the lists hold the session peak until it catches up.
        constexpr float CameraCutDistance = 10.0f;
        if (!CullViews.empty())
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

        DrawListPeak  = Math::Max(DrawListPeak, LastDrawListRequired);
        BlockListPeak = Math::Max(BlockListPeak, LastBlocksRequested);

        // Windowed like the other GPU-fed sizes, so the raw readback's lag does not collapse it.
        // The readback lags the frame by the ring depth, so a moving camera outgrows an exact fit before it is seen.
        auto WithHeadroom = [](uint32 Demand) { return (uint32)Math::Min<uint64>((uint64)Demand + Demand / 4u, 0xFFFFFFFFull); };
        const uint32 DrawListWindowed = Math::Max(WithHeadroom(DrawListDemand.Observe(LastDrawListRequired)), 65536u);
        const uint32 DrawListWanted   = ApplyDebugCapacityScale(bAfterCameraCut ? Math::Max(DrawListWindowed, DrawListPeak) : DrawListWindowed);
        const SIZE_T MeshletDrawListSize = Math::Max<SIZE_T>(
            sizeof(uint32) * 2,
            (SIZE_T)DrawListWanted * sizeof(uint32) * 2);

        // Must mirror MeshletCullPass' bucket count exactly, which clamps both operands to 1.
        const SIZE_T NumArgSlots = (SIZE_T)Math::Max(NumCullViews, 1u) * (SIZE_T)Math::Max(NumDraws, 1u);
        
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
        {
            const uint8 Slot = CurrentFrameSlot;
            ReserveBuffer(CL, MeshletDrawListRing[Slot], MeshletDrawListSize);
            DrawListCapacity = MeshletDrawListRing[Slot].CapacityOf<FUIntVector2>();
            if (IsDebugCapacityScaled())
            {
                DrawListCapacity = Math::Min(DrawListCapacity, DrawListWanted);
            }

            // Read after the resize, because last frame's value describes the other ring slot's buffer.
            const uint32 MaxGroups = Math::Max(RHI::GetMaxMeshWorkGroupCount(), 1u);
            const uint32 WantedSubDraws = (DrawListCapacity == 0u) ? 1u : (((DrawListCapacity - 1u) / MaxGroups) + 1u);
            const SIZE_T NumBucketSlices = NumArgSlots * (SIZE_T)kMeshletSliceCount;
            // One spare view of slots, since a multi-view shadow draw's strided range runs a whole view past its last read.
            const SIZE_T SpareViewSlots = (SIZE_T)Math::Max(NumDraws, 1u) * (SIZE_T)kMeshletSliceCount * (SIZE_T)WantedSubDraws;
            const SIZE_T MeshDrawArgsSize = Math::Max<SIZE_T>(
                sizeof(RHI::FDrawMeshTasksIndirectArguments),
                (NumBucketSlices * (SIZE_T)WantedSubDraws + SpareViewSlots)
                    * sizeof(RHI::FDrawMeshTasksIndirectArguments));

            ReserveBuffer(CL, MeshDrawArgsRing[Slot], MeshDrawArgsSize);
            const uint32 AllocatedArgSlots = MeshDrawArgsRing[Slot].CapacityOf<RHI::FDrawMeshTasksIndirectArguments>();
            MeshSubDrawsPerSlice = (uint32)Math::Min<SIZE_T>(WantedSubDraws, AllocatedArgSlots / NumBucketSlices);

            if (MeshSubDrawsPerSlice > LoggedSubDrawPeak)
            {
                LoggedSubDrawPeak = MeshSubDrawsPerSlice;
                LOG_INFO("Meshlet sub-draws per slice: {} (draw-list capacity {}, mesh workgroups per draw {}).",
                         MeshSubDrawsPerSlice, DrawListCapacity, MaxGroups);
            }
            
            // Windowed peak, not the last readback: that count lags kFramesInFlight and collapses the
            // allocation the moment the camera looks at something empty.
            const uint32 VisibleDemand   = VisibleInstanceDemand.Observe(LastVisibleInstances);

            // A level load or mass spawn outruns the lagging demand, so the growth since that readback is reserved too.
            const uint32 SlotCount       = Frame.Geometry.RetainedUpload.SlotCount;
            const uint32 SlotCountBefore = RecentSlotCounts[RecentSlotCursor];
            RecentSlotCounts[RecentSlotCursor] = SlotCount;
            RecentSlotCursor = (RecentSlotCursor + 1u) % (uint32)std::size(RecentSlotCounts);
            const uint32 SlotGrowth = SlotCount > SlotCountBefore ? SlotCount - SlotCountBefore : 0u;

            uint32 VisibleCapacityWanted = Math::Max(VisibleDemand, 4096u) + SlotGrowth;

            const uint32 VisibleCapacityMax = Math::Max(SlotCount, 1u);

            // Every retained slot fits under this, so an instance can never be refused a visible slot.
            constexpr uint64 ExactVisibleBoundBytes = 64ull << 20;
            if (LastVisibleInstances == 0u || (uint64)VisibleCapacityMax * sizeof(FGPUInstance) <= ExactVisibleBoundBytes)
            {
                VisibleCapacityWanted = VisibleCapacityMax;
            }

            VisibleCapacityWanted = Math::Min(VisibleCapacityWanted, VisibleCapacityMax);

            ReserveBuffer(CL, VisibleInstanceRing[Slot], (SIZE_T)VisibleCapacityWanted * sizeof(FGPUInstance));

            // From the allocation rather than the request, so a grow that failed cannot hand the cull
            // room it does not have, and the slack the grow already paid for is not thrown away.
            FrameVisibleInstanceCapacity = (uint32)Math::Min<uint64>(
                VisibleInstanceRing[Slot].Size / sizeof(FGPUInstance), VisibleCapacityMax);

            const SIZE_T InstanceViewRangeSize = Math::Max<SIZE_T>(
                sizeof(uint32) * 2,
                (SIZE_T)FrameVisibleInstanceCapacity * (SIZE_T)Math::Max(NumCullViews, 1u) * sizeof(uint32) * 2);
            
            ReserveBuffer(CL, InstanceViewRangeRing[Slot], InstanceViewRangeSize);

            // Only the GPU knows how many blocks were appended, and its counter lags the frames in flight.
            const uint32 BlockListWindowed = WithHeadroom(BlockListDemand.Observe(LastBlocksRequested));
            const uint32 BlockListWanted   = ApplyDebugCapacityScale(bAfterCameraCut ? Math::Max(BlockListWindowed, BlockListPeak) : BlockListWindowed);

            const SIZE_T MeshletBlockSize = Math::Max<SIZE_T>(
                sizeof(uint32) * 2,
                (SIZE_T)Math::Max<uint32>(BlockListWanted, 1u) * sizeof(uint32) * 2);
            ReserveBuffer(CL, MeshletBlockRing[Slot], MeshletBlockSize);
            BlockListCapacity = MeshletBlockRing[Slot].CapacityOf<FUIntVector2>();
            if (IsDebugCapacityScaled())
            {
                BlockListCapacity = Math::Min(BlockListCapacity, BlockListWanted);
            }

            // Requirement is from kFramesInFlight ago, capacity is from now, so print both sides.
            const auto LogOverflow = [](const char* What, uint32 Needed, uint32 GrownTo)
            {
                LOG_WARN("RenderScene: {} overflowed -- {} needed, capacity now {}. Instances that did not fit "
                         "drew at their coarsest LOD that frame, or not at all without one; this clears once the larger allocation cycles in.",
                         What, Needed, GrownTo);
            };

            // Demand over the cap, not the readback's lag, since only the latter corrects itself.
            const bool bPreSkinCapped = LastPreSkinOverflowed && LastPreSkinRequested > GetMaxPreSkinnedVertices();

            static uint32 OverflowLogCounter = 0;
            if (LastVisibleOverflowed || LastDrawListOverflowed || LastBlocksOverflowed || bPreSkinCapped)
            {
                if ((OverflowLogCounter++ % 60u) == 0u)
                {
                    if (LastVisibleOverflowed)  { LogOverflow("visible-instance buffer", LastVisibleInstances, FrameVisibleInstanceCapacity); }
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
            }
        }

        SceneGlobalData.CullData.MeshletDrawListCapacity = DrawListCapacity;
        SceneGlobalData.CullData.InstanceNum             = FrameVisibleInstanceCapacity;

        UploadBoneArena(CL, Frame);
        UploadSkinnedFrameData(CL, Frame);
        // The arena's size, not a per-frame total, since it bounds an INDEX a stable base points into.
        SceneGlobalData.CullData.BoneNum                 = Frame.Geometry.BoneCount;

        {
            LUMINA_PROFILE_SECTION_COLORED("Write Scene Buffers", tracy::Color::OrangeRed3);

            const bool bEnvParamsChanged = !bEnvironmentParamsUploaded || std::memcmp(&EnvironmentParams, &LastUploadedEnvironmentParams, sizeof(FEnvironmentParams)) != 0;
            if (bEnvParamsChanged)
            {
                LastUploadedEnvironmentParams = EnvironmentParams;
                bEnvironmentParamsUploaded   = true;
            }

            // A day cycle moves the sun every frame, and a sub-quarter-degree step is invisible in reflections but costs a whole cube bake.
            constexpr float SkyCubeSunCosThreshold = 0.99999f;
            const bool bSunChanged = bLastIBLHasSun != (LightData.bHasSun != 0)
                || Math::Dot(LastIBLSunDirection, LightData.SkySunDirection) < SkyCubeSunCosThreshold;
            const bool bMapChanged = LastIBLEnvironmentMapID != EnvironmentMapID;

            const bool bResChanged = Frame.Volumetrics.IBLResolution != LastExtractedIBLResolution;
            LastExtractedIBLResolution = Frame.Volumetrics.IBLResolution;

            if (FrameFlags.bHasEnvironment &&
                (!bIBLValid || bEnvParamsChanged || bSunChanged || bMapChanged || bResChanged))
            {
                bIBLDirty                  = true;
                LastIBLEnvironmentParams   = EnvironmentParams;
                LastIBLEnvironmentMapID    = EnvironmentMapID;
                LastIBLSunDirection        = LightData.SkySunDirection;
                bLastIBLHasSun             = (LightData.bHasSun != 0);
                bIBLValid                  = true;
            }

            constexpr float SunCosThreshold = 0.99996f;
            const bool bConvHasSunChanged = bLastConvolvedHasSun != (LightData.bHasSun != 0);
            float SunCos = 1.0f;
            if (bLastConvolvedHasSun && LightData.bHasSun)
            {
                SunCos = Math::Dot(LastConvolvedSunDirection, LightData.SkySunDirection);
            }
            const bool bConvSunChanged = bConvHasSunChanged || (SunCos < SunCosThreshold);
            const bool bConvParamsChanged = std::memcmp(&LastConvolvedEnvironmentParams, &EnvironmentParams, sizeof(FEnvironmentParams)) != 0;
            const bool bConvMapChanged =
                LastConvolvedEnvironmentMapID != EnvironmentMapID;

            if (FrameFlags.bHasEnvironment &&
                (!bIBLConvolutionValid || bConvParamsChanged || bConvSunChanged || bConvMapChanged || bResChanged))
            {
                bIBLConvolutionDirty           = true;
                LastConvolvedEnvironmentParams = EnvironmentParams;
                LastConvolvedEnvironmentMapID  = EnvironmentMapID;
                LastConvolvedSunDirection      = LightData.SkySunDirection;
                bLastConvolvedHasSun           = (LightData.bHasSun != 0);
                bIBLConvolutionValid           = true;
            }
            
            if (!FrameFlags.bHasEnvironment)
            {
                bIBLValid            = false;
                bIBLConvolutionValid = false;
            }
            SceneRootShared = FSceneRoot{};
            // Only the live prefix of each array reaches the ring; the header carries where they landed.
            LightData.Lights  = RHI::CopyTransientArray(Frame.Lighting.Lights.data(),  NumLiveLights);
            LightData.Shadows = RHI::CopyTransientArray(Frame.Lighting.Shadows.data(), NumLiveShadows);
            const uint32 NumLightFunctions = (uint32)Frame.Lighting.LightFunctionDraws.size();
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
            LightData.LightFunctions     = RHI::CopyTransientArray(Frame.Lighting.LightFunctions.data(), NumLightFunctions);
            LightData.LightFunctionAtlas = LightFunctionAtlas ? (uint32)LightFunctionAtlas.GetResourceID() : 0u;
            SceneBindings.Lights = RHI::CopyTransient(LightData);
            if (VisibleInstanceRing[CurrentFrameSlot])
            {
                SceneBindings.Instances = VisibleInstanceRing[CurrentFrameSlot].Gpu;
            }
            // Persistent and slot-addressed, so this is just the arena's address.
            if (BoneArenaBuffer)
            {
                SceneRootShared.Bones = { BoneArenaBuffer };
            }

            // Last frame's snapshot, so this publishes what SnapshotMotionState left at the end of it.
            if (bPrevMotionStateValid && PrevRetainedTransformBuffer)
            {
                SceneRootShared.PrevRetainedTransforms = { PrevRetainedTransformBuffer };

                if (PrevBoneArenaBuffer)
                {
                    SceneRootShared.PrevBones = { PrevBoneArenaBuffer };
                }
            }
            if (!BillboardInstances.empty())
            {
                SceneRootShared.Billboards = RHI::CopyTransientArray(BillboardInstances.data(), BillboardInstances.size());
            }
            if (!CullViews.empty())
            {
                SceneRootShared.CullViews = RHI::CopyTransientArray(CullViews.data(), CullViews.size());
            }
            if (!Frame.Primitives.WidgetInstances.empty())
            {
                SceneRootShared.Widgets = RHI::CopyTransientArray(Frame.Primitives.WidgetInstances.data(), Frame.Primitives.WidgetInstances.size());
            }
            // Splines are small and bounded, so the shared transient ring is the right home.
            NumActiveSplines       = (uint32)Frame.Splines.Splines.size();
            SplineBufferSpan       = {};
            SplinePointBufferSpan  = {};
            SplineSampleBufferSpan = {};
            if (NumActiveSplines > 0)
            {
                SplineBufferSpan = RHI::CopyTransientArray(Frame.Splines.Splines.data(),
                                                                 Frame.Splines.Splines.size());
                if (!Frame.Splines.Points.empty())
                {
                    SplinePointBufferSpan = RHI::CopyTransientArray(Frame.Splines.Points.data(),
                                                                          Frame.Splines.Points.size());
                }
                if (!Frame.Splines.Samples.empty())
                {
                    SplineSampleBufferSpan = RHI::CopyTransientArray(Frame.Splines.Samples.data(),
                                                                           Frame.Splines.Samples.size());
                }
            }

            NumActiveProbes  = (uint32)Frame.ReflectionProbes.Probes.size();
            ProbeBufferSpan  = {};
            if (NumActiveProbes > 0)
            {
                InitReflectionProbeTargets();
                ProbeBufferSpan = RHI::CopyTransientArray(Frame.ReflectionProbes.Probes.data(),
                                                                Frame.ReflectionProbes.Probes.size());
            }

            SceneRootShared.Materials            = Render().GetMaterialManager().GetMaterialTable();
            SceneRootShared.Collections          = Render().GetCollectionManager().GetSpan();
            SceneRootShared.MeshletDrawList      = { GetMeshletDrawList(), DrawListCapacity };
            SceneRootShared.PreSkinnedVertices   = { GetPreSkinnedVerticesBuffer(), PreSkinnedVertexCapacity };
            SceneRootShared.PreSkinnedPrevPositions = { PreSkinnedPrevPositionsBuffer, PreSkinnedPrevCapacity };
            SceneRootShared.SkinnedFrameData     = { SkinnedFrameDataBuffer };
            // Spheres and cones are indexed by one base, so each carries the same capacity.
            SceneRootShared.SkinnedMeshletBounds = { SkinnedMeshletBoundsBuffer, SkinnedMeshletBoundsCapacity };
            SceneRootShared.SkinnedMeshletCones  = { SkinnedMeshletConeBuffer, SkinnedMeshletBoundsCapacity };
            if (IsGTAOEnabled())
            {
                SceneGlobalData.GTAOSettings.AOTextureIndex = (uint32)CurrentView->Images[(int)ENamedImage::GTAOBlur].GetResourceID();
            }
            if (CurrentView->Images[(int)ENamedImage::SunFarShadowMask].IsValid() && WantsSunFarShadowMask())
            {
                SceneGlobalData.GTAOSettings.SunShadowMaskIndex = (uint32)CurrentView->Images[(int)ENamedImage::SunFarShadowMask].GetResourceID();
            }

            SceneGlobalData.SceneDepthIndex =
                (uint32)CurrentView->Images[(int)ENamedImage::DepthAttachment].GetResourceID();

            const bool bDecals = !Frame.Primitives.DecalExtracts.empty();
            SceneGlobalData.DBufferAIndex = bDecals ? (uint32)CurrentView->Images[(int)ENamedImage::DBufferA].GetResourceID() : Constants::kIndexNoneU32;
            SceneGlobalData.DBufferBIndex = bDecals ? (uint32)CurrentView->Images[(int)ENamedImage::DBufferB].GetResourceID() : Constants::kIndexNoneU32;
            SceneGlobalData.DBufferCIndex = bDecals ? (uint32)CurrentView->Images[(int)ENamedImage::DBufferC].GetResourceID() : Constants::kIndexNoneU32;
            SceneGlobalData.DBufferDIndex = bDecals ? (uint32)CurrentView->Images[(int)ENamedImage::DBufferD].GetResourceID() : Constants::kIndexNoneU32;

            PublishFogGlobals(SceneGlobalData);

            SetSceneRoot(CL, *CurrentView, RHI::CopyTransient(SceneGlobalData));

            DispatchGPUSceneCull(CL, Frame);
        }

        // A frame with no retained slots never reaches the launch in the cull, and its bone fills still belong on the workers.
        LaunchDeferredStageFills();
    }

    void FDefaultSceneRenderer::SkinningPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        // Two blocks per gathered skinned SLOT, the surface LOD and the coarser shadow LOD.
        const uint32 NumSkinned = (uint32)Frame.Geometry.SkinnedSlots.size();
        if (NumSkinned == 0)
        {
            return;
        }

        // The GPU scope is opened by the caller, not here.
        LUMINA_PROFILE_SECTION_COLORED("Skinning Pass", tracy::Color::SkyBlue);

        static const FShaderH WorkShader = FShaderLibrary::Get("BuildSkinWork.slang");
        static const FShaderH SkinShader = FShaderLibrary::Get("Skinning.slang");
        if (!SkinShader || !WorkShader)
        {
            return;
        }

        const uint8 Slot = CurrentFrameSlot;

        ReserveBuffer(CL, SkinWorkBaseRing[Slot], (uint64)NumSkinned * sizeof(uint32));
        if (!SkinWorkBaseRing[Slot] || !GetSkinDispatchArgs() || !GetPreSkinnedVerticesBuffer()
            || !SkinnedSlotListBuffer || !SkinnedFrameDataBuffer || !RetainedStaticBuffer)
        {
            return;
        }

        //~ Lay out the dispatch as one workgroup per instance and meshlet, enumerated on the GPU.
        {
            struct FBuildSkinWorkPC
            {
                RHI::TGPUSpan<uint32>            SlotList;
                RHI::TGPUSpan<FSkinnedFrameData> SkinnedData;
                RHI::TGPUSpan<uint32>            OutWorkBase;
                RHI::TGPUSpan<RHI::FDispatchIndirectArguments> OutDispatchArgs;
            } WPC = {};
            static_assert(sizeof(FBuildSkinWorkPC) == 64, "FBuildSkinWorkPC must match BuildSkinWork.slang.");

            WPC.SlotList        = { SkinnedSlotListBuffer, NumSkinned };
            WPC.SkinnedData     = { SkinnedFrameDataBuffer };
            WPC.OutWorkBase     = { SkinWorkBaseRing[Slot], NumSkinned };
            WPC.OutDispatchArgs = { GetSkinDispatchArgs() };

            DispatchCompute(CL, WorkShader, WPC, 1u, 1u, 1u);

            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);
        }

        struct FSkinningPushConstants
        {
            RHI::TGPUSpan<uint32>             WorkBase;
            RHI::TGPUSpan<uint32>             SlotList;
            RHI::TGPUSpan<FSkinnedFrameData>  SkinnedData;
            RHI::TGPUSpan<FInstanceStatic>    RetainedStatic;
            RHI::TGPUSpan<FPreSkinnedVertex>  OutVertices;
            RHI::TGPUSpan<FPrevSkinnedPosition> OutPrevPositions;
        } PC = {};
        static_assert(sizeof(FSkinningPushConstants) == 96, "FSkinningPushConstants must match Skinning.slang.");

        PC.WorkBase       = { SkinWorkBaseRing[Slot], NumSkinned };
        PC.SlotList       = { SkinnedSlotListBuffer, NumSkinned };
        PC.SkinnedData    = { SkinnedFrameDataBuffer };
        PC.RetainedStatic = { RetainedStaticBuffer };
        PC.OutVertices    = { GetPreSkinnedVerticesBuffer() };
        PC.OutPrevPositions = { PreSkinnedPrevPositionsBuffer, PreSkinnedPrevCapacity };

        DispatchComputeIndirect(CL, SkinShader, PC, GetSkinDispatchArgs());

        // Pre-skinned vertices feed every draw VS.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::MeshShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndexRead);
    }

    void FDefaultSceneRenderer::BuildDepthPyramid(RHI::FCmdListH CL, const FSceneImage& Source, const FSceneImage& Pyramid, bool bReduceMax,
                                                  uint32 SpdCounterIndex)
    {
        static const FShaderH ComputeShader = FShaderLibrary::Get("DepthPyramidSPD.slang");
        if (!ComputeShader)
        {
            return;
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
            LOG_ERROR("Depth pyramid: source image has no sampled heap slot; skipping. Sampling it would "
                      "index the texture heap with 0xFFFFFFFF and fault the device.");
            return;
        }

        PC.SrcDepthIndex      = (uint32)SrcDepthSlot;
        PC.ReduceMax          = bReduceMax ? 1u : 0u;
        PC.AtomicCounter      = { SpdCounter };
        for (uint32 i = 0; i < SpdMaxMips; ++i)
        {
            const uint32 SrcMip = (i < MipCount) ? i : 0u;
            const int32  Slot   = Pyramid.GetMipUAVIndex(SrcMip);
            if (Slot < 0)
            {
                LOG_ERROR("Depth pyramid: mip {} has no storage heap slot; skipping the pass.", SrcMip);
                return;
            }
            PC.MipUAV[i] = (uint32)Slot;
        }

        RHI::CmdDispatch(CL, MakeArgs(PC), DispatchX, DispatchY, 1);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::DepthPyramidPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        if (DrawCommands.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Depth Pyramid Pass (SPD)", tracy::Color::Orange);

        // Reverse-Z scene depth, so MIN reduction keeps the farthest occluder in each footprint.
        BuildDepthPyramid(CL,
            GetNamedImage(ENamedImage::DepthAttachment),
            GetNamedImage(ENamedImage::DepthPyramid),
            /*bReduceMax*/ false, /*SpdCounterIndex*/ 0u);
    }

    void FDefaultSceneRenderer::CascadePyramidPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& LightData   = Frame.Lighting.LightData;

        if (!LightData.bHasSun ||
            Frame.SceneGlobalData.CullData.bShadowOcclusionCull == 0u ||
            Frame.Geometry.DrawCommands.empty() ||
            Frame.Lighting.Lights[0].ShadowDataIndex == Constants::kIndexNone ||
            Frame.Views.CascadeViewBase == Constants::kIndexNoneU32)
        {
            bCascadePyramidValid.store(false, std::memory_order_release);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cascade Pyramid Pass (SPD)", tracy::Color::Orange3);

        BuildDepthPyramid(CL,
            GetNamedImage(ENamedImage::Cascade),
            GetNamedImage(ENamedImage::CascadePyramid),
            /*bReduceMax*/ true, /*SpdCounterIndex*/ 1u);

        bCascadePyramidValid.store(true, std::memory_order_release);
    }

    void FDefaultSceneRenderer::UpdateMeshletBoundFeedback(uint8 Slot)
    {
        const RHI::FGPUAllocation& Readback = MeshletBoundReadback[Slot];
        if (Readback.Gpu == 0)
        {
            return;
        }
        static_assert(FDefaultSceneRenderer::kTotalsSlots >= 8, "Totals[7] is read below.");
        if (const uint32* Mapped = Readback.CpuAs<const uint32>())
        {
            LastVisibleInstances     = Mapped[0];
            LastVisibleOverflowed    = Mapped[1];
            LastDrawListRequired     = Mapped[2];
            LastDrawListOverflowed   = Mapped[3];
            LastBlocksRequested      = Mapped[4];
            LastBlocksOverflowed     = Mapped[5];
            LastPreSkinRequested     = Mapped[6];
            LastPreSkinOverflowed    = Mapped[7];
        }

        LUMINA_PROFILE_VALUE("Cull/VisibleInstances",  (int64)LastVisibleInstances);
        LUMINA_PROFILE_VALUE("Cull/DrawListEntries",   (int64)LastDrawListRequired);
        LUMINA_PROFILE_VALUE("Cull/MeshletBlocks",     (int64)LastBlocksRequested);
        LUMINA_PROFILE_VALUE("Cull/PreSkinVertices",   (int64)LastPreSkinRequested);
    }

    void FDefaultSceneRenderer::DispatchGPUSceneCull(RHI::FCmdListH CL, const FFrameData& Frame)
    {
        static const FShaderH CullInstancesShader = FShaderLibrary::Get("CullInstances.slang");
        static const FShaderH DrawPrefixShader = FShaderLibrary::Get("BuildDrawPrefix.slang");
        if (CullInstancesShader == nullptr || DrawPrefixShader == nullptr)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("GPU Scene Cull", tracy::Color::Magenta);
        SCENE_GPU_SCOPE(CL, "GPU Scene Cull");

        const FFrameData::FGeometry::FRetainedUpload& Upload = Frame.Geometry.RetainedUpload;
        const FInstanceCullEntry* SrcCullEntries = ScenePrimitives.GetRetainedCullEntries();
        const FTransform3x4*      SrcTransforms  = ScenePrimitives.GetRetainedTransforms();
        const FInstanceStatic*    SrcStatic      = ScenePrimitives.GetRetainedStatic();

        const uint8  Slot          = CurrentFrameSlot;
        const uint32 RetainedSlots = Upload.SlotCount;
        const uint32 NumBatches    = Math::Max(Frame.Views.NumDrawsPerView, 1u);
        const uint32 NumCullViews  = (uint32)Frame.Views.CullViews.size();
        LUMINA_PROFILE_VALUE("Cull/RetainedSlots", (int64)RetainedSlots);

        if (!TotalsZeroed[Slot] && GetTotals())
        {
            RHI::CmdMemset(CL, GetTotals(), 0u);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            TotalsZeroed[Slot] = true;
        }

        {
            LUMINA_PROFILE_SECTION_COLORED("Retained Upload", tracy::Color::Magenta4);
            SCENE_GPU_SCOPE(CL, "Retained Upload");

            // Only when something is sent, since waiting on every earlier stage stalls the frame head for nothing otherwise.
            const bool bUploadsThisFrame = Upload.bFull || Upload.bFullStatic || !Upload.DirtySlots.empty() || !Upload.DirtyStaticSlots.empty();
            if (bUploadsThisFrame)
            {
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::AllCommands, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::TransferWrite,
                    RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
            }

            const SIZE_T CullBytes      = Math::Max<SIZE_T>(sizeof(FInstanceCullEntry), (SIZE_T)RetainedSlots * sizeof(FInstanceCullEntry));
            const SIZE_T TransformBytes = Math::Max<SIZE_T>(sizeof(FTransform3x4),      (SIZE_T)RetainedSlots * sizeof(FTransform3x4));
            const SIZE_T StaticBytes    = Math::Max<SIZE_T>(sizeof(FInstanceStatic),    (SIZE_T)RetainedSlots * sizeof(FInstanceStatic));

            // Retained state is zeroed on growth because a free slot IS zero; the CPU writes zero on free too.
            // A full re-send rewrites every live slot through the upload ring ahead of this list, so a preserve copy would clobber it.
            ReserveBuffer(CL, RetainedCullEntryBuffer, CullBytes,      /*bAllowShrink*/ Upload.bFull, /*bPreserveContents*/ !Upload.bFull,
                          Upload.bFull ? CullBytes : 0u);
            ReserveBuffer(CL, RetainedTransformBuffer, TransformBytes, /*bAllowShrink*/ Upload.bFull, /*bPreserveContents*/ !Upload.bFull,
                          Upload.bFull ? TransformBytes : 0u);
            ReserveBuffer(CL, RetainedStaticBuffer,    StaticBytes,    /*bAllowShrink*/ Upload.bFullStatic, /*bPreserveContents*/ !Upload.bFullStatic,
                          Upload.bFullStatic ? StaticBytes : 0u);

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
                                    && RetainedStaticBuffer.CapacityOf<FInstanceStatic>()    >= RetainedSlots;
            if (RetainedCullEntryBuffer && RetainedTransformBuffer && RetainedStaticBuffer && RetainedSlots > 0 && bRetainedFits)
            {
                // Contiguous slots are contiguous in the source arrays, so each run is one copy.
                auto CollectRuns = [](const TVector<uint32>& Slots, TVector<FUIntVector2>& Runs)
                {
                    Runs.clear();
                    const SIZE_T NumDirty = Slots.size();
                    for (SIZE_T i = 0; i < NumDirty; )
                    {
                        SIZE_T j = i + 1;
                        while (j < NumDirty && Slots[j] == Slots[j - 1] + 1u)
                        {
                            ++j;
                        }
                        Runs.push_back(FUIntVector2{ Slots[i], (uint32)(j - i) });
                        i = j;
                    }
                };

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
                    CollectRuns(Upload.DirtySlots, RetainedRunScratch);

                    // A crowd dirties thousands of short runs, which cost more to record as copies than to scatter on the GPU.
                    constexpr SIZE_T ScatterUploadMinSlots     = 1024;
                    constexpr SIZE_T ScatterUploadMaxRunLength = 64;
                    const SIZE_T NumDirty = Upload.DirtySlots.size();
                    if (NumDirty >= ScatterUploadMinSlots && RetainedRunScratch.size() * ScatterUploadMaxRunLength > NumDirty)
                    {
                        const RHI::FGPURange Slots = RHI::CopyTransientArray(Upload.DirtySlots.data(), NumDirty);

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

                if (Upload.bFullStatic)
                {
                    StageWrite(RetainedStaticBuffer.Gpu, SrcStatic, (SIZE_T)RetainedSlots * sizeof(FInstanceStatic), true);
                    FlushStagedWrites(CL);
                }
                else
                {
                    CollectRuns(Upload.DirtyStaticSlots, RetainedRunScratch);
                    WriteBufferRuns(CL, RetainedStaticBuffer.Gpu, SrcStatic, sizeof(FInstanceStatic), RetainedRunScratch, true);
                }
                FinishInstanceBlockBounds(CL);
                LaunchDeferredStageFills();
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
        }

        // Surface descriptors are interned, so this moves only when a new distinct LOD table appears.
        const uint32 NumDescs = Upload.SurfaceDescCount;
        {
            const SIZE_T DescBytes = Math::Max<SIZE_T>(sizeof(FSurfaceDescGPU), (SIZE_T)NumDescs * sizeof(FSurfaceDescGPU));
            const RHI::GPUPtr PrevDescs = SurfaceDescBuffer.Gpu;
            // Same reasoning as above, since a reclaim drops descriptors this frame may not re-send.
            ReserveBuffer(CL, SurfaceDescBuffer, DescBytes, /*bAllowShrink*/ Upload.bSurfaceDescsChanged);

            if (SurfaceDescBuffer.Gpu != PrevDescs)
            {
                UploadedSurfaceDescs = 0;
            }

            const bool bNeedsDescWrite = SurfaceDescBuffer && NumDescs > 0
                                      && (Upload.bSurfaceDescsChanged || UploadedSurfaceDescs != NumDescs);
            if (bNeedsDescWrite)
            {
                if (ScenePrimitives.GetSurfaceDescCount() == NumDescs)
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
        }

        const uint32 VisibleCapacity = FrameVisibleInstanceCapacity;
        const uint32 SeedViews       = Math::Max(NumCullViews, 1u);
        const SIZE_T ViewDrawEntries = (SIZE_T)SeedViews * (SIZE_T)NumBatches;
        // MeshDrawArgsRing is deliberately NOT resized here; CompileDrawCommands_Render sizes it.
        ReserveBuffer(CL, RenderBucketRing[Slot], ViewDrawEntries * sizeof(FRenderBucketGPU));

        // Every field starts at zero, since CullInstances accumulates skinned batches too.
        RHI::CmdMemset(CL, { GetRenderBuckets().Gpu, ViewDrawEntries * sizeof(FRenderBucketGPU) }, 0u);

        Barriers::TransferToCompute(CL);

        // Reads RetainedStatic, so it has to sit after that upload and after the barrier publishing it.
        SkinnedMeshletBoundsPass(CL, Frame);

        //~ Cull + LOD + compaction.
        if (RetainedSlots > 0 && NumCullViews > 0 && NumDescs > 0 && VisibleInstanceRing[Slot])
        {
            LUMINA_PROFILE_SECTION_COLORED("Cull Instances", tracy::Color::Magenta2);
            SCENE_GPU_SCOPE(CL, "Cull Instances");

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
            };
            static_assert(sizeof(FCullInstancesPC) == 224, "FCullInstancesPC must match CullInstances.slang.");

            FCullInstancesPC PC = {};
            PC.NumViews               = NumCullViews;
            PC.NumBatches             = NumBatches;
            PC.bUseLODs               = FrameSettings.bUseLODs ? 1u : 0u;
            PC.SkinFrameTag           = CurrentSkinnedFrameTag;
            PC.RetainedCullEntries    = { RetainedCullEntryBuffer, RetainedSlots };
            PC.RetainedTransforms     = { RetainedTransformBuffer, RetainedSlots };
            PC.RetainedStatic         = { RetainedStaticBuffer, RetainedSlots };
            PC.SurfaceDescs           = { SurfaceDescBuffer, UploadedSurfaceDescs };
            PC.OutInstances           = { VisibleInstanceRing[Slot], VisibleCapacity };
            PC.OutInstanceCount       = RHI::TGPUSpan<uint32>::FromAddress(GetCullCounters().Address, 1u);
            PC.OutInstanceViewRanges  = { GetInstanceViewRanges() };
            PC.OutBuckets             = { GetRenderBuckets(), NumCullViews * NumBatches };
            PC.OutOverflowFlag        = RHI::TGPUSpan<uint32>::FromAddress(GetCullCounters().Address + sizeof(uint32), 1u);
            PC.SkinnedFrameData       = { SkinnedFrameDataBuffer };
            PC.PreSkinArena           = { GetPreSkinnedVerticesBuffer(), PreSkinnedVertexCapacity };
            PC.OutPreSkinCursor       = RHI::TGPUSpan<uint32>::FromAddress(GetCullCounters().Address + sizeof(uint32) * 2, 1u);
            // An empty span turns the block skip off, which is the safe answer whenever the bounds are not current.
            PC.BlockBounds            = bInstanceBlockBoundsValid && CVarInstanceBlockCull.GetValue()
                                      ? RHI::TGPUSpan<FInstanceBlockBounds>{ InstanceBlockBoundsBuffer, (uint32)InstanceBlockBounds.size() }
                                      : RHI::TGPUSpan<FInstanceBlockBounds>{};

            // Blades are appended into the retained block here, between its upload and the cull that
            // reads it, so grass is just more instances by the time anything downstream looks.
            GrassScatterPass(CL);

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullInstancesShader));
            // CullInstances.slang undoes the fold with GroupID.y * MAX_DISPATCH_AXIS * LOCAL_SIZE_X.
            const FUIntVector2 Grid = RenderUtils::FoldGroupCount(RenderUtils::GetGroupCount(RetainedSlots, 64u));
            RHI::CmdDispatch(CL, MakeArgs(PC), Grid.x, Grid.y, 1u);

            Barriers::ComputeToGeometry(CL);
        }

        //~ Draw-argument layout, from counts the CPU never sees.
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
            PC.NumViews             = SeedViews;
            PC.NumDraws             = NumBatches;
            PC.DrawList             = { GetMeshletDrawList(), DrawListCapacity };
            PC.BlockList            = { GetMeshletBlocks(), BlockListCapacity };
            PC.PreSkinArena         = { GetPreSkinnedVerticesBuffer(), PreSkinnedVertexCapacity };
            PC.VisibleInstances     = { VisibleInstanceRing[Slot], VisibleCapacity };
            PC.Buckets              = { GetRenderBuckets() };
            PC.InstanceCount        = RHI::TGPUSpan<uint32>::FromAddress(GetCullCounters().Address, 4u);
            PC.OutTotals            = { GetTotals(), kTotalsSlots };
            PC.OutBlockDispatchArgs = { GetBlockDispatchArgs() };

            DispatchCompute(CL, DrawPrefixShader, PC, 1u, 1u, 1u);

            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::MeshShader |
                RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite | RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);
        }

        if (NumCullViews > 0u)
        {
            LUMINA_PROFILE_SECTION_COLORED("Build Meshlet Blocks", tracy::Color::Magenta2);
            SCENE_GPU_SCOPE(CL, "Build Meshlet Blocks");
            DispatchMeshletBlockBuild(CL, /*bLatePass*/ false);
        }

        // Every view's meshlets, culled once; everything after reads what this wrote.
        MeshletCullPass(CL, EMeshletSlice::Early);

        if (MeshletBoundReadback[Slot].Gpu != 0)
        {
            // The cull passes write the totals in compute, and the copy must not read them mid-write.
            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead);
            RHI::CmdMemcpy(CL, { MeshletBoundReadback[Slot].Gpu, sizeof(uint32) * kTotalsSlots }, { GetTotals().Gpu, sizeof(uint32) * kTotalsSlots });
        }
    }

    //~ Begin new-RHI helpers

    void FDefaultSceneRenderer::DispatchMeshletBlockBuild(RHI::FCmdListH CL, bool bLatePass)
    {
        static const FShaderH BlocksShader = FShaderLibrary::Get("BuildMeshletBlocks.slang");
        const uint32 NumCullViews = RenderFrame != nullptr ? (uint32)RenderFrame->Views.CullViews.size() : 0u;
        if (!BlocksShader || NumCullViews == 0u)
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
        } BPC = {};
        static_assert(sizeof(FBuildMeshletBlocksPC) == 128, "FBuildMeshletBlocksPC must match BuildMeshletBlocks.slang.");

        BPC.NumViews           = NumCullViews;
        BPC.NumBatches         = Math::Max(RenderFrame->Views.NumDrawsPerView, 1u);
        BPC.InstanceCount      = RHI::TGPUSpan<uint32>::FromAddress(GetCullCounters().Address, 4u);
        BPC.VisibleInstances   = { VisibleInstanceRing[CurrentFrameSlot], FrameVisibleInstanceCapacity };
        BPC.InstanceViewRanges = { GetInstanceViewRanges() };
        BPC.Buckets            = { GetRenderBuckets() };
        BPC.OutBlockList       = { GetMeshletBlocks(), BlockListCapacity };
        BPC.SurfaceDescs       = { SurfaceDescBuffer, UploadedSurfaceDescs };
        // Latched by the early build, so a console change between the two builds cannot split one phase and not the other.
        if (!bLatePass)
        {
            bSplitMeshletPhasesThisFrame = CVarSplitMeshletPhases.GetValue();
        }
        BPC.BuildMode          = bLatePass ? 1u : (bSplitMeshletPhasesThisFrame ? 0u : 2u);
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

    void FDefaultSceneRenderer::MeshletCullPass(RHI::FCmdListH CL, EMeshletSlice Slice)
    {
        static const FShaderH ArgsShader = FShaderLibrary::Get("BuildMeshletCullArgs.slang");
        static const FShaderH CullShader = FShaderLibrary::Get("MeshletCull.slang");
        if (ArgsShader == nullptr || CullShader == nullptr || RenderFrame == nullptr)
        {
            return;
        }

        const uint32 NumViews = (uint32)RenderFrame->Views.CullViews.size();
        const uint32 NumDraws = Math::Max(RenderFrame->Views.NumDrawsPerView, 1u);
        if (NumViews == 0u || !GetRenderBuckets() || !GetMeshletBlocks())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Meshlet Cull", tracy::Color::Magenta);
        SCENE_GPU_SCOPE(CL, "Meshlet Cull");

        // Instances last frame did not see get their blocks only now, and only if this frame's pyramid leaves them visible.
        if (Slice == EMeshletSlice::Late && bSplitMeshletPhasesThisFrame)
        {
            SCENE_GPU_SCOPE(CL, "Build Late Meshlet Blocks");
            DispatchMeshletBlockBuild(CL, /*bLatePass*/ true);
        }

        struct FCullArgsPC
        {
            uint32 NumViews;
            uint32 NumDraws;
            uint32 Slice;
            uint32 bPost;
            uint32 MaxMeshGroups;
            uint32 SubDrawsPerSlice;
            uint32 bSplitPhases;
            uint32 _Pad1;
            RHI::TGPUSpan<FRenderBucketGPU> Buckets;
            RHI::TGPUSpan<RHI::FDispatchIndirectArguments> OutCullDispatchArgs;
            RHI::TGPUSpan<RHI::FDrawMeshTasksIndirectArguments> OutMeshDrawArgs;
        } APC = {};
        static_assert(sizeof(FCullArgsPC) == 80, "FCullArgsPC must match BuildMeshletCullArgs.slang.");

        APC.NumViews                = NumViews;
        APC.NumDraws                = NumDraws;
        APC.Slice                   = (uint32)Slice;
        APC.MaxMeshGroups           = Math::Max(RHI::GetMaxMeshWorkGroupCount(), 1u);
        APC.SubDrawsPerSlice        = MeshSubDrawsPerSlice;
        APC.bSplitPhases            = bSplitMeshletPhasesThisFrame ? 1u : 0u;
        APC.Buckets             = { GetRenderBuckets() };
        APC.OutCullDispatchArgs = { GetMeshletCullDispatchArgs() };
        APC.OutMeshDrawArgs     = { GetMeshDrawArgs() };

        // Serial prefix, so one group; the post pass below is per-bucket and takes a real grid.
        constexpr uint32 kArgsGroupSize = 64;
        const uint32 PostGroups = (NumViews * NumDraws + kArgsGroupSize - 1u) / kArgsGroupSize;

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
            uint32 _Pad0;
            RHI::TGPUSpan<FRenderBucketGPU> Buckets;
            RHI::TGPUSpan<FUIntVector2>     BlockList;
            RHI::TGPUSpan<uint32>           PrevVisibility;
            RHI::TGPUSpan<uint32>           OutVisibility;
            RHI::TGPUSpan<uint32>           MeshletVisibility;
        } CPC = {};
        static_assert(sizeof(FMeshletCullPC) == 104, "FMeshletCullPC must match MeshletCull.slang.");

        CPC.NumViews       = NumViews;
        CPC.NumDraws       = NumDraws;
        CPC.Slice          = (uint32)Slice;
        CPC.VisibilityTag  = InstanceVisibilityTag;
        CPC.bSplitPhases   = APC.bSplitPhases;
        CPC.Buckets        = { GetRenderBuckets() };
        CPC.BlockList      = { GetMeshletBlocks() };
        CPC.PrevVisibility = { GetInstanceVisibilityPrev(), InstanceVisibilityCapacity };
        CPC.OutVisibility  = { GetInstanceVisibilityWrite(), InstanceVisibilityCapacity };
        CPC.MeshletVisibility = { GetMeshletVisibility(), CVarCullMeshletVisibility.GetValue() != 0 ? MeshletVisibilityCapacity : 0u };

        DispatchComputeIndirect(CL, CullShader, CPC, GetMeshletCullDispatchArgs());
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::MeshShader | RHI::EStageFlags::IndirectArguments,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);

        // Turn what it appended into the slice every draw indexes, and the counts they draw from.
        APC.bPost = 1u;
        DispatchCompute(CL, ArgsShader, APC, PostGroups, 1u, 1u);

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::MeshShader |
            RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);
    }

    void FDefaultSceneRenderer::DrawMeshletBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch,
                                               const FMeshletPassContext& Ctx)
    {
        if (MeshSubDrawsPerSlice == 0u)
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
        Push.MaxMeshGroups        = Math::Max(RHI::GetMaxMeshWorkGroupCount(), 1u);
        Push.CullViewIndex        = Ctx.CullViewIndex;
        Push.ShadowDataIndex      = Ctx.ShadowDataIndex;
        Push.ViewIndex            = Ctx.ShadowViewIndex;
        Push.ViewportW            = Ctx.ViewportW;
        Push.ViewportH            = Ctx.ViewportH;
        Push.ShadowViews          = Ctx.ShadowViews;
        Push.ArgViewStride        = NumDrawsPerView;
        Push.Pad0                 = 0;

        // One mesh workgroup per surviving meshlet, so the grid IS the survivor count.
        const uint32 SliceArgBase = (ArgIndex * kMeshletSliceCount + Slice) * MeshSubDrawsPerSlice;

        if (Ctx.ShadowViews.Count > 0u)
        {
            // An empty view's slot holds zero groups, so it costs the command processor a read and nothing more.
            ASSERT(MeshSubDrawsPerSlice == 1u);
            const uint32 ViewStride = NumDrawsPerView * kMeshletSliceCount * sizeof(RHI::FDrawMeshTasksIndirectArguments);
            RHI::CmdDrawMeshTasksIndirect(CL, MakeArgs(Push), GetMeshDrawArgs().Skip(SliceArgBase * sizeof(RHI::FDrawMeshTasksIndirectArguments)), Ctx.ShadowViews.Count, ViewStride);
            return;
        }

        RHI::CmdDrawMeshTasksIndirectCount(CL, MakeArgs(Push), GetMeshDrawArgs().Skip(SliceArgBase * sizeof(RHI::FDrawMeshTasksIndirectArguments)), GetRenderBuckets().Skip(ArgIndex * sizeof(FRenderBucketGPU) + offsetof(FRenderBucketGPU, SubDrawCount) + Slice * sizeof(uint32)), MeshSubDrawsPerSlice, sizeof(RHI::FDrawMeshTasksIndirectArguments));
    }
}
