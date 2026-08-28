#pragma once

#include "Animation/AnimCompression.h"
#include "Animation/AnimNotify.h"
#include "Animation/AnimSyncTrack.h"
#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Core/Math/AABB.h"
#include "Core/Object/InstancedStruct.h"
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

    // An additive clip holds a delta against a base pose, which the graph layers onto another pose.
    REFLECT()
    enum class EAdditiveAnimType : uint8
    {
        None,
        LocalSpace,
        MeshSpace,
    };

    // What an additive clip subtracts from itself to get its delta.
    REFLECT()
    enum class EAdditiveBasePoseType : uint8
    {
        RefPose,
        AnimFrame,
        AnimScaled,
    };

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

        // Optional typed notify that runs its own code; empty leaves the entry name-only.
        TInstancedStruct<SAnimNotify> Notify;

        friend FArchive& operator << (FArchive& Ar, FAnimationNotify& Data)
        {
            Ar << Data.NotifyName;
            Ar << Data.Time;
            Ar << Data.NotifyTrack;
            Ar << Data.Color;

            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_NOTIFY_OBJECTS)
            {
                Ar << Data.Notify;
            }
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

        // Optional typed notify that runs its own code; empty leaves the entry name-only.
        TInstancedStruct<SAnimNotifyState> Notify;

        friend FArchive& operator << (FArchive& Ar, FAnimationNotifyState& Data)
        {
            Ar << Data.NotifyName;
            Ar << Data.StartTime;
            Ar << Data.EndTime;
            Ar << Data.NotifyTrack;
            Ar << Data.Color;

            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_NOTIFY_OBJECTS)
            {
                Ar << Data.Notify;
            }
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

        // Import-time staging for Compressed, and never written: a loaded clip has none.
        TVector<FAnimationChannel> Channels;

        TVector<FAnimationNotify> Notifies;
        TVector<FAnimationNotifyState> NotifyStates;
        TVector<FAnimationCurve> Curves;

        // The only pose data the sampler reads.
        FCompressedAnimData Compressed;

        // Notify lanes in display order; persisted separately so empty tracks and ordering survive
        // save/reload. A notify references its lane by name (NotifyTrack).
        TVector<FName> NotifyTracks;

        // Notify lane whose entries mark the cycle points a synchronized blend lines up on.
        FName SyncTrackName;

        // Reflected from the named lane, never serialized.
        FSyncTrack SyncTrack;

        RUNTIME_API void RebuildSyncTrack();

        // Resolved once per (skeleton, bind generation) and never mutated after, so sampling inside ParallelFor needs no lock.
        struct FResolvedSkeleton
        {
            const FSkeletonResource* Skeleton = nullptr;
            uint32 Generation = 0;

            TVector<int32> CompressedBones;

            // Inverse of CompressedBones, so sampling one bone never scans.
            TVector<int32> SkeletonToCompressed;
        };

        RUNTIME_API const FResolvedSkeleton* GetResolvedSkeleton(const FSkeletonResource* Skeleton);

        RUNTIME_API void InvalidateResolvedSkeletons();

        friend FArchive& operator << (FArchive& Ar, FAnimationResource& Data)
        {
            Ar << Data.Name;
            Ar << Data.Duration;

            if (Ar.GetFileVersion() < (int32)ELuminaEngineVersion::ANIM_CHANNELS_DROPPED)
            {
                Ar << Data.Channels;
            }

            Ar << Data.Notifies;
            Ar << Data.NotifyStates;
            Ar << Data.NotifyTracks;

            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_CURVES)
            {
                Ar << Data.Curves;
            }

            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_COMPRESSED_TRACKS)
            {
                Ar << Data.Compressed;
            }

            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_SYNC_TRACK)
            {
                Ar << Data.SyncTrackName;
            }

            Data.RebuildSyncTrack();
            Data.InvalidateResolvedSkeletons();

            return Ar;
        }

    private:

        std::atomic<const FResolvedSkeleton*> ActiveResolvedSkeleton{ nullptr };
        FMutex ResolveMutex;
        TVector<TUniquePtr<FResolvedSkeleton>> ResolvedSkeletons;
    };
    
    
    REFLECT()
    class RUNTIME_API CAnimation : public CObject
    {
        GENERATED_BODY()
        
        friend class CMeshImporter;
        
    public:

        CAnimation();

        void Serialize(FArchive& Ar) override;
        
        bool IsAsset() const override { return true; }
        
        /** Writes (Global * InvBind) per bone; bones without channels keep their bind-pose local transform. */
        void SamplePose(float Time, FSkeletonResource* RESTRICT InSkeleton, TVector<FMatrix4>& RESTRICT OutBoneTransforms) const;

        /** Samples the clip into a local-space TRS pose, or into its additive delta when the clip is additive. */
        // MaxBones < 0 samples every animated bone; otherwise only channels targeting bones below
        // MaxBones are sampled and the rest stay at bind pose (skeleton LOD for distant meshes).
        void SampleLocalPose(float Time, FSkeletonResource* RESTRICT InSkeleton, FPose& RESTRICT OutPose, int32 MaxBones = -1) const;

        /** Samples the authored pose as-is, ignoring the additive settings. */
        void SampleRawLocalPose(float Time, FSkeletonResource* RESTRICT InSkeleton, FPose& RESTRICT OutPose, int32 MaxBones = -1) const;

        /** Samples the clip's delta against its configured base pose. Yields an identity delta on a non-additive clip. */
        void SampleAdditiveDelta(float Time, FSkeletonResource* RESTRICT InSkeleton, FPose& RESTRICT OutDelta, int32 MaxBones = -1) const;

        /** Samples the base pose an additive clip is authored against, at the clip time Time maps to. */
        void SampleAdditiveBasePose(float Time, FSkeletonResource* RESTRICT InSkeleton, FPose& RESTRICT OutBase, int32 MaxBones = -1) const;

        bool IsAdditive() const { return AdditiveAnimType != EAdditiveAnimType::None; }

        /** The base clip an additive delta is taken against, or null when the base is the skeleton's ref pose. */
        CAnimation* GetAdditiveBaseAnimation() const;

        /** Time in the base clip that a playhead of Time in this clip maps to. */
        float GetAdditiveBaseTime(float Time) const;

        /** Samples a single bone's local TRS at Time, falling back to its bind-pose value for untouched channels. */
        void SampleBoneLocal(float Time, FSkeletonResource* RESTRICT InSkeleton, int32 BoneIndex,
                             FVector3& OutT, FQuat& OutR, FVector3& OutS) const;

        float GetDuration() const { return AnimationResource->Duration; }
        FAnimationResource* GetAnimationResource() const { return AnimationResource.get(); }

        const TVector<FAnimationNotify>& GetNotifies() const { return AnimationResource->Notifies; }
        const TVector<FAnimationNotifyState>& GetNotifyStates() const { return AnimationResource->NotifyStates; }
        bool HasNotifies() const { return !AnimationResource->Notifies.empty() || !AnimationResource->NotifyStates.empty(); }

        const FSyncTrack& GetSyncTrack() const { return AnimationResource->SyncTrack; }

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

        /** Makes the clip a delta layered onto another pose: Local Space subtracts per bone, Mesh Space in component space. */
        PROPERTY(Editable, Category = "Additive")
        EAdditiveAnimType AdditiveAnimType = EAdditiveAnimType::None;

        /** What the delta is measured against: the skeleton's ref pose, one frame of a base clip, or that clip followed over time. */
        PROPERTY(Editable, Category = "Additive", EditCondition = "AdditiveAnimType != None", EditConditionHides)
        EAdditiveBasePoseType AdditiveBasePoseType = EAdditiveBasePoseType::RefPose;

        /** Clip the base pose is sampled from. Leaving it empty falls back to the skeleton's ref pose. */
        PROPERTY(Editable, Category = "Additive",
                 EditCondition = "AdditiveAnimType != None && AdditiveBasePoseType != RefPose", EditConditionHides)
        TObjectPtr<CAnimation> AdditiveBaseAnimation;

        /** Time in the base clip that Anim Frame freezes on. */
        PROPERTY(Editable, Category = "Additive", Units = "s", ClampMin = 0.0f,
                 EditCondition = "AdditiveAnimType != None && AdditiveBasePoseType == AnimFrame", EditConditionHides)
        float AdditiveBaseFrameTime = 0.0f;

    private:
        
        TUniquePtr<FAnimationResource> AnimationResource;
    };
}
