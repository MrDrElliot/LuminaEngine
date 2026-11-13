#include "RuntimePCH.h"
#include "Animation/BindPose.h"
#include "BoneLOD.h"
#include "Renderer/MeshData.h"
#include "Renderer/MeshQuantization.h"
#include "Renderer/SkeletonResource.h"
#include <cfloat>

namespace Lumina::BoneLOD
{
    namespace
    {
        // A frozen bone is assumed to have swung this far, which moves a point at distance d by the chord 2d sin(a/2).
        constexpr float kAssumedSwingRadians = 1.0f;

        constexpr int32 kInfluencesPerVertex = 4;

        float SwingChord()
        {
            return 2.0f * Math::Sin(kAssumedSwingRadians * 0.5f);
        }

        // Errors within a factor of two share a band, so the sort keeps the asset's order inside it and clip data stays read in order.
        int32 ErrorBand(float Error)
        {
            constexpr float kSmallestError = 1e-5f;
            constexpr int32 kLargestBand   = 64;
            if (Error >= FLT_MAX)
            {
                return kLargestBand;
            }
            return Math::Min((int32)Math::Floor(Math::Log2(Math::Max(Error, kSmallestError))), kLargestBand - 1);
        }

        FVector3 JointPosition(const FSkeletonResource& Skeleton, int32 Bone)
        {
            return FVector3(AnimPose::GetBindGlobalMatrix(&Skeleton, Bone)[3]);
        }

        // Freezing a bone moves every vertex its subtree carries, by that subtree's share of the vertex.
        class FVertexErrorAccumulator
        {
        public:

            FVertexErrorAccumulator(const FSkeletonResource& InSkeleton, TVector<float>& InErrors)
                : Skeleton(InSkeleton)
                , Errors(InErrors)
                , NumBones(InSkeleton.GetNumBones())
                , Chord(SwingChord())
            {
                Joints.SetNum(NumBones);
                for (int32 Bone = 0; Bone < NumBones; ++Bone)
                {
                    Joints[Bone] = JointPosition(Skeleton, Bone);
                }
                SubtreeWeight.Init(0.0f, NumBones);
            }

            void Add(const FVector3& Position, const int32 (&Joint)[kInfluencesPerVertex], const uint8 (&Weight)[kInfluencesPerVertex])
            {
                for (int32 Influence = 0; Influence < kInfluencesPerVertex; ++Influence)
                {
                    const float Share = (float)Weight[Influence] / 255.0f;
                    if (Share <= 0.0f || Joint[Influence] < 0 || Joint[Influence] >= NumBones)
                    {
                        continue;
                    }
                    for (int32 Bone = Joint[Influence]; Bone >= 0; Bone = Skeleton.GetBone(Bone).ParentIndex)
                    {
                        if (SubtreeWeight[Bone] == 0.0f)
                        {
                            Touched.Add(Bone);
                        }
                        SubtreeWeight[Bone] += Share;
                    }
                }

                for (int32 Bone : Touched)
                {
                    const float Displacement = SubtreeWeight[Bone] * Math::Length(Position - Joints[Bone]) * Chord;
                    Errors[Bone] = Math::Max(Errors[Bone], Displacement);
                    SubtreeWeight[Bone] = 0.0f;
                }
                Touched.Reset();
            }

            const TVector<FVector3>& GetJoints() const { return Joints; }

        private:

            const FSkeletonResource& Skeleton;
            TVector<float>&          Errors;
            TVector<FVector3>        Joints;
            TVector<float>           SubtreeWeight;
            TVector<int32>           Touched;
            int32                    NumBones;
            float                    Chord;
        };

        void AccumulateSourceVertices(const FMeshResource& Mesh, FVertexErrorAccumulator& Accumulator)
        {
            const size_t NumVertices = Math::Min(Mesh.Positions.Num(), Math::Min(Mesh.JointIndices.Num(), Mesh.JointWeights.Num()));
            for (int32 Vertex = 0, Limit = IntCastChecked<int32>(NumVertices); Vertex < Limit; ++Vertex)
            {
                int32 Joints[kInfluencesPerVertex];
                uint8 Weights[kInfluencesPerVertex];
                for (int32 Influence = 0; Influence < kInfluencesPerVertex; ++Influence)
                {
                    Joints[Influence]  = (int32)Mesh.JointIndices[Vertex][Influence];
                    Weights[Influence] = Mesh.JointWeights[Vertex][Influence];
                }
                Accumulator.Add(Mesh.Positions[Vertex], Joints, Weights);
            }
        }

