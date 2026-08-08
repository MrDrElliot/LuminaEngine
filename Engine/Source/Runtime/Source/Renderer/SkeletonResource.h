#pragma once

#include "Lumina.h"
#include "Containers/Array.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Utils/NonCopyable.h"

namespace Lumina
{
    /** The bone hierarchy a skinned mesh poses against. Owned by the skeleton asset, not by the mesh. */
    struct RUNTIME_API FSkeletonResource : INonCopyable
    {
        struct FBoneInfo
        {
            FName    Name;
            int32    ParentIndex;        // -1 for root bone
            FMatrix4 InvBindMatrix;
            FMatrix4 LocalTransform;     // relative to parent

            friend FArchive& operator << (FArchive& Ar, FBoneInfo& Data)
            {
                Ar << Data.Name;
                Ar << Data.ParentIndex;
                Ar << Data.InvBindMatrix;
                Ar << Data.LocalTransform;
                return Ar;
            }
        };

        FName                   Name;
        TVector<FBoneInfo>      Bones;
        THashMap<FName, int32>  BoneNameToIndex;

        TVector<FVector3>       BindLocalTranslations;
        TVector<FQuat>          BindLocalRotations;
        TVector<FVector3>       BindLocalScales;
        uint32                  BindPoseGeneration = 0;

        // Transient import-dialog flag; not serialized.
        bool                    bShouldImport = true;

        FORCEINLINE int32 GetNumBones() const { return (int32)Bones.size(); }

        FORCEINLINE bool HasBindPoseCache() const
        {
            return !Bones.empty() && BindLocalRotations.size() == Bones.size();
        }

        void BuildBindPoseCache();

        FORCEINLINE int32 FindBoneIndex(const FName& BoneName) const
        {
            auto It = BoneNameToIndex.find(BoneName);
            return It != BoneNameToIndex.end() ? It->second : INDEX_NONE;
        }

        FORCEINLINE bool IsBoneIndexValid(int32 BoneIndex) const
        {
            return BoneIndex >= 0 && BoneIndex < GetNumBones();
        }

        FORCEINLINE const FBoneInfo& GetBone(int32 BoneIndex) const { return Bones[BoneIndex]; }
        FORCEINLINE FBoneInfo&       GetBone(int32 BoneIndex)       { return Bones[BoneIndex]; }

        FORCEINLINE const FBoneInfo* GetParentBone(int32 BoneIndex) const
        {
            if (!IsBoneIndexValid(BoneIndex))
            {
                return nullptr;
            }

            const int32 ParentIdx = Bones[BoneIndex].ParentIndex;
            return ParentIdx >= 0 ? &Bones[ParentIdx] : nullptr;
        }

        TVector<int32> GetChildBones(int32 BoneIndex) const
        {
            TVector<int32> Children;
            for (int32 i = 0; i < GetNumBones(); ++i)
            {
                if (Bones[i].ParentIndex == BoneIndex)
                {
                    Children.push_back(i);
                }
            }
            return Children;
        }

        TVector<int32> GetRootBones() const
        {
            TVector<int32> Roots;
            for (int32 i = 0; i < GetNumBones(); ++i)
            {
                if (Bones[i].ParentIndex < 0)
                {
                    Roots.push_back(i);
                }
            }
            return Roots;
        }

        friend FArchive& operator << (FArchive& Ar, FSkeletonResource& Data)
        {
            Ar << Data.Name;
            Ar << Data.Bones;
            Ar << Data.BoneNameToIndex;

            Data.BuildBindPoseCache();

            return Ar;
        }
    };
}
