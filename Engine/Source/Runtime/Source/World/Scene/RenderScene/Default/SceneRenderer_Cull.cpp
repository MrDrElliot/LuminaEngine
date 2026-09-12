#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
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

        const RHI::GPUPtr PrevArena = BoneArenaBuffer.Gpu;
        ReserveBuffer(CL, BoneArenaBuffer, ArenaBytes);
        if (!BoneArenaBuffer)
        {
            return;
        }

        // A reallocation drops every resident pose, so the dirty set no longer describes the buffer.
        if (BoneArenaBuffer.Gpu != PrevArena)
        {
            WriteBuffer(CL, BoneArenaBuffer.Gpu, Mirror.data(), ArenaBytes);
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

            WriteBuffer(CL, BoneArenaBuffer.Gpu + (uint64)Start * sizeof(FBoneTransform),
                        Mirror.data() + Start, (uint64)(End - Start) * sizeof(FBoneTransform));
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

            const FSkinnedFrameData& D = Data[Slot];
            const uint32 Begin = Math::Min(D.SurfaceMeshletOffset, D.ShadowMeshletOffset);
            const uint32 End   = Math::Max(D.SurfaceMeshletOffset + D.SurfaceMeshletCount,
                                           D.ShadowMeshletOffset + D.ShadowMeshletCount);
            const uint32 Len   = (End > Begin) ? (End - Begin) : 0u;

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

            const uint32 Begin = Math::Min(D.SurfaceMeshletOffset, D.ShadowMeshletOffset);
            const uint32 End   = Math::Max(D.SurfaceMeshletOffset + D.SurfaceMeshletCount,
                                           D.ShadowMeshletOffset + D.ShadowMeshletCount);
            const uint32 Len   = (End > Begin) ? (End - Begin) : 0u;

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

        // Only the gathered slots, coalesced; ungathered slots are rejected by their frame tag.
        // Every run is one slot wide, so this sorts the bare indices rather than (base, count) pairs.
        TVector<uint32>& Sorted = SkinnedUploadScratch;
        Sorted.assign(Slots.begin(), Slots.end());

        Algo::Sort(Sorted);

        // A merged gap is still staged and copied, so this trades one copy region against ~1 KiB of bytes.
        constexpr uint32 kSkinnedMergeGap = 24;

        const uint32 Count = (uint32)Data.size();

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

        uint32 RunStart = Sorted[0];
        uint32 RunEnd   = Sorted[0] + 1u;
        for (SIZE_T i = 1; i < Sorted.size(); ++i)
        {
            const uint32 Start = Sorted[i];
            const uint32 End   = Sorted[i] + 1u;
            if (Start <= RunEnd + kSkinnedMergeGap)
            {
                RunEnd = Math::Max(RunEnd, End);
                continue;
            }
            AddRun(RunStart, RunEnd);
            RunStart = Start;
            RunEnd   = End;
        }
        AddRun(RunStart, RunEnd);

        WriteBufferRuns(CL, SkinnedFrameDataBuffer.Gpu, Data.data(), sizeof(FSkinnedFrameData), Runs);

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

        // Windowed like the other GPU-fed sizes, so the raw readback's lag does not collapse it.
        const uint32 DrawListWanted = Math::Max(DrawListDemand.Observe(LastDrawListRequired), 65536u);
        const SIZE_T MeshletDrawListSize = Math::Max<SIZE_T>(
            sizeof(uint32) * 2,
            (SIZE_T)DrawListWanted * sizeof(uint32) * 2);

        // Must mirror MeshletCullPass' bucket count exactly, which clamps both operands to 1.
        const SIZE_T NumArgSlots = (SIZE_T)Math::Max(NumCullViews, 1u) * (SIZE_T)Math::Max(NumDraws, 1u);
        
        ReserveBuffer(CL, PreSkinnedVerticesBuffer, PreSkinnedSize);
        PreSkinnedVertexCapacity = (uint32)Math::Min<uint64>(
            PreSkinnedVerticesBuffer.Size / sizeof(FPreSkinnedVertex), 0xFFFFFFFFull);
        {
            const uint8 Slot = CurrentFrameSlot;
            ReserveBuffer(CL, MeshletDrawListRing[Slot], MeshletDrawListSize);
            DrawListCapacity = MeshletDrawListRing[Slot].CapacityOf<FUIntVector2>();

            // Read after the resize, because last frame's value describes the other ring slot's buffer.
            const uint32 MaxGroups = Math::Max(RHI::GetMaxMeshWorkGroupCount(), 1u);
            const uint32 WantedSubDraws = (DrawListCapacity == 0u) ? 1u : (((DrawListCapacity - 1u) / MaxGroups) + 1u);
            MeshSubDrawsPerSlice   = Math::Clamp(WantedSubDraws, 1u, 8u);

            const uint8 SubDrawBit = (uint8)(1u << (MeshSubDrawsPerSlice - 1u));
            if ((LoggedSubDrawMask & SubDrawBit) == 0u)
            {
                LoggedSubDrawMask |= SubDrawBit;
                LOG_INFO("Meshlet sub-draws per slice: {} (draw-list capacity {}, mesh workgroup limit {}). "
                         "A slice needing more than {} sub-draws has the remainder dropped.",
                         MeshSubDrawsPerSlice, DrawListCapacity, MaxGroups, MeshSubDrawsPerSlice);
            }

            // Mesh draw args are MeshSubDrawsPerSlice per bucket and slice, 12B stride.
            const SIZE_T MeshDrawArgsSize = Math::Max<SIZE_T>(
                sizeof(RHI::FDrawMeshTasksIndirectArguments),
                NumArgSlots * (SIZE_T)kMeshletSliceCount * (SIZE_T)MeshSubDrawsPerSlice
                    * sizeof(RHI::FDrawMeshTasksIndirectArguments));

            ReserveBuffer(CL, MeshDrawArgsRing[Slot], MeshDrawArgsSize);
            
            // Windowed peak, not the last readback: that count lags kFramesInFlight and collapses the
            // allocation the moment the camera looks at something empty.
            const uint32 VisibleDemand   = VisibleInstanceDemand.Observe(LastVisibleInstances);
            uint32 VisibleCapacityWanted = Math::Max(VisibleDemand, 4096u);

            const uint32 VisibleCapacityMax = Math::Max(Frame.Geometry.RetainedUpload.SlotCount, 1u);

            if (LastVisibleInstances == 0u)
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
            const uint32 BlockListWanted = BlockListDemand.Observe(LastBlocksRequested);

            const SIZE_T MeshletBlockSize = Math::Max<SIZE_T>(
                sizeof(uint32) * 2,
                (SIZE_T)Math::Max<uint32>(BlockListWanted, 1u) * sizeof(uint32) * 2);
            ReserveBuffer(CL, MeshletBlockRing[Slot], MeshletBlockSize);
            BlockListCapacity = MeshletBlockRing[Slot].CapacityOf<FUIntVector2>();

            // Requirement is from kFramesInFlight ago, capacity is from now, so print both sides.
            const auto LogOverflow = [](const char* What, uint32 Needed, uint32 GrownTo)
            {
                LOG_WARN("RenderScene: {} overflowed -- {} needed, capacity now {}. Geometry was dropped "
                         "that frame; this clears once the larger allocation cycles in.",
                         What, Needed, GrownTo);
            };

            // Demand over the cap, not the readback's lag, since only the latter corrects itself.
            const bool bPreSkinCapped = LastPreSkinOverflowed && LastPreSkinRequested > GetMaxPreSkinnedVertices();

            static uint32 OverflowLogCounter = 0;
            if (LastVisibleOverflowed || LastDrawListOverflowed || bPreSkinCapped)
            {
                if ((OverflowLogCounter++ % 60u) == 0u)
                {
                    if (LastVisibleOverflowed)  { LogOverflow("visible-instance buffer", LastVisibleInstances, FrameVisibleInstanceCapacity); }
                    if (LastDrawListOverflowed) { LogOverflow("meshlet draw list",       LastDrawListRequired, DrawListCapacity); }
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

            const bool bSunChanged = LastIBLSunDirection != LightData.SunDirection || bLastIBLHasSun != (LightData.bHasSun != 0);
            const bool bMapChanged = LastIBLEnvironmentMapID != EnvironmentMapID;

            const bool bResChanged = Frame.Volumetrics.IBLResolution != LastExtractedIBLResolution;
            LastExtractedIBLResolution = Frame.Volumetrics.IBLResolution;

            if (FrameFlags.bHasEnvironment &&
                (!bIBLValid || bEnvParamsChanged || bSunChanged || bMapChanged || bResChanged))
            {
                bIBLDirty                  = true;
                LastIBLEnvironmentParams   = EnvironmentParams;
                LastIBLEnvironmentMapID    = EnvironmentMapID;
                LastIBLSunDirection        = LightData.SunDirection;
                bLastIBLHasSun             = (LightData.bHasSun != 0);
                bIBLValid                  = true;
            }

            constexpr float SunCosThreshold = 0.99996f;
            const bool bConvHasSunChanged = bLastConvolvedHasSun != (LightData.bHasSun != 0);
            float SunCos = 1.0f;
            if (bLastConvolvedHasSun && LightData.bHasSun)
            {
                SunCos = Math::Dot(LastConvolvedSunDirection, LightData.SunDirection);
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
                LastConvolvedSunDirection      = LightData.SunDirection;
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

            SceneRootShared.Materials            = Render().GetMaterialManager().GetMaterialSpan();
            SceneRootShared.Collections          = Render().GetCollectionManager().GetSpan();
            SceneRootShared.MeshletDrawList      = { GetMeshletDrawList(), DrawListCapacity };
            SceneRootShared.PreSkinnedVertices   = { GetPreSkinnedVerticesBuffer(), PreSkinnedVertexCapacity };
            SceneRootShared.SkinnedFrameData     = { SkinnedFrameDataBuffer };
            // Spheres and cones are indexed by one base, so each carries the same capacity.
            SceneRootShared.SkinnedMeshletBounds = { SkinnedMeshletBoundsBuffer, SkinnedMeshletBoundsCapacity };
            SceneRootShared.SkinnedMeshletCones  = { SkinnedMeshletConeBuffer, SkinnedMeshletBoundsCapacity };
            if (IsGTAOEnabled())
            {
                SceneGlobalData.GTAOSettings.AOTextureIndex = (uint32)CurrentView->Images[(int)ENamedImage::GTAOBlur].GetResourceID();
            }

            FrameFlags.bShadowMaskValid = (LightData.bHasSun != 0) &&
                                              (Frame.Lighting.Lights[0].ShadowDataIndex != INDEX_NONE);
            if (FrameFlags.bShadowMaskValid)
            {
                SceneGlobalData.ShadowMaskIndex = (uint32)CurrentView->Images[(int)ENamedImage::ShadowMask].GetResourceID();
            }
            else
            {
                SceneGlobalData.ShadowMaskIndex = ~0u;
            }

            // Published unconditionally, since the images outlive the pass and nothing else reads them.
            SceneGlobalData.MomentZerothIndex = (uint32)CurrentView->Images[(int)ENamedImage::MomentZeroth].GetResourceID();
            SceneGlobalData.MomentsIndex      = (uint32)CurrentView->Images[(int)ENamedImage::Moments].GetResourceID();

            PublishFogGlobals(SceneGlobalData);

            SetSceneRoot(CL, *CurrentView, RHI::CopyTransient(SceneGlobalData));

            DispatchGPUSceneCull(CL, Frame);
        }
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

        const uint8  Slot     = CurrentFrameSlot;
        const uint32 NumPairs = NumSkinned * 2u;

        ReserveBuffer(CL, SkinWorkBaseRing[Slot], (uint64)NumPairs * sizeof(uint32));
        if (!SkinWorkBaseRing[Slot] || !GetSkinDispatchArgs() || !GetPreSkinnedVerticesBuffer()
            || !SkinnedSlotListBuffer || !SkinnedFrameDataBuffer || !RetainedStaticBuffer)
        {
            return;
        }

        //~ Lay out the dispatch as one workgroup per instance, block and meshlet, enumerated on the GPU.
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
            WPC.OutWorkBase     = { SkinWorkBaseRing[Slot], NumPairs };
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
        } PC = {};
        static_assert(sizeof(FSkinningPushConstants) == 80, "FSkinningPushConstants must match Skinning.slang.");

        PC.WorkBase       = { SkinWorkBaseRing[Slot], NumPairs };
        PC.SlotList       = { SkinnedSlotListBuffer, NumSkinned };
        PC.SkinnedData    = { SkinnedFrameDataBuffer };
        PC.RetainedStatic = { RetainedStaticBuffer };
        PC.OutVertices    = { GetPreSkinnedVerticesBuffer() };

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
            Frame.Geometry.DrawCommands.empty() ||
            Frame.Lighting.Lights[0].ShadowDataIndex == INDEX_NONE ||
            Frame.Views.CascadeViewBase == ~0u)
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

            const SIZE_T CullBytes      = Math::Max<SIZE_T>(sizeof(FInstanceCullEntry), (SIZE_T)RetainedSlots * sizeof(FInstanceCullEntry));
            const SIZE_T TransformBytes = Math::Max<SIZE_T>(sizeof(FTransform3x4),      (SIZE_T)RetainedSlots * sizeof(FTransform3x4));
            const SIZE_T StaticBytes    = Math::Max<SIZE_T>(sizeof(FInstanceStatic),    (SIZE_T)RetainedSlots * sizeof(FInstanceStatic));

            // Retained state is zeroed on growth because a free slot IS zero; the CPU writes zero on free too.
            ReserveBuffer(CL, RetainedCullEntryBuffer, CullBytes,      /*bAllowShrink*/ Upload.bFull);
            ReserveBuffer(CL, RetainedTransformBuffer, TransformBytes, /*bAllowShrink*/ Upload.bFull);
            ReserveBuffer(CL, RetainedStaticBuffer,    StaticBytes,    /*bAllowShrink*/ Upload.bFull);

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

            if (RetainedCullEntryBuffer && RetainedTransformBuffer && RetainedStaticBuffer && RetainedSlots > 0)
            {
                if (Upload.bFull)
                {
                    LUMINA_PROFILE_SECTION_COLORED("FULL resend", tracy::Color::Red3);

                    StageWrite(RetainedCullEntryBuffer.Gpu,
                               SrcCullEntries, (SIZE_T)RetainedSlots * sizeof(FInstanceCullEntry));
                    StageWrite(RetainedTransformBuffer.Gpu,
                               SrcTransforms, (SIZE_T)RetainedSlots * sizeof(FTransform3x4));
                    StageWrite(RetainedStaticBuffer.Gpu,
                               SrcStatic, (SIZE_T)RetainedSlots * sizeof(FInstanceStatic));
                    FlushStagedWrites(CL);
                }
                else
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

                    // Both buffers share the slot list, so one run scan feeds both writes.
                    CollectRuns(Upload.DirtySlots, RetainedRunScratch);
                    WriteBufferRuns(CL, RetainedCullEntryBuffer.Gpu, SrcCullEntries, sizeof(FInstanceCullEntry), RetainedRunScratch);
                    WriteBufferRuns(CL, RetainedTransformBuffer.Gpu, SrcTransforms,  sizeof(FTransform3x4),      RetainedRunScratch);

                    CollectRuns(Upload.DirtyStaticSlots, RetainedRunScratch);
                    WriteBufferRuns(CL, RetainedStaticBuffer.Gpu, SrcStatic, sizeof(FInstanceStatic), RetainedRunScratch);
                }
            }
            const uint32 CullCap      = RetainedCullEntryBuffer.CapacityOf<FInstanceCullEntry>();
            const uint32 TransformCap = RetainedTransformBuffer.CapacityOf<FTransform3x4>();
            const uint32 StaticCap    = RetainedStaticBuffer.CapacityOf<FInstanceStatic>();
            RetainedDeviceCapacity.store(Math::Min(CullCap, Math::Min(TransformCap, StaticCap)), std::memory_order_release);
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
            };
            static_assert(sizeof(FCullInstancesPC) == 208, "FCullInstancesPC must match CullInstances.slang.");

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

        {
            static const FShaderH BlocksShader = FShaderLibrary::Get("BuildMeshletBlocks.slang");
            if (BlocksShader && NumCullViews > 0u)
            {
                LUMINA_PROFILE_SECTION_COLORED("Build Meshlet Blocks", tracy::Color::Magenta2);
                SCENE_GPU_SCOPE(CL, "Build Meshlet Blocks");

                struct FBuildMeshletBlocksPC
                {
                    uint32 NumViews;
                    uint32 NumBatches;
                    RHI::TGPUSpan<uint32>           InstanceCount;
                    RHI::TGPUSpan<FGPUInstance>     VisibleInstances;
                    RHI::TGPUSpan<FUIntVector2>     InstanceViewRanges;
                    RHI::TGPUSpan<FRenderBucketGPU> Buckets;
                    RHI::TGPUSpan<FUIntVector2>     OutBlockList;
                } BPC = {};
                static_assert(sizeof(FBuildMeshletBlocksPC) == 88, "FBuildMeshletBlocksPC must match BuildMeshletBlocks.slang.");

                BPC.NumViews           = NumCullViews;
                BPC.NumBatches         = NumBatches;
                BPC.InstanceCount      = RHI::TGPUSpan<uint32>::FromAddress(GetCullCounters().Address, 4u);
                BPC.VisibleInstances   = { VisibleInstanceRing[Slot], VisibleCapacity };
                BPC.InstanceViewRanges = { GetInstanceViewRanges() };
                BPC.Buckets            = { GetRenderBuckets() };
                BPC.OutBlockList       = { GetMeshletBlocks(), BlockListCapacity };

                RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BlocksShader));

                RHI::CmdDispatchIndirect(CL, MakeArgs(BPC), GetBlockDispatchArgs());

                // The block list has exactly one reader, and it is compute now in MeshletCullPass below.
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                    RHI::EStageFlags::Compute,
                    RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            }
        }

        // Every view's meshlets, culled once; everything after reads what this wrote.
        MeshletCullPass(CL, EMeshletSlice::Early);

        if (MeshletBoundReadback[Slot].Gpu != 0)
        {
            RHI::CmdMemcpy(CL, { MeshletBoundReadback[Slot].Gpu, sizeof(uint32) * kTotalsSlots }, { GetTotals().Gpu, sizeof(uint32) * kTotalsSlots });
        }
    }

    //~ Begin new-RHI helpers

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

        struct FCullArgsPC
        {
            uint32 NumViews;
            uint32 NumDraws;
            uint32 Slice;
            uint32 bPost;
            uint32 MaxMeshGroups;
            uint32 SubDrawsPerSlice;
            uint32 _Pad0;
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
            RHI::TGPUSpan<FRenderBucketGPU> Buckets;
            RHI::TGPUSpan<FUIntVector2>     BlockList;
            RHI::TGPUSpan<uint32>           PrevVisibility;
            RHI::TGPUSpan<uint32>           OutVisibility;
        } CPC = {};
        static_assert(sizeof(FMeshletCullPC) == 80, "FMeshletCullPC must match MeshletCull.slang.");

        CPC.NumViews       = NumViews;
        CPC.NumDraws       = NumDraws;
        CPC.Slice          = (uint32)Slice;
        CPC.VisibilityTag  = InstanceVisibilityTag;
        CPC.Buckets        = { GetRenderBuckets() };
        CPC.BlockList      = { GetMeshletBlocks() };
        CPC.PrevVisibility = { GetInstanceVisibilityPrev(), InstanceVisibilityCapacity };
        CPC.OutVisibility  = { GetInstanceVisibilityWrite(), InstanceVisibilityCapacity };

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
        } Push;
        static_assert(sizeof(FMeshletPassPush) == 48, "FMeshletPassPush must match FMeshletPassArgs in MeshletGeometry.slang.");

        Push.Buckets              = { GetRenderBuckets() };
        Push.ArgBase              = ArgIndex;
        Push.Slice                = Slice;
        Push.MaxMeshGroups        = Math::Max(RHI::GetMaxMeshWorkGroupCount(), 1u);
        Push.CullViewIndex        = Ctx.CullViewIndex;
        Push.ShadowDataIndex      = Ctx.ShadowDataIndex;
        Push.ViewIndex            = Ctx.ShadowViewIndex;
        Push.ViewportW            = Ctx.ViewportW;
        Push.ViewportH            = Ctx.ViewportH;

        // One mesh workgroup per surviving meshlet, so the grid IS the survivor count.
        const uint32 SliceArgBase = (ArgIndex * kMeshletSliceCount + Slice) * MeshSubDrawsPerSlice;

        RHI::CmdDrawMeshTasksIndirectCount(CL, MakeArgs(Push), GetMeshDrawArgs().Skip(SliceArgBase * sizeof(RHI::FDrawMeshTasksIndirectArguments)), GetRenderBuckets().Skip(ArgIndex * sizeof(FRenderBucketGPU) + offsetof(FRenderBucketGPU, SubDrawCount) + Slice * sizeof(uint32)), MeshSubDrawsPerSlice, sizeof(RHI::FDrawMeshTasksIndirectArguments));
    }
}
