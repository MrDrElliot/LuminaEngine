#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "GpuPoseEvaluation.h"

#include "Animation/AnimGpuClip.h"
#include "Renderer/ShaderLibrary.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    namespace
    {
        // One workgroup per character and one thread per bone, so a skeleton must fit a group.
        constexpr uint32 kGpuPoseGroupSize = 128;

        // Dispatches cap a grid dimension, so characters past it wrap into the second one.
        constexpr uint32 kGpuPoseMaxGroupsX = 65535;

        template <typename T>
        RHI::TGPUSpan<T> SpanOf(const TGpuPosePool<T>& Pool)
        {
            return { Pool.Buffer, (uint32)Pool.Data.Num() };
        }
    }

    uint32 FGpuPoseRegistry::ResolveSkeleton(const FSkeletonResource& Skeleton)
    {
        if (const uint32* Found = SkeletonIndex.Find(&Skeleton))
        {
            return *Found;
        }

        const int32 NumBones = Skeleton.GetNumBones();
        FGpuPoseSkeleton Record;
        Record.BoneBase = (uint32)BoneInfo.Data.Num();
        Record.NumBones = (uint32)NumBones;

        // Parents can be listed after their children, so depths settle by repeated passes rather than one ordered walk.
        TVector<int32> Depth;
        Depth.Init(-1, NumBones);
        bool bSettled = false;
        while (!bSettled)
        {
            bSettled = true;
            for (int32 b = 0; b < NumBones; ++b)
            {
                const int32 Parent = Skeleton.Bones[b].ParentIndex;
                int32 Next = 0;
                if (Parent >= 0 && Parent < NumBones)
                {
                    Next = Depth[Parent] < 0 ? -1 : Depth[Parent] + 1;
                }
                if (Next != Depth[b])
                {
                    Depth[b] = Next;
                    bSettled = false;
                }
            }
        }

        for (int32 b = 0; b < NumBones; ++b)
        {
            const int32 Parent = Skeleton.Bones[b].ParentIndex;
            BoneInfo.Data.Emplace(Parent < 0 || Parent >= NumBones ? -1 : Parent, Math::Max(Depth[b], 0));
            Record.MaxDepth = Math::Max(Record.MaxDepth, (uint32)Math::Max(Depth[b], 0));

            const FMatrix4& M = Skeleton.Bones[b].InvBindMatrix;
            for (int32 r = 0; r < 3; ++r)
            {
                InvBind.Data.Emplace(M[0][r], M[1][r], M[2][r], M[3][r]);
            }

            const FQuat&    R = Skeleton.BindLocalRotations[b];
            const FVector3& T = Skeleton.BindLocalTranslations[b];
            BindLocal.Data.Emplace(R.x, R.y, R.z, R.w);
            BindLocal.Data.Emplace(T, 0.0f);
        }

        const uint32 Index = (uint32)Skeletons.Data.Num();
        Skeletons.Data.Add(Record);
        SkeletonIndex.FindOrAdd(&Skeleton) = Index;
        return Index;
    }

    uint32 FGpuPoseRegistry::ResolveClip(const CAnimation* Clip, FSkeletonResource& Skeleton)
    {
        if (Clip == nullptr)
        {
            return kGpuPoseBindClip;
        }

        THashMap<const CAnimation*, uint32>& SkeletonClips = ClipIndexBySkeleton.FindOrAdd(&Skeleton);
        if (const uint32* Found = SkeletonClips.Find(Clip))
        {
            return *Found;
        }

        // A clip the GPU cannot play is remembered as the bind pose, so it is not baked again every frame.
        FAnimGpuClip Baked;
        uint32 Index = kGpuPoseBindClip;
        if (Anim::BakeGpuClip(*Clip, Skeleton, Baked))
        {
            FGpuPoseClip Record;
            Record.FrameBase = (uint32)Rotations.Data.Num();
            Record.NumFrames = Baked.NumFrames;
            Record.NumBones  = Baked.NumBones;
            Record.Duration  = Baked.Duration;
            Rotations.Data.Append(Baked.Rotations);
            Translations.Data.Append(Baked.Translations);
            Index = (uint32)Clips.Data.Num();
            Clips.Data.Add(Record);
        }
        SkeletonClips.FindOrAdd(Clip) = Index;
        return Index;
    }

    void FGpuPoseRegistry::Release()
    {
        Clips.Buffer.Release();
        Rotations.Buffer.Release();
        Translations.Buffer.Release();
        Skeletons.Buffer.Release();
        BoneInfo.Buffer.Release();
        InvBind.Buffer.Release();
        BindLocal.Buffer.Release();
        *this = FGpuPoseRegistry();
    }

    void FDefaultSceneRenderer::PrepareGpuPoses(RHI::FCmdListH CL, const FFrameData& Frame)
    {
        FGpuPoseRegistry& Registry = GpuPoses;
        Registry.Jobs.Reset();
        Registry.Samples.Reset();

        const TVector<FGpuPoseRequest>&     Requests = Frame.Geometry.GpuPoseRequests;
        const TVector<FAnimWeightedSample>& Weighted = Frame.Geometry.GpuPoseSamples;
        if (Requests.IsEmpty())
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();
        for (const FGpuPoseRequest& Request : Requests)
        {
            if (Request.Skeleton == nullptr || (uint32)Request.Skeleton->GetNumBones() > kGpuPoseGroupSize)
            {
                continue;
            }

            FGpuPoseJob& Job = Registry.Jobs.EmplaceGetRef();
            Job.ArenaBase    = Request.BoneSlice;
            Job.Skeleton     = Registry.ResolveSkeleton(*Request.Skeleton);
            Job.FirstSample  = (uint32)Registry.Samples.Num();
            Job.NumSamples   = Request.NumSamples;
            Job.RootLockBone = Request.RootLockBone;
            for (uint32 s = 0; s < Request.NumSamples; ++s)
            {
                const FAnimWeightedSample& Source = Weighted[(int32)(Request.FirstSample + s)];
                FGpuPoseSample& Sample = Registry.Samples.EmplaceGetRef();
                Sample.Clip   = Registry.ResolveClip(Source.Clip, *Request.Skeleton);
                Sample.Time   = Source.Time;
                Sample.Weight = Source.Weight;
            }
        }

        UploadGpuPosePools(CL);
    }

    void FDefaultSceneRenderer::UploadGpuPosePools(RHI::FCmdListH CL)
    {
        // Pools only grow, so each upload sends just the tail baked since the last one, and a grown buffer keeps its prefix.
        const auto Upload = [&]<typename T>(TGpuPosePool<T>& Pool)
        {
            const int32 Count = Pool.Data.Num();
            if (Count == Pool.Uploaded && Pool.Buffer.Gpu != 0)
            {
                return;
            }
            ReserveBuffer(CL, Pool.Buffer, (uint64)Math::Max(Count, 1) * sizeof(T), false, true);
            if (Count > Pool.Uploaded)
            {
                const uint64 Offset = (uint64)Pool.Uploaded * sizeof(T);
                WriteBuffer(CL, Pool.Buffer.Gpu + Offset, Pool.Data.GetData() + Pool.Uploaded, (uint64)(Count - Pool.Uploaded) * sizeof(T));
            }
            Pool.Uploaded = Count;
        };

        FGpuPoseRegistry& Registry = GpuPoses;
        Upload(Registry.Clips);
        Upload(Registry.Rotations);
        Upload(Registry.Translations);
        Upload(Registry.Skeletons);
        Upload(Registry.BoneInfo);
        Upload(Registry.InvBind);
        Upload(Registry.BindLocal);
    }

    void FDefaultSceneRenderer::EvaluateGpuPoses(RHI::FCmdListH CL)
    {
        const FGpuPoseRegistry& Registry = GpuPoses;
        const RHI::FGPUAllocation Arena = GetBoneArena();
        if (Registry.Jobs.IsEmpty() || Arena.Gpu == 0)
        {
            return;
        }

        const FShaderH Shader = FShaderLibrary::Get("AnimEvaluatePose.slang");
        if (!Shader)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Evaluate GPU Poses");

        struct FGpuPoseArgs
        {
            RHI::TGPUSpan<FGpuPoseJob>      Jobs;
            RHI::TGPUSpan<FGpuPoseSample>   Samples;
            RHI::TGPUSpan<FGpuPoseClip>     Clips;
            RHI::TGPUSpan<FUIntVector2>     Rotations;
            RHI::TGPUSpan<FVector4>         Translations;
            RHI::TGPUSpan<FGpuPoseSkeleton> Skeletons;
            RHI::TGPUSpan<FIntVector2>      BoneInfo;
            RHI::TGPUSpan<FVector4>         InvBind;
            RHI::TGPUSpan<FVector4>         BindLocal;
            RHI::TGPUSpan<FBoneTransform>   OutBones;
        } Args;
        static_assert(sizeof(FGpuPoseArgs) == 160, "FGpuPoseArgs must match AnimEvaluatePose.slang.");

        Args.Jobs         = RHI::TGPUSpan<FGpuPoseJob>(RHI::CopyTransientArray(Registry.Jobs.GetData(), Registry.Jobs.Num()));
        Args.Samples      = RHI::TGPUSpan<FGpuPoseSample>(RHI::CopyTransientArray(Registry.Samples.GetData(), Math::Max(Registry.Samples.Num(), 1)));
        Args.Clips        = SpanOf(Registry.Clips);
        Args.Rotations    = SpanOf(Registry.Rotations);
        Args.Translations = SpanOf(Registry.Translations);
        Args.Skeletons    = SpanOf(Registry.Skeletons);
        Args.BoneInfo     = SpanOf(Registry.BoneInfo);
        Args.InvBind      = SpanOf(Registry.InvBind);
        Args.BindLocal    = SpanOf(Registry.BindLocal);
        Args.OutBones     = RHI::TGPUSpan<FBoneTransform>(Arena);

        const uint32 NumJobs = (uint32)Registry.Jobs.Num();
        const uint32 GroupsX = Math::Min(NumJobs, kGpuPoseMaxGroupsX);
        const uint32 GroupsY = (NumJobs + kGpuPoseMaxGroupsX - 1) / kGpuPoseMaxGroupsX;
        DispatchCompute(CL, Shader, Args, GroupsX, GroupsY, 1u);

        // The skinned meshlet bounds, the skinning pass and the materials all read these bones next.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::AllCommands,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }
}