        // The finest LOD covers every vertex, and coarser ones only re-index the same skin data.
        void AccumulateMeshletVertices(const FMeshletData& Data, FVertexErrorAccumulator& Accumulator)
        {
            const int32 NumMeshlets = Math::Min(Data.Meshlets.Num(), Data.MeshletBonePalettes.Num());
            for (int32 MeshletIndex = 0; MeshletIndex < NumMeshlets; ++MeshletIndex)
            {
                const FMeshlet& Meshlet = Data.Meshlets[MeshletIndex];
                if (Meshlet.LODIndex != 0u)
                {
                    continue;
                }

                const FMeshletBonePalette& Palette = Data.MeshletBonePalettes[MeshletIndex];
                for (uint32 i = 0; i < Meshlet.VertexCount; ++i)
                {
                    const uint32 VertexIndex = Meshlet.VertexOffset + i;
                    if (VertexIndex >= static_cast<uint32>(Data.MeshletSkinnedVertices.Num()))
                    {
                        break;
                    }
                    const FMeshletSkinnedVertex& Vertex = Data.MeshletSkinnedVertices[VertexIndex];

                    int32 Joints[kInfluencesPerVertex];
                    uint8 Weights[kInfluencesPerVertex];
                    for (int32 Influence = 0; Influence < kInfluencesPerVertex; ++Influence)
                    {
                        const uint32 Slot = (Vertex.JointIndices >> (Influence * 8)) & 0xFFu;
                        const uint32 PaletteEntry = Palette.Offset + Slot;
                        Joints[Influence]  = (Slot < Palette.Count && PaletteEntry < static_cast<uint32>(Data.MeshletBoneIndices.Num()))
                            ? (int32)Data.MeshletBoneIndices[PaletteEntry]
                            : Constants::kIndexNone;
                        Weights[Influence] = (uint8)((Vertex.JointWeights >> (Influence * 8)) & 0xFFu);
                    }
                    Accumulator.Add(DecodeMeshPosition(Data.PositionGrid, Vertex.Position), Joints, Weights);
                }
            }
        }
    }

    TVector<float> ComputeBoneErrors(const FMeshResource& Mesh, const FSkeletonResource& Skeleton)
    {
        const int32 NumBones = Skeleton.GetNumBones();
        TVector<float> Errors(NumBones, 0.0f);
        if (NumBones == 0)
        {
            return Errors;
        }

        FVertexErrorAccumulator Accumulator(Skeleton, Errors);
        if (!Mesh.JointIndices.IsEmpty())
        {
            AccumulateSourceVertices(Mesh, Accumulator);
        }
        else
        {
            AccumulateMeshletVertices(Mesh.MeshletData, Accumulator);
        }

        // Unweighted bones still carry attachments, so a frozen bone also moves its child joints, and an unweighted leaf moves as far as it reaches.
        const TVector<FVector3>& Joints = Accumulator.GetJoints();
        const TVector<float> VertexErrors = Errors;
        const float Chord = SwingChord();
        TVector<uint8> HasChildren(NumBones, 0);
        for (int32 Bone = 0; Bone < NumBones; ++Bone)
        {
            const int32 Parent = Skeleton.GetBone(Bone).ParentIndex;
            if (Parent >= 0)
            {
                HasChildren[Parent] = 1;
            }
        }
        for (int32 Bone = 0; Bone < NumBones; ++Bone)
        {
            const int32 Parent = Skeleton.GetBone(Bone).ParentIndex;
            if (Parent < 0)
            {
                continue;
            }
            const float Reach = Math::Length(Joints[Bone] - Joints[Parent]) * Chord;
            Errors[Parent] = Math::Max(Errors[Parent], Reach);
            if (!HasChildren[Bone] && VertexErrors[Bone] == 0.0f)
            {
                Errors[Bone] = Math::Max(Errors[Bone], Reach);
            }
        }

        // A parent never cuts before its children; walked by ancestry because an imported skeleton may not be parents-first yet.
        for (int32 Bone = 0; Bone < NumBones; ++Bone)
        {
            const float Error = Errors[Bone];
            for (int32 Ancestor = Skeleton.GetBone(Bone).ParentIndex; Ancestor >= 0 && Errors[Ancestor] < Error;
                 Ancestor = Skeleton.GetBone(Ancestor).ParentIndex)
            {
                Errors[Ancestor] = Error;
            }
        }

        return Errors;
    }

    TVector<int32> SortBonesByError(const FSkeletonResource& Skeleton, const TVector<float>& BoneErrors)
    {
        const int32 NumBones = Skeleton.GetNumBones();
        const bool bHasErrors = BoneErrors.Num() == NumBones;

        TVector<TVector<int32>> Children(NumBones);
        auto Before = [&](int32 A, int32 B)
        {
            const int32 BandA = bHasErrors ? ErrorBand(BoneErrors[A]) : 0;
            const int32 BandB = bHasErrors ? ErrorBand(BoneErrors[B]) : 0;
            return BandA != BandB ? BandA > BandB : A < B;
        };
        auto After = [&](int32 A, int32 B) { return Before(B, A); };
        TVector<int32> Ready;

        for (int32 Bone = 0; Bone < NumBones; ++Bone)
        {
            const int32 Parent = Skeleton.GetBone(Bone).ParentIndex;
            if (Parent >= 0 && Parent < NumBones)
            {
                Children[Parent].Add(Bone);
            }
            else
            {
                Ready.Add(Bone);
                Algo::PushHeap(Ready.begin(), Ready.end(), After);
            }
        }

        // Only a bone whose parent is already placed can go next, so the order is parents-first whatever the source order was.
        TVector<int32> NewToOld;
        NewToOld.Reserve(NumBones);
        while (!Ready.IsEmpty())
        {
            Algo::PopHeap(Ready.begin(), Ready.end(), After);
            const int32 Bone = Ready.Pop();
            NewToOld.Add(Bone);
            for (int32 Child : Children[Bone])
            {
                Ready.Add(Child);
                Algo::PushHeap(Ready.begin(), Ready.end(), After);
            }
        }

        // A parent cycle leaves bones unreachable, and such a skeleton is better left as it came.
        if (NewToOld.Num() != NumBones)
        {
            NewToOld.SetNum(NumBones);
            for (int32 Bone = 0; Bone < NumBones; ++Bone)
            {
                NewToOld[Bone] = Bone;
            }
        }
        return NewToOld;
    }

