#pragma once

#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Core/Math/AABB.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Threading/Thread.h"
#include "Core/Versioning/CoreVersion.h"
#include "Memory/SmartPtr.h"
#include <atomic>
#include "Renderer/SkeletonResource.h"
#include "Animation.generated.h"

namespace Lumina
{
    class CSkeleton;
    struct FSkeletonResource;
    struct FPose;

    struct FAnimationChannel
    {
        enum class ETargetPath : uint8
        {
            Translation,
            Rotation,
            Scale,
            Weights
        };
    
        FName TargetBone; 
        ETargetPath TargetPath;
        TVector<float> Timestamps;
        TVector<FVector3> Translations;
        TVector<FQuat> Rotations;
        TVector<FVector3> Scales;
        
        friend FArchive& operator << (FArchive& Ar, FAnimationChannel& Data)
        {
            Ar << Data.TargetBone;
            Ar << Data.TargetPath;
            Ar << Data.Timestamps;
            Ar << Data.Translations;
            Ar << Data.Rotations;
            Ar << Data.Scales;
            
            return Ar;
        }
    };
    
    struct FAnimationNotify
    {
        FName NotifyName;
        float Time;
        FName NotifyTrack;
        FVector4 Color;
    
        friend FArchive& operator << (FArchive& Ar, FAnimationNotify& Data)
        {
            Ar << Data.NotifyName;
            Ar << Data.Time;
            Ar << Data.NotifyTrack;
            Ar << Data.Color;
            return Ar;
        }
    };
    
    struct FAnimationNotifyState
    {
        FName NotifyName;
        float StartTime;
        float EndTime;
        FName NotifyTrack;
        FVector4 Color;
    
        friend FArchive& operator << (FArchive& Ar, FAnimationNotifyState& Data)
        {
            Ar << Data.NotifyName;
            Ar << Data.StartTime;
            Ar << Data.EndTime;
            Ar << Data.NotifyTrack;
            Ar << Data.Color;
            return Ar;
        }
    };
        
    // A named float track authored on the clip and sampled by the clip's playhead. Curves ride the
    // animation graph alongside the pose: blends lerp them, so a value follows whatever the pose does.
    struct FAnimationCurve
    {
        FName Name;
        SKeyedCurve Curve;
        FVector4 Color = FVector4(0.4f, 0.75f, 0.95f, 1.0f);

        friend FArchive& operator << (FArchive& Ar, FAnimationCurve& Data)
        {
            Ar << Data.Name;
            Ar << Data.Curve;
            Ar << Data.Color;
            return Ar;
        }
    };

    struct FAnimationResource
    {
        FName Name;
        float Duration;
        TVector<FAnimationChannel> Channels;
        TVector<FAnimationNotify> Notifies;
        TVector<FAnimationNotifyState> NotifyStates;
        TVector<FAnimationCurve> Curves;

        // Notify lanes in display order; persisted separately so empty tracks and ordering survive
        // save/reload. A notify references its lane by name (NotifyTrack).
        TVector<FName> NotifyTracks;

        // Channel -> skeleton bone indices resolved once per (skeleton, bind generation) instead of a
        // per-channel FName hash lookup every sample. Sets are immutable once published so readers can
        // hold one without locking; sampling runs inside ParallelFor.
        struct FResolvedChannelSet
        {
            const FSkeletonResource* Skeleton = nullptr;
            uint32 Generation = 0;
            TVector<int32> BoneIndices;
        };

        const FResolvedChannelSet* GetResolvedChannelSet(const FSkeletonResource* Skeleton);

        void InvalidateResolvedChannelSets();

        friend FArchive& operator << (FArchive& Ar, FAnimationResource& Data)
        {
            Ar << Data.Name;
            Ar << Data.Duration;
            Ar << Data.Channels;
            Ar << Data.Notifies;
            Ar << Data.NotifyStates;
            Ar << Data.NotifyTracks;

            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_CURVES)
            {
                Ar << Data.Curves;
            }

            Data.InvalidateResolvedChannelSets();

            return Ar;
        }

    private:

        std::atomic<const FResolvedChannelSet*> ActiveChannelSet{ nullptr };
        FMutex ChannelSetMutex;
        TVector<TUniquePtr<FResolvedChannelSet>> ChannelSets;
    };
    
    
    REFLECT()
    class RUNTIME_API CAnimation : public CObject
    {
        GENERATED_BODY()
        
        friend class CMeshImporter;
        
    public:

        CAnimation();

        using Super::Serialize;
        void Serialize(FArchive& Ar) override;
        
        bool IsAsset() const override { return true; }
        
        /** Writes (Global * InvBind) per bone; bones without channels keep their bind-pose local transform. */
        void SamplePose(float Time, FSkeletonResource* RESTRICT InSkeleton, TVector<FMatrix4>& RESTRICT OutBoneTransforms) const;

        /** Samples the clip into a local-space TRS pose; bones without channels keep their bind-pose local transform. */
        // MaxBones < 0 samples every animated bone; otherwise only channels targeting bones below
        // MaxBones are sampled and the rest stay at bind pose (skeleton LOD for distant meshes).
        void SampleLocalPose(float Time, FSkeletonResource* RESTRICT InSkeleton, FPose& RESTRICT OutPose, int32 MaxBones = -1) const;

        /** Samples a single bone's local TRS at Time, falling back to its bind-pose value for untouched channels. */
        void SampleBoneLocal(float Time, FSkeletonResource* RESTRICT InSkeleton, int32 BoneIndex,
                             FVector3& OutT, FQuat& OutR, FVector3& OutS) const;

        float GetDuration() const { return AnimationResource->Duration; }
        FAnimationResource* GetAnimationResource() const { return AnimationResource.get(); }

        const TVector<FAnimationNotify>& GetNotifies() const { return AnimationResource->Notifies; }
        const TVector<FAnimationNotifyState>& GetNotifyStates() const { return AnimationResource->NotifyStates; }
        bool HasNotifies() const { return !AnimationResource->Notifies.empty() || !AnimationResource->NotifyStates.empty(); }

        const TVector<FAnimationCurve>& GetCurves() const { return AnimationResource->Curves; }
        bool HasCurves() const { return !AnimationResource->Curves.empty(); }

        int32 FindCurveIndex(const FName& CurveName) const;

        /** Curve value at Time, or Default when the clip has no curve of that name. */
        float EvaluateCurve(const FName& CurveName, float Time, float Default = 0.0f) const;

        PROPERTY(Editable, Category = "Skeleton")
        TObjectPtr<CSkeleton> Skeleton;

        /**
         * Extract the root bone's motion each frame and use it to drive the owning entity's transform; the
         * root is stripped from the in-place pose so the mesh stays centered. Ignored when the root is locked.
         */
        PROPERTY(Editable, Category = "Root Motion")
        bool bEnableRootMotion = false;

        /** Pin the root bone to its bind pose so the mesh never drifts (in-place). Wins over bEnableRootMotion. */
        PROPERTY(Editable, Category = "Root Motion")
        bool bLockRootMotion = false;

        /** Bone driving root motion; empty resolves to the first root bone (ParentIndex < 0). */
        PROPERTY(Editable, Category = "Root Motion")
        FName RootBoneName;

    private:
        
        TUniquePtr<FAnimationResource> AnimationResource;
    };
}
