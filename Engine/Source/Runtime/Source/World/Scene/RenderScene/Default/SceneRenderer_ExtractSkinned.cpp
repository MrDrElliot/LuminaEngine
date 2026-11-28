#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace SceneRendererExtractSkinnedInternal
    {
        bool IsOrthographicLOD(const FCullView& View)
        {
            return (GetCullViewFlags(View) & ECullViewFlags::OrthographicLOD) != 0u;
        }

        // Measured once per primitive, since its bone tolerance and every surface's LOD range read the same distance.
        float LODViewDistance(const FCullView& View, const FVector4& Sphere)
        {
            return Math::Length(FVector3(Sphere) - FVector3(View.ViewOriginAndFlags));
        }

        // Distance to the nearest point of the bounds, so a large mesh is never coarser than its closest part allows.
        float LODReach(const FCullView& View, const FVector4& Sphere, float ViewDistance)
        {
            if (IsOrthographicLOD(View))
            {
                return 1.0f;
            }
            return Math::Max(ViewDistance - Sphere.w, 0.0f);
        }

        // Group spheres nest loosely inside the instance sphere, mirroring kClusterReachSlack in Culling.slang.
        constexpr float kClusterReachSlack = 1.25f;

        // Mirrors SelectClusterSlotRange in Culling.slang, so the slots pre-skinned hold every meshlet the camera's cut can take.
        FUIntVector2 SelectClusterSlotRange(const FSurfaceDescGPU& Desc, const FCullView& View, const FVector4& Sphere, float MaxScale, float ViewDistance)
        {
            const uint32 NumSlots = Math::Min(Desc.NumLODs, (uint32)MAX_MESH_LODS);
            if (NumSlots <= 1u || View.LODErrorScale <= 0.0f)
            {
                return FUIntVector2(0u, 0u);
            }

            float ReachNear = 1.0f;
            float ReachFar  = 1.0f;
            if (!IsOrthographicLOD(View))
            {
                ReachNear = Math::Max(ViewDistance - Sphere.w * kClusterReachSlack, 0.0f);
                ReachFar  = ViewDistance + Sphere.w * kClusterReachSlack;
            }
            const float ErrorScale = View.LODErrorScale * MaxScale * Desc.LODErrorMultiplier;

            uint32 Lo = 0u;
            while (Lo + 1u < NumSlots && Desc.LODParentError[Lo] * ErrorScale <= ReachNear)
            {
                ++Lo;
            }
            uint32 Hi = NumSlots - 1u;
            while (Hi > Lo && Desc.LODError[Hi] * ErrorScale > ReachFar)
            {
                --Hi;
            }
            return FUIntVector2(Lo, Hi);
        }

        // The mesh-local error the camera tolerates here, which is what picks a skinned LOD and how much of the skeleton may hold still.
        float ResolveBoneLODTolerance(const SWorldRenderSettings& Settings, const FCullView* CameraView, const FVector4& Sphere, float MaxScale, float ViewDistance)
        {
            if (!Settings.bUseLODs || CameraView == nullptr || CameraView->LODErrorScale <= 0.0f || MaxScale <= 0.0f)
            {
                return 0.0f;
            }
            return LODReach(*CameraView, Sphere, ViewDistance) / (CameraView->LODErrorScale * MaxScale);
        }

        // Leaves FrameTag and SkinnedBoundsBase alone, since UploadSkinnedFrameData stamps both on the render thread.
        void PublishSkinnedFrameData(FSkinnedFrameData& Out, const FSkinnedFrameData& In)
        {
            Out.SurfaceMeshletOffset    = In.SurfaceMeshletOffset;
            Out.SurfaceMeshletCount     = In.SurfaceMeshletCount;
            Out.MeshletTotalCount       = In.MeshletTotalCount;
            Out.SkinnedVertexBase       = In.SkinnedVertexBase;
            Out.BoneOffset              = In.BoneOffset;
            Out.RetainedSlot            = In.RetainedSlot;
        }

        struct FSkinnedFrameDataTarget
        {
            FSkinnedFrameData*  Data;
            uint32              Num;
            uint32              RetainedSlots;
        };

        // Claims room in a shared list a batch at a time, since every worker bumping the cursor per element fights over one line.
        template<typename T>
        class TBatchedAppend
        {
        public:
            LE_NO_COPYMOVE(TBatchedAppend);

            TBatchedAppend(TAtomic<uint32>& InCursor, TVector<T>& InDest)
                : Cursor(&InCursor)
                , Dest(&InDest)
            {}

            ~TBatchedAppend()
            {
                Flush();
            }

            void Add(const T& Value)
            {
                Pending[NumPending++] = Value;
                if (NumPending == kBatch)
                {
                    Flush();
                }
            }

        private:
            void Flush()
            {
                if (NumPending == 0u)
                {
                    return;
                }
                const uint32 Base     = Cursor->fetch_add(NumPending, std::memory_order_relaxed);
                const uint32 Capacity = (uint32)Dest->Num();
                for (uint32 i = 0; i < NumPending && Base + i < Capacity; ++i)
                {
                    (*Dest)[Base + i] = Pending[i];
                }
                NumPending = 0;
            }

            static constexpr uint32 kBatch = 64;

            TAtomic<uint32>* Cursor;
            TVector<T>*      Dest;
            T                Pending[kBatch];
            uint32           NumPending = 0;
        };

        constexpr uint64 kNoPreSkinShareKey = ~0ull;

        // Packs what decides a borrowed pose's skinned vertices, or returns none when a field outgrows its bits.
        uint64 PreSkinShareKey(uint32 BoneArenaBase, uint32 MeshletHeaderSlot, FUIntVector2 Slots, uint32 Surface)
        {
            if (BoneArenaBase >= (1u << 24) || MeshletHeaderSlot >= (1u << 16) || Slots.x >= (1u << 4) || Slots.y >= (1u << 4) || Surface >= (1u << 5))
            {
                return kNoPreSkinShareKey;
            }
            return ((uint64)BoneArenaBase << 29) | ((uint64)MeshletHeaderSlot << 13) | ((uint64)Slots.x << 9) | ((uint64)Slots.y << 5) | Surface;
        }

        // The LOD table comes from the interned FSurfaceDescGPU the binding names, so many instances of few meshes read a handful of descs.
        void EmitPrimitiveSurfaces(FDefaultSceneRenderer::FThreadLocalDrawData& Local,
                                   const FScenePrimitive& Prim,
                                   const FSurfaceBinding* Bindings,
                                   const FSurfaceDescGPU* SurfaceDescs,
                                   uint32 NumSurfaceDescs,
                                   uint32 BoneArenaBase,
                                   const FSkinnedFrameDataTarget& Target,
                                   const SWorldRenderSettings& Settings,
                                   const FCullView* CameraView,
                                   TSpan<const FCullView* const> CaptureViews,
                                   const FVector4& Sphere,
                                   float MaxScale,
                                   float ViewDistance,
                                   FPreSkinShareTable* PreSkinShare)
        {
            const uint32 MeshletHeaderSlot = Prim.MeshletHeaderSlot;

            for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
            {
                const FSurfaceBinding& Binding = Bindings[s];
                const uint32 InstanceSlot = Binding.InstanceSlot;
                const uint32 SkinnedIndex = Binding.SkinnedIndex;
                if (Binding.SurfaceDescIndex >= NumSurfaceDescs || InstanceSlot >= Target.RetainedSlots || SkinnedIndex == kNoSkinnedIndex)
                {
                    continue;
                }
                const FSurfaceDescGPU& Desc = SurfaceDescs[Binding.SurfaceDescIndex];

                // Shadows take the camera's pick, and a capture that sees the mesh widens the slice it then cuts from its own camera.
                FUIntVector2 Slots(0u, 0u);
                if (Settings.bUseLODs && CameraView != nullptr)
                {
                    Slots = SelectClusterSlotRange(Desc, *CameraView, Sphere, MaxScale, ViewDistance);
                    for (const FCullView* Capture : CaptureViews)
                    {
                        const FUIntVector2 Range = SelectClusterSlotRange(Desc, *Capture, Sphere, MaxScale, LODViewDistance(*Capture, Sphere));
                        Slots = FUIntVector2(Math::Min(Slots.x, Range.x), Math::Max(Slots.y, Range.y));
                    }
                }

                const uint32 LastLOD    = Desc.NumLODs > 0u ? Math::Min(Desc.NumLODs, (uint32)MAX_MESH_LODS) - 1u : 0u;
                const uint32 SliceBegin = Desc.LODMeshletOffset[Slots.x];
                const uint32 SliceEnd   = Desc.LODMeshletOffset[Slots.y] + Desc.LODMeshletCount[Slots.y];

                FSkinnedFrameData Data = {};
                Data.SurfaceMeshletOffset = SliceBegin;
                // Zero meshlet counts gate the cull shader's MeshletHeader deref.
                if (MeshletHeaderSlot != 0u)
                {
                    Data.SurfaceMeshletCount = SliceEnd > SliceBegin ? SliceEnd - SliceBegin : 0u;
                    Data.MeshletTotalCount   = Desc.LODMeshletOffset[LastLOD] + Desc.LODMeshletCount[LastLOD];
                }
                // Seeded to the sentinel so a rejected slot keeps no slice rather than last frame's base.
                Data.SkinnedVertexBase       = kNoPreSkinBase;
                if (PreSkinShare && MeshletHeaderSlot && SkinnedIndex < kPreSkinShareBit)
                {
                    const uint64 Key = PreSkinShareKey(BoneArenaBase, MeshletHeaderSlot, Slots, s);
                    const uint32 Owner = Key != kNoPreSkinShareKey ? PreSkinShare->Claim(Key, SkinnedIndex) : SkinnedIndex;
                    if (Owner != SkinnedIndex)
                    {
                        Data.SkinnedVertexBase = kPreSkinShareBit | Owner;
                    }
                }
                Data.BoneOffset              = BoneArenaBase;
                Data.RetainedSlot            = InstanceSlot;

                // Indices are disjoint across primitives, and the array only grows in the merge, after every emit.
                if (SkinnedIndex < Target.Num)
                {
                    PublishSkinnedFrameData(Target.Data[SkinnedIndex], Data);
                    Local.SkinnedIndices.Add(SkinnedIndex);
                }
                else
                {
                    Local.DeferredSlots.Emplace(SkinnedIndex, Data);
                }
            }
        }

        // A GPU-crowd pose skips the upload and marks the slice as holding no CPU pose, unless a ragdoll dirtied the CPU bones over it.
        bool EmitGpuPose(FScenePrimitive& Tracked, const SSkeletalMeshComponent& Mesh, uint32 BoneSlice,
                         TVector<FGpuPoseRequest>& Requests, TVector<FAnimWeightedSample>& Samples)
        {
            if (!Mesh.bGpuPose || Mesh.bRenderBonesDirty || Mesh.GpuSkeleton == nullptr || Mesh.GpuSamples.IsEmpty() || (uint32)Mesh.GpuSkeleton->GetNumBones() != Tracked.BoneCount)
            {
                return false;
            }

            FGpuPoseRequest& Request = Requests.EmplaceGetRef();
            Request.BoneSlice    = BoneSlice;
            Request.FirstSample  = (uint32)Samples.Num();
            Request.NumSamples   = (uint32)Mesh.GpuSamples.Num();
            Request.RootLockBone = Mesh.GpuRootLockBone;
            Request.Skeleton     = Mesh.GpuSkeleton;
            Samples.Append(Mesh.GpuSamples);

            Tracked.UploadedSliceBase = kNoBoneSlice;
            return true;
        }

        // Writes the pose into its slice unless the arena already holds it, and reports the range to upload.
        void PackPoseIntoSlice(FScenePrimitive& Tracked, SSkeletalMeshComponent& MeshComponent, uint32 BoneSlice,
                               TVector<FBoneTransform>& Arena, TVector<FUIntVector2>& UploadRanges)
        {
            const uint32 BoneCount = Tracked.BoneCount;
            if (BoneCount == 0u || BoneSlice == kNoBoneSlice || static_cast<int64>(BoneSlice) + BoneCount > Arena.Num())
            {
                return;
            }

            // Folded in here so the residency test below is the one gate, even for a writer that never bumped the serial.
            if (MeshComponent.bRenderBonesDirty)
            {
                MeshComponent.bRenderBonesDirty = false;
                ++MeshComponent.PoseSerial;
            }

            if (Tracked.UploadedSliceBase == BoneSlice && Tracked.UploadedPoseSerial == MeshComponent.PoseSerial)
            {
                return;
            }

            FBoneTransform* Dst = Arena.GetData() + BoneSlice;

            static_assert(sizeof(FBoneTransform) == 2 * sizeof(FVector4), "PackedBones holds two vectors per bone.");

            // A partial pose cannot be packed against this slice, so it falls through to identity.
            if (MeshComponent.PackedPoseSerial == MeshComponent.PoseSerial && static_cast<size_t>(MeshComponent.PackedBones.Num()) == (SIZE_T)BoneCount * 2u)
            {
                Memory::Memcpy(Dst, MeshComponent.PackedBones.GetData(), (SIZE_T)BoneCount * sizeof(FBoneTransform));
            }
            else if ((uint32)MeshComponent.BoneTransforms.Num() == BoneCount)
            {
                SkeletalUtils::PackRenderBones(MeshComponent.BoneTransforms.GetData(), BoneCount, Dst);
            }
            else
            {
                // With no pose, BoneWorld * InvBindMatrix collapses to identity for every bone.
                const FBoneTransform IdentityBone = IdentityBoneTransform();
                for (uint32 b = 0; b < BoneCount; ++b)
                {
                    Dst[b] = IdentityBone;
                }
            }

            Tracked.UploadedSliceBase  = BoneSlice;
            Tracked.UploadedPoseSerial = MeshComponent.PoseSerial;
            UploadRanges.Add(FUIntVector2{ BoneSlice, BoneCount });
        }
    }

    void FDefaultSceneRenderer::ScheduleSkinnedGather(ECS::FRegistry& Registry)
    {
        LUMINA_PROFILE_SCOPE();
        if (ScenePrimitives.GetSkinnedPrimitiveCount() == 0)
        {
            Jobs::Spawn(Jobs::EJobPriority::High, ExtractLatch, [this]
            {
                MergeMeshDrawData(ThreadLocalStorage);
            });
            return;
        }

        auto FollowerStorage = Registry.GetStorage<SFollowerPoseComponent>();
        auto SkeletalStorage = Registry.GetStorage<SSkeletalMeshComponent>();

        // Rebuilt only on a shape change, so the cull walks skeletal, not every primitive.
        const TVector<uint32>& SkeletalList = ScenePrimitives.GetSkeletalIndices();
        SkeletalPrimitiveIndices = SkeletalList.GetData();
        const uint32 NumSkeletal = (uint32)SkeletalList.Num();

        // Exact bound, since only a skeletal primitive can become a candidate.
        if ((uint32)SkinnedCandidates.Num() < NumSkeletal)
        {
            SkinnedCandidates.SetNum(NumSkeletal);
            PendingSliceAllocs.SetNum(NumSkeletal);
            SharedPoses.SetNum(NumSkeletal);
        }
        SkinnedCandidateCursor.store(0, std::memory_order_relaxed);

        Jobs::Spawn(Jobs::EJobPriority::High, ExtractLatch,
            [this, FollowerStorage, SkeletalStorage, NumSkeletal]() -> Jobs::TTask<void>
        {
            co_await LightsReady;

            co_await Jobs::Call(Jobs::ForEach(NumSkeletal, [this](const Jobs::FRange& Range)
            {
                LUMINA_PROFILE_SECTION("Cull Skinned Primitives");
                FThreadLocalDrawData& Local = AcquireThreadLocalDrawData(Range.Thread);
                CullSkinnedPrimitives(Range, Local);
            }, GSkinnedCullGrain));

            // Sizes the arena from what survived, which is what keeps it O(visible), not O(scene).
            LayoutSkinnedBoneSlices(ThreadLocalStorage, FollowerStorage, SkeletalStorage);

            co_await Jobs::Call(Jobs::ForEach(SkinnedCandidateCount, [this, SkeletalStorage](const Jobs::FRange& Range)
            {
                LUMINA_PROFILE_SECTION("Emit Skinned Primitives");
                FThreadLocalDrawData& Local = AcquireThreadLocalDrawData(Range.Thread);
                EmitSkinnedPrimitives(Range, Local, SkeletalStorage);
            }, GSkinnedEmitGrain));

            MergeMeshDrawData(ThreadLocalStorage);
        });
    }

    void FDefaultSceneRenderer::CullSkinnedPrimitives(const Jobs::FRange& Range, FThreadLocalDrawData& Local)
    {
        using namespace SceneRendererExtractSkinnedInternal;

        const FFrameData&        Frame     = *ExtractFrame;
        const FSceneCullContext& SceneCull = Frame.Geometry.SceneCullContext;
        const FVector3           CameraPos = FVector3(Frame.SceneGlobalData.CameraData.Location);

        const FScenePrimitive*    Prims   = ScenePrimitives.GetPrimitives();
        const FVector4*           Spheres = ScenePrimitives.GetBounds();
        const FPrimitiveCullData* Culls   = ScenePrimitives.GetCullData();

        // Dense, since the range indexes skeletal primitives rather than the whole table.
        const uint32* Skeletal = SkeletalPrimitiveIndices;

        // A skeleton on a layer no view draws is never skinned.
        const bool   bOtherViews  = !Frame.Views.CaptureViews.IsEmpty() || Frame.ReflectionProbes.BakingProbe >= 0;
        const uint32 DrawnLayers  = Frame.ViewLayers.CullMask | (bOtherViews ? RenderLayers::kAllMask : 0u);

        TBatchedAppend<uint32> Accepted(SkinnedCandidateCursor, SkinnedCandidates);

        const auto Accept = [&](uint32 PrimIndex)
        {
            const FScenePrimitive& Prim = Prims[PrimIndex];
            // Parked awaiting a resolve or a skeleton, which the sync pass retries.
            if (Prim.Surfaces == nullptr || Prim.SurfaceCount == 0u || Prim.BoneCount == 0u)
            {
                return;
            }
            if ((Prim.RenderLayers & DrawnLayers) == 0u)
            {
                return;
            }

            // A superset of what the emit pass accepts; anything it rejects just leaves an unused hole.
            Accepted.Add(PrimIndex);
        };

        if (!SceneCull.bEnabled)
        {
            for (uint32 d = Range.Start; d < Range.End; ++d)
            {
                Accept(Skeletal[d]);
            }
            return;
        }

        alignas(32) float CX[8], CY[8], CZ[8], RR[8], MD[8];

        uint32 d = Range.Start;
        for (; d + 8u <= Range.End; d += 8u)
        {
            uint32 CasterMask = 0u;
            for (uint32 n = 0; n < 8u; ++n)
            {
                const uint32 PrimIndex = Skeletal[d + n];
                const FVector4& S = Spheres[PrimIndex];
                CX[n] = S.x;
                CY[n] = S.y;
                CZ[n] = S.z;
                RR[n] = S.w;
                MD[n] = Culls[PrimIndex].MaxDrawDistance;
                CasterMask |= (Culls[PrimIndex].bCastShadow != 0u) ? (1u << n) : 0u;
            }

            uint32 KeepMask = 0u;
            uint32 MaybeMask = 0u;
            SceneCull.ShouldKeepBatch8(CX, CY, CZ, RR, MD, CasterMask, CameraPos, KeepMask, MaybeMask);

            for (uint32 n = 0; n < 8u; ++n)
            {
                const uint32 Bit = 1u << n;
                bool bKeep = (KeepMask & Bit) != 0u;

                if (!bKeep && (MaybeMask & Bit) != 0u)
                {
                    const uint32 PrimIndex = Skeletal[d + n];
                    bKeep = SceneCull.ShouldKeep(FVector3(CX[n], CY[n], CZ[n]), RR[n],
                                                 Culls[PrimIndex].bCastShadow != 0u, MD[n], CameraPos);
                }

                if (bKeep)
                {
                    Accept(Skeletal[d + n]);
                }
            }
        }

        for (; d < Range.End; ++d)
        {
            const uint32 PrimIndex = Skeletal[d];
            const FVector4& S = Spheres[PrimIndex];

            if (!SceneCull.ShouldKeep(FVector3(S), S.w, Culls[PrimIndex].bCastShadow != 0u,
                                      Culls[PrimIndex].MaxDrawDistance, CameraPos))
            {
                continue;
            }

            Accept(PrimIndex);
        }
    }

    void FPreSkinShareTable::Prepare(uint32 MinEntries)
    {
        constexpr uint64 kFrameShift = 48;
        const uint32 Wanted = (uint32)Math::NextPowerOfTwo((int)Math::Max(MinEntries * 2u, 64u));
        uint64 Frame = (FrameBits >> kFrameShift) + 1u;
        if ((uint32)Keys.Num() < Wanted || Frame > 0xFFFFu)
        {
            Keys.Init(0ull, (int32)Math::Max(Wanted, (uint32)Keys.Num()));
            Owners.Init(0ull, Keys.Num());
            Frame = 1u;
        }
        FrameBits = Frame << kFrameShift;
    }

    uint32 FPreSkinShareTable::Claim(uint64 Key, uint32 SkinnedIndex)
    {
        constexpr uint64 kFrameMask = 0xFFFF000000000000ull;
        constexpr uint32 kMaxProbes = 64;
        if (Keys.IsEmpty())
        {
            return SkinnedIndex;
        }
        const uint64 Tagged = FrameBits | Key;
        const uint32 Mask   = (uint32)Keys.Num() - 1u;
        uint32 Probe        = (uint32)Containers::MixHash64(Key) & Mask;

        for (uint32 Step = 0; Step < kMaxProbes && Step <= Mask; ++Step, Probe = (Probe + 1u) & Mask)
        {
            std::atomic_ref<uint64> Slot(Keys[Probe]);
            uint64 Seen = Slot.load(std::memory_order_acquire);
            if ((Seen & kFrameMask) != FrameBits && Slot.compare_exchange_strong(Seen, Tagged, std::memory_order_acq_rel))
            {
                std::atomic_ref<uint64>(Owners[Probe]).store(FrameBits | SkinnedIndex, std::memory_order_release);
                return SkinnedIndex;
            }
            if (Seen != Tagged)
            {
                continue;
            }

            // The winner stores its index right after its exchange, so this waits out a handful of instructions.
            std::atomic_ref<uint64> Owner(Owners[Probe]);
            uint64 Claimed = Owner.load(std::memory_order_acquire);
            while ((Claimed & kFrameMask) != FrameBits)
            {
                _mm_pause();
                Claimed = Owner.load(std::memory_order_acquire);
            }
            return (uint32)Claimed;
        }
        return SkinnedIndex;
    }

    void FDefaultSceneRenderer::LayoutSkinnedBoneSlices(TVector<FThreadLocalDrawData>& ThreadLocal, const auto& FollowerStorage, const auto& SkeletalStorage)
    {
        using namespace SceneRendererExtractSkinnedInternal;

        LUMINA_PROFILE_SECTION("Layout Skinned Bone Slices");

        const uint32 NumPrimitives = ScenePrimitives.Num();

        // Grown, never cleared, since only entries this frame wrote are ever read.
        if ((uint32)BoneSliceByPrimitive.Num() < NumPrimitives)
        {
            BoneSliceByPrimitive.SetNum(NumPrimitives, kNoBoneSlice);
        }
        if ((uint32)BoneSliceBorrowed.Num() < NumPrimitives)
        {
            BoneSliceBorrowed.SetNum(NumPrimitives, 0u);
        }

        SkinnedCandidateCount = Math::Min(SkinnedCandidateCursor.load(std::memory_order_relaxed),
                                          (uint32)SkinnedCandidates.Num());
        PendingSliceCursor.store(0, std::memory_order_relaxed);
        SharedPoseCursor.store(0, std::memory_order_relaxed);

        ++BoneSliceFrameNumber;

        const FScenePrimitive* LayoutPrims = ScenePrimitives.GetPrimitives();

        // Reuse needs nothing shared; only the misses fall through to the serial allocator below.
        Jobs::ParallelFor(SkinnedCandidateCount, [&](const Jobs::FRange& Range)
        {
            TBatchedAppend<uint32>       Pending(PendingSliceCursor, PendingSliceAllocs);
            TBatchedAppend<FUIntVector2> Shared(SharedPoseCursor, SharedPoses);

            // A crowd follows a handful of leaders, so the last lookup usually answers the next follower too.
            ECS::FEntity LastLeaderEntity = ECS::NullEntity;
            uint32       LastLeader       = Constants::kIndexNoneU32;

            for (uint32 c = Range.Start; c < Range.End; ++c)
            {
                const uint32 Index = SkinnedCandidates[c];
                if (Index >= NumPrimitives)
                {
                    continue;
                }

                BoneSliceBorrowed[Index] = 0u;
                const FScenePrimitive& Prim = LayoutPrims[Index];

                // Its own slice is left untouched, so it ages out while the leader's serves both.
                const SFollowerPoseComponent* Follower = FollowerStorage.TryGet(Prim.Entity);
                if (Follower != nullptr && Follower->bSharesLeaderPose)
                {
                    BoneSliceByPrimitive[Index] = kNoBoneSlice;
                    if (Follower->CopiedLeader != LastLeaderEntity)
                    {
                        LastLeaderEntity = Follower->CopiedLeader;
                        LastLeader       = ScenePrimitives.FindSkeletalPrimitive(LastLeaderEntity);
                    }

                    // Only the entity and bone count are read here, which nothing in this pass writes.
                    const uint32 Leader = LastLeader;
                    const bool bCanLend = Leader < NumPrimitives && Leader != Index && LayoutPrims[Leader].Entity == LastLeaderEntity &&
                                          LayoutPrims[Leader].BoneCount == Prim.BoneCount;
                    Shared.Add(FUIntVector2(Index, bCanLend ? Leader : Constants::kIndexNoneU32));
                    continue;
                }

                const uint32 Base = ScenePrimitives.TouchBoneSlice(Index, Prim.BoneCount, BoneSliceFrameNumber);
                BoneSliceByPrimitive[Index] = Base;
                if (Base == kNoBoneSlice)
                {
                    Pending.Add(Index);
                }
            }
        }, GSkinnedLayoutGrain, Jobs::EJobPriority::High);

        const auto AcquirePending = [&]()
        {
            const uint32 NumPending = Math::Min(PendingSliceCursor.load(std::memory_order_relaxed), (uint32)PendingSliceAllocs.Num());
            for (uint32 n = 0; n < NumPending; ++n)
            {
                const uint32 Index = PendingSliceAllocs[n];
                BoneSliceByPrimitive[Index] = ScenePrimitives.AcquireBoneSlice(Index, LayoutPrims[Index].BoneCount, BoneSliceFrameNumber);
            }
            PendingSliceCursor.store(0, std::memory_order_relaxed);
        };
        
        AcquirePending();

        // A leader nothing else gathered this frame still owes its followers a pose, so it is given a slice once here and packed below.
        UngatheredLeaderScratch.Reset();
        const uint32 NumShared = Math::Min(SharedPoseCursor.load(std::memory_order_relaxed), (uint32)SharedPoses.Num());
        uint32 LastLeader = Constants::kIndexNoneU32;
        for (uint32 n = 0; n < NumShared; ++n)
        {
            const uint32 Leader = SharedPoses[n].y;
            if (Leader == Constants::kIndexNoneU32 || Leader == LastLeader)
            {
                continue;
            }
            LastLeader = Leader;
            if (LayoutPrims[Leader].BoneSliceFrame != BoneSliceFrameNumber)
            {
                const uint32 Base = ScenePrimitives.AcquireBoneSlice(Leader, LayoutPrims[Leader].BoneCount, BoneSliceFrameNumber);
                BoneSliceByPrimitive[Leader] = Base;
                if (Base != kNoBoneSlice)
                {
                    UngatheredLeaderScratch.Add(Leader);
                }
            }
        }

        // Every leader slice is settled, so followers only read them; one whose leader cannot lend packs its own copy of the pose.
        Jobs::ParallelFor(NumShared, [&](const Jobs::FRange& Range)
        {
            TBatchedAppend<uint32> OwnSlice(PendingSliceCursor, PendingSliceAllocs);
            for (uint32 n = Range.Start; n < Range.End; ++n)
            {
                const FUIntVector2 Pose = SharedPoses[n];
                const uint32 Base = Pose.y != Constants::kIndexNoneU32 ? BoneSliceByPrimitive[Pose.y] : kNoBoneSlice;
                if (Base != kNoBoneSlice)
                {
                    BoneSliceByPrimitive[Pose.x] = Base;
                    BoneSliceBorrowed[Pose.x]    = 1u;
                }
                else
                {
                    OwnSlice.Add(Pose.x);
                }
            }
        }, GSkinnedLayoutGrain, Jobs::EJobPriority::High);
        
        AcquirePending();

        PreSkinShare.Prepare(NumShared);

        ScenePrimitives.ReleaseStaleBoneSlices(BoneSliceFrameNumber, kBoneSliceGraceFrames);

        ExtractFrame->Geometry.BoneCount = ScenePrimitives.GetBoneSliceExtent();
        ExtractFrame->Geometry.BonesData.SetNumUninitialized(ExtractFrame->Geometry.BoneCount);

        FScenePrimitive* MutablePrims = ScenePrimitives.GetMutablePrimitives();
        for (const uint32 Leader : UngatheredLeaderScratch)
        {
            FScenePrimitive& Tracked = MutablePrims[Leader];
            if (SSkeletalMeshComponent* LeaderMesh = SkeletalStorage.TryGet(Tracked.Entity))
            {
                PackPoseIntoSlice(Tracked, *LeaderMesh, BoneSliceByPrimitive[Leader],
                                  ExtractFrame->Geometry.BonesData, LeaderPoseUploadRanges);
            }
        }
    }

    void FDefaultSceneRenderer::EmitSkinnedPrimitives(const Jobs::FRange& Range, FThreadLocalDrawData& Local, const auto& SkeletalStorage)
    {
        using namespace SceneRendererExtractSkinnedInternal;

        const FFrameData&        Frame           = *ExtractFrame;
        const FSceneCullContext& SceneCull       = Frame.Geometry.SceneCullContext;
        const FSceneGlobalData&  SceneGlobalData = Frame.SceneGlobalData;
        const FVector3           CameraPos       = FVector3(SceneGlobalData.CameraData.Location);
        const FCullView*         CameraView      = &Frame.Views.CameraLODView;
        const TVector<FCullView>& CaptureLODViews = Frame.Views.CaptureLODViews;
        const FCullView*         SeenByCaptures[GMaxCullViews];

        const FScenePrimitive*    Prims           = ScenePrimitives.GetPrimitives();
        const FVector4*           Spheres         = ScenePrimitives.GetBounds();
        const FPrimitiveCullData* Culls           = ScenePrimitives.GetCullData();
        const FSurfaceBinding*    Bindings        = ScenePrimitives.GetBindings();
        const FSurfaceDescGPU*    SurfaceDescs    = ScenePrimitives.GetSurfaceDescs();
        const uint32              NumSurfaceDescs = ScenePrimitives.GetSurfaceDescCount();
        FScenePrimitive*          MutablePrims    = ScenePrimitives.GetMutablePrimitives();

        // Sized by LayoutSkinnedBoneSlices before this pass runs, and every slice below is disjoint.
        TVector<FBoneTransform>& ArenaMirror = ExtractFrame->Geometry.BonesData;
        const uint32 ArenaCount = (uint32)ArenaMirror.Num();

        const double WorldTime = World->GetTimeSinceWorldCreation();

        TVector<FSkinnedFrameData>& SkinnedData = ExtractFrame->Geometry.SkinnedFrameData;
        const FSkinnedFrameDataTarget Target{ SkinnedData.GetData(), (uint32)SkinnedData.Num(), ScenePrimitives.GetRetainedSlotCount() };

        for (uint32 c = Range.Start; c < Range.End; ++c)
        {
            const uint32 i = SkinnedCandidates[c];
            const uint32 BoneSlice = BoneSliceByPrimitive[i];
            // Its slice request failed, so there is nothing to emit.
            if (BoneSlice == kNoBoneSlice)
            {
                continue;
            }

            const FScenePrimitive& Prim = Prims[i];
            SSkeletalMeshComponent* MeshComponent = SkeletalStorage.TryGet(Prim.Entity);
            if (MeshComponent == nullptr)
            {
                continue;
            }

            const FVector4&           Sphere = Spheres[i];
            const FPrimitiveCullData& Cull   = Culls[i];

            const FVector3 Center = FVector3(Sphere);
            const float    Radius = Sphere.w;

            const FVector3 ToCamera = Center - CameraPos;
            float          DistSq   = Math::Dot(ToCamera, ToCamera);

            // The captures that draw this mesh, which animate it, refine its LOD and keep its bones moving as the camera would.
            uint32 NumCaptureViews = 0u;
            for (int32 k = 0; k < CaptureLODViews.Num(); ++k)
            {
                const FCullView& View = CaptureLODViews[k];
                if ((View.LayerMask & Prim.RenderLayers) != 0u && PassesCaptureFilter(View, Prim.CaptureFilterMask)
                    && Frame.Views.CaptureLODFrusta[k].IntersectsSphere(Center, Radius))
                {
                    SeenByCaptures[NumCaptureViews++] = &View;
                    const FVector3 ToCapture = Center - FVector3(View.ViewOriginAndFlags);
                    DistSq = Math::Min(DistSq, Math::Dot(ToCapture, ToCapture));
                }
            }
            const TSpan<const FCullView* const> CaptureViews(SeenByCaptures, NumCaptureViews);

            if (NumCaptureViews > 0u || SceneCull.IsCameraVisible(Center, Radius, Cull.MaxDrawDistance, CameraPos))
            {
                MeshComponent->LastRenderedTime = WorldTime;
            }

            // Both counts were cached at sync; a skeleton swap re-syncs the primitive before it is gathered again.
            const uint32 SkeletonBoneCount = Prim.BoneCount;

            uint32 BoneArenaBase = kNoBoneSlice;
            if (SkeletonBoneCount > 0 && (SIZE_T)BoneSlice + SkeletonBoneCount <= static_cast<size_t>(ArenaCount))
            {
                BoneArenaBase = BoneSlice;

                // A borrowed slice belongs to the leader, whose own gather or the layout pass packs it.
                if (!BoneSliceBorrowed[i] && !EmitGpuPose(MutablePrims[i], *MeshComponent, BoneSlice, Local.GpuPoseRequests, Local.GpuPoseSamples))
                {
                    PackPoseIntoSlice(MutablePrims[i], *MeshComponent, BoneSlice, ArenaMirror, Local.BoneUploadRanges);
                }
            }

            MeshComponent->LastDistanceOverRadius = Radius > 0.0f ? Math::Sqrt(DistSq) / Radius : 0.0f;

            const float MaxScale     = Prim.WorldMaxScale;
            const float ViewDistance = LODViewDistance(*CameraView, Sphere);
            float BoneTolerance = ResolveBoneLODTolerance(FrameSettings, CameraView, Sphere, MaxScale, ViewDistance);
            for (const FCullView* Capture : CaptureViews)
            {
                BoneTolerance = Math::Min(BoneTolerance, ResolveBoneLODTolerance(FrameSettings, Capture, Sphere, MaxScale, LODViewDistance(*Capture, Sphere)));
            }
            MeshComponent->LastBoneLODTolerance = BoneTolerance;

            // Only a follower skinning a borrowed pose can share the vertices its leader's other followers skin.
            const bool bSharesPose = BoneSliceBorrowed[i] && BoneArenaBase != kNoBoneSlice;
            EmitPrimitiveSurfaces(Local, Prim, Bindings + Prim.BindingBase,
                                  SurfaceDescs, NumSurfaceDescs,
                                  BoneArenaBase, Target, FrameSettings, CameraView, CaptureViews, Sphere, MaxScale, ViewDistance,
                                  bSharesPose ? &PreSkinShare : nullptr);
        }
    }

    void FDefaultSceneRenderer::MergeMeshDrawData(TVector<FThreadLocalDrawData>& ThreadLocal)
    {
        using namespace SceneRendererExtractSkinnedInternal;

        LUMINA_PROFILE_SECTION("Merge Mesh Draw Data");

        FFrameData& Frame               = *ExtractFrame;
        auto& DrawCommands              = Frame.Geometry.DrawCommands;
        auto& OpaqueDrawList            = Frame.Geometry.OpaqueDrawList;
        auto& TranslucentDrawList       = Frame.Geometry.TranslucentDrawList;
        auto& DeferredMaterials         = Frame.Geometry.DeferredMaterials;
        uint32& NumDrawsPerView         = Frame.Views.NumDrawsPerView;

        const uint32 NumThreads = (uint32)ThreadLocal.Num();

        DeferredMaterials.Reset();

        TVector<FUIntVector2>& BoneUploadRanges = Frame.Geometry.BoneUploadRanges;
        BoneUploadRanges.Reset();
        BoneUploadRanges.Append(LeaderPoseUploadRanges);
        LeaderPoseUploadRanges.Reset();

        TVector<FGpuPoseRequest>&     GpuPoseRequests = Frame.Geometry.GpuPoseRequests;
        TVector<FAnimWeightedSample>& GpuPoseSamples  = Frame.Geometry.GpuPoseSamples;
        GpuPoseRequests.Reset();
        GpuPoseSamples.Reset();

        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FThreadLocalDrawData& Local = ThreadLocal[t];
            BoneUploadRanges.Append(Local.BoneUploadRanges);

            // Each worker numbered its samples from zero, so they are rebased onto the merged list.
            const uint32 SampleBase = (uint32)GpuPoseSamples.Num();
            for (FGpuPoseRequest Request : Local.GpuPoseRequests)
            {
                Request.FirstSample += SampleBase;
                GpuPoseRequests.Add(Request);
            }
            GpuPoseSamples.Append(Local.GpuPoseSamples);
        }

        FSceneBatchRegistry& Registry = ScenePrimitives.GetBatches();
        const uint32 NumBatches = Registry.Num();

        DrawCommands.Reset();

        for (uint32 b = 0; b < NumBatches; ++b)
        {
            const FSceneBatchRegistry::FBatch& Batch = Registry.Get(b);

            FMeshDrawCommand& Cmd = DrawCommands.EmplaceGetRef();
            Cmd.VertexShader                   = Batch.VertexShader;
            Cmd.MeshShaderShadow               = Batch.MeshShaderShadow;
            Cmd.MeshShaderBase                 = Batch.MeshShaderBase;
            Cmd.PixelShader                    = Batch.PixelShader;
            Cmd.VisBufferMeshShader            = Batch.VisBufferMeshShader;
            Cmd.VisBufferMeshShaderMasked      = Batch.VisBufferMeshShaderMasked;
            Cmd.MaskedVisBufferPixelShader     = Batch.MaskedVisBufferPixelShader;
            Cmd.MeshShaderShadowMasked         = Batch.MeshShaderShadowMasked;
            Cmd.ShadowMaskedPixelShader        = Batch.ShadowMaskedPixelShader;
            Cmd.IndirectDrawOffset             = b;
            Cmd.DrawCount                      = 1u;
            Cmd.bTranslucent                   = Batch.Key.bTranslucent;
            Cmd.bMasked                        = Batch.Key.bMasked;
            Cmd.bAdditive                      = Batch.Key.bAdditive;
            Cmd.bModulate                      = Batch.Key.bModulate;
            Cmd.bWriteDepth                    = Batch.Key.bWriteDepth;
            Cmd.bTwoSided                      = Batch.Key.bTwoSided;
            Cmd.bWorldPositionOffset           = Batch.Key.bWorldPositionOffset;
            // From the bindings, not this frame's gather, since the GPU cull emits from the whole retained set and a wrong variant reads skinned vertices at the static stride.
            Cmd.bAnySkinned                    = (Batch.SkinnedRefCount != 0u) ? 1u : 0u;
            Cmd.bAnyStatic                     = (Batch.StaticRefCount != 0u) ? 1u : 0u;

            if (!Batch.Key.bTranslucent && Batch.RefCount != 0u)
            {
                for (const FSceneBatchRegistry::FDeferredMaterialSlot& MatSlot : Batch.DeferredMaterials)
                {
                    const uint8 Kinds = (uint8)((Batch.StaticRefCount != 0u ? 1u : 0u) | (Batch.SkinnedRefCount != 0u ? 2u : 0u));
                    DeferredMaterials.Emplace((uint32)MatSlot.MaterialIndex, MatSlot.DeferredShader, Kinds);
                }
            }
        }

        // The per-draw meshlet prefix is GPU-built by BuildDrawPrefix; the CPU only publishes the count.
        NumDrawsPerView   = NumBatches;

        // CullInstances emits skinned instances too; the CPU still owns their per-frame payload.
        {
            LUMINA_PROFILE_SECTION("Publish Skinned Frame Data");

            TVector<FSkinnedFrameData>& SkinnedData = Frame.Geometry.SkinnedFrameData;
            TVector<uint32>&            SkinnedIndices = Frame.Geometry.SkinnedIndices;

            SkinnedIndices.Reset();

            // Indexed by skinned index, so it is only as long as the most skinned instances ever bound at once.
            uint32 SkinnedExtent = (uint32)SkinnedData.Num();
            for (uint32 t = 0; t < NumThreads; ++t)
            {
                for (const FDeferredSkinnedSlot& Deferred : ThreadLocal[t].DeferredSlots)
                {
                    SkinnedExtent = Math::Max(SkinnedExtent, Deferred.SkinnedIndex + 1u);
                }
            }
            if ((uint32)SkinnedData.Num() < SkinnedExtent)
            {
                SkinnedData.SetNum(SkinnedExtent);
            }

            for (uint32 t = 0; t < NumThreads; ++t)
            {
                FThreadLocalDrawData& Local = ThreadLocal[t];
                if (!Local.bTouched)
                {
                    continue;
                }

                SkinnedIndices.Append(Local.SkinnedIndices);
                for (const FDeferredSkinnedSlot& Deferred : Local.DeferredSlots)
                {
                    PublishSkinnedFrameData(SkinnedData[Deferred.SkinnedIndex], Deferred.Data);
                    SkinnedIndices.Add(Deferred.SkinnedIndex);
                }
            }
        }

        // Every slot keeps a command so IndirectDrawOffset stays a slot index, but only bound batches draw.
        const uint32 NumEmittedBatches = (uint32)DrawCommands.Num();
        OpaqueDrawList.Reserve(NumEmittedBatches);
        TranslucentDrawList.Reserve(NumEmittedBatches);
        for (uint32 i = 0; i < NumEmittedBatches; ++i)
        {
            if (!Registry.IsLive(i))
            {
                continue;
            }

            const FMeshDrawCommand& Command = DrawCommands[i];
            if (Command.bTranslucent)
            {
                TranslucentDrawList.Add(i);
                const bool bUnordered = Command.bAdditive || Command.bModulate;
                Frame.Geometry.bHasUnorderedTranslucency |= bUnordered;
                Frame.Geometry.bHasBlendedTranslucency   |= !bUnordered;
            }
            else if (!DrawCommands[i].bMasked)
            {
                OpaqueDrawList.Add(i);
            }
        }

        // Masked batches go last, so the depth opaque geometry already wrote rejects their clip shader before it runs.
        for (uint32 i = 0; i < NumEmittedBatches; ++i)
        {
            if (Registry.IsLive(i) && !DrawCommands[i].bTranslucent && DrawCommands[i].bMasked)
            {
                OpaqueDrawList.Add(i);
            }
        }
    }
}