    TVector<int32> ReorderSkeleton(FSkeletonResource& Skeleton, const TVector<int32>& NewToOld)
    {
        const int32 NumBones = Skeleton.GetNumBones();
        TVector<int32> OldToNew(NumBones, Constants::kIndexNone);
        if (NewToOld.Num() != NumBones)
        {
            for (int32 Bone = 0; Bone < NumBones; ++Bone)
            {
                OldToNew[Bone] = Bone;
            }
            return OldToNew;
        }

        for (int32 New = 0; New < NumBones; ++New)
        {
            OldToNew[NewToOld[New]] = New;
        }

        TVector<FSkeletonResource::FBoneInfo> Reordered;
        Reordered.Reserve(NumBones);
        Skeleton.BoneNameToIndex.Reset();
        for (int32 New = 0; New < NumBones; ++New)
        {
            FSkeletonResource::FBoneInfo Bone = Skeleton.Bones[NewToOld[New]];
            if (Bone.ParentIndex >= 0)
            {
                Bone.ParentIndex = OldToNew[Bone.ParentIndex];
            }
            Skeleton.BoneNameToIndex[Bone.Name] = New;
            Reordered.Add(Move(Bone));
        }
        Skeleton.Bones = Move(Reordered);

        Skeleton.BuildBindPoseCache();
        return OldToNew;
    }

    TUniquePtr<FBoneLODLayout> BuildLayout(const FMeshResource& Mesh, const FSkeletonResource& Skeleton, const TVector<int32>& AlwaysAnimated)
    {
        TUniquePtr<FBoneLODLayout> Layout = MakeUnique<FBoneLODLayout>();
        const int32 NumBones = Skeleton.GetNumBones();
        TVector<float> Errors = ComputeBoneErrors(Mesh, Skeleton);

        for (int32 Bone : AlwaysAnimated)
        {
            for (; Bone >= 0 && Bone < NumBones; Bone = Skeleton.GetBone(Bone).ParentIndex)
            {
                Errors[Bone] = FLT_MAX;
            }
        }

        const TVector<int32> NewToOld = SortBonesByError(Skeleton, Errors);
        bool bIdentity = true;
        for (int32 Bone = 0; Bone < NumBones && bIdentity; ++Bone)
        {
            bIdentity = NewToOld[Bone] == Bone;
        }
        if (bIdentity)
        {
            Layout->Errors = Move(Errors);
            return Layout;
        }

        Layout->PoseSkeleton = MakeUnique<FSkeletonResource>();
        Layout->PoseSkeleton->Name            = Skeleton.Name;
        Layout->PoseSkeleton->Bones           = Skeleton.Bones;
        Layout->PoseSkeleton->BoneNameToIndex = Skeleton.BoneNameToIndex;
        Layout->AssetToPose = ReorderSkeleton(*Layout->PoseSkeleton, NewToOld);
        Layout->PoseToAsset = NewToOld;
        Layout->Errors      = RemapBoneErrors(Errors, Layout->AssetToPose);
        return Layout;
    }

    TVector<float> RemapBoneErrors(const TVector<float>& BoneErrors, const TVector<int32>& OldToNew)
    {
        if (BoneErrors.Num() != OldToNew.Num())
        {
            return {};
        }
        TVector<float> Remapped(OldToNew.Num(), 0.0f);
        for (int32 Old = 0; Old < OldToNew.Num(); ++Old)
        {
            Remapped[OldToNew[Old]] = BoneErrors[Old];
        }
        return Remapped;
    }

    int32 CountActiveBones(const TVector<float>& BoneErrors, int32 NumBones, float Tolerance, int32 MinBones)
    {
        // Errors measured against another bone layout say nothing about this one.
        if (Tolerance <= 0.0f || BoneErrors.Num() != NumBones)
        {
            return NumBones;
        }

        const int32 Floor = Math::Min(Math::Max(MinBones, 1), NumBones);
        for (int32 Bone = NumBones - 1; Bone >= Floor; --Bone)
        {
            if (BoneErrors[Bone] > Tolerance)
            {
                return Bone + 1;
            }
        }
        return Floor;
    }
}
