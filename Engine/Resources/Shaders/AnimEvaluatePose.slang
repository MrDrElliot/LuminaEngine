#include "Includes/SceneGlobals.slang"

// Poses the GPU crowd tier's characters from their weighted clip samples, one workgroup a character and one thread a bone.

#define LOCAL_SIZE_X 128

static const uint kBindClip   = 0xFFFFFFFFu;
static const uint kMaxGroupsX = 65535u;

struct FGpuPoseJob
{
    uint ArenaBase;
    uint Skeleton;
    uint FirstSample;
    uint NumSamples;
    int  RootLockBone;
    uint _Pad0;
    uint _Pad1;
    uint _Pad2;
};

struct FGpuPoseSample
{
    uint  Clip;
    float Time;
    float Weight;
    uint  _Pad;
};

// A baked clip's first frame-bone entry, frame-major with a frame's bones contiguous.
struct FGpuPoseClip
{
    uint  FrameBase;
    uint  NumFrames;
    uint  NumBones;
    float Duration;
};

struct FGpuPoseSkeleton
{
    uint BoneBase;
    uint NumBones;
    uint MaxDepth;
    uint _Pad;
};

struct FGpuPoseArgs
{
    GPUSpan<FGpuPoseJob>      Jobs;
    GPUSpan<FGpuPoseSample>   Samples;
    GPUSpan<FGpuPoseClip>     Clips;
    GPUSpan<uint2>            Rotations;
    GPUSpan<float4>           Translations;
    GPUSpan<FGpuPoseSkeleton> Skeletons;
    // Parent and hierarchy depth per bone.
    GPUSpan<int2>             BoneInfo;
    // Three rows per bone.
    GPUSpan<float4>           InvBind;
    // Rotation then translation per bone.
    GPUSpan<float4>           BindLocal;
    GPUSpan<FBoneTransform>   OutBones;
};
FGpuPoseArgs* Pass() { return GetPassArgs<FGpuPoseArgs>(); }

groupshared float4 gWorld[LOCAL_SIZE_X * 3];

// Four 16-bit signed normalized components, two to a word, sign-extended by the arithmetic shifts.
float4 UnpackSnorm4(uint2 P)
{
    const int4 V = int4(int(P.x << 16) >> 16, int(P.x) >> 16, int(P.y << 16) >> 16, int(P.y) >> 16);
    return max(float4(V) / 32767.0, -1.0);
}

// Locates the frame pair the way the CPU sampler does, so a time lands on the same two keys.
void SampleBone(FGpuPoseClip Clip, float Time, uint Bone, out float4 Q, out float3 T)
{
    uint  F0    = 0u;
    uint  F1    = 0u;
    float Alpha = 0.0;
    if (Clip.NumFrames > 1u && Clip.Duration > 0.0)
    {
        const float Position = saturate(Time / Clip.Duration) * float(Clip.NumFrames - 1u);
        F0    = min(uint(floor(Position)), Clip.NumFrames - 1u);
        F1    = min(F0 + 1u, Clip.NumFrames - 1u);
        Alpha = Position - floor(Position);
    }

    const uint I0 = Clip.FrameBase + F0 * Clip.NumBones + Bone;
    const uint I1 = Clip.FrameBase + F1 * Clip.NumBones + Bone;
    const float4 QA = UnpackSnorm4(Pass().Rotations.Get(I0));
    float4 QB = UnpackSnorm4(Pass().Rotations.Get(I1));
    QB = dot(QA, QB) < 0.0 ? -QB : QB;
    Q = normalize(lerp(QA, QB, Alpha));
    T = lerp(Pass().Translations.Get(I0).xyz, Pass().Translations.Get(I1).xyz, Alpha);
}

void Compose(float4 Q, float3 T, out float4 R0, out float4 R1, out float4 R2)
{
    const float x = Q.x, y = Q.y, z = Q.z, w = Q.w;
    R0 = float4(1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - w * z),       2.0 * (x * z + w * y),       T.x);
    R1 = float4(2.0 * (x * y + w * z),       1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - w * x),       T.y);
    R2 = float4(2.0 * (x * z - w * y),       2.0 * (y * z + w * x),       1.0 - 2.0 * (x * x + y * y), T.z);
}

// One row of the product of a parent's rows with a child's rows, translation in w.
float4 MulRow(float4 ParentRow, float4 L0, float4 L1, float4 L2)
{
    return float4(ParentRow.x * L0.xyz + ParentRow.y * L1.xyz + ParentRow.z * L2.xyz,
                  ParentRow.x * L0.w + ParentRow.y * L1.w + ParentRow.z * L2.w + ParentRow.w);
}

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void ComputeMain(uint3 Group : SV_GroupID, uint Lane : SV_GroupIndex)
{
    // Uniform across the group, so leaving here cannot strand a barrier below.
    const uint JobIndex = Group.y * kMaxGroupsX + Group.x;
    if (!Pass().Jobs.Contains(JobIndex))
    {
        return;
    }

    const FGpuPoseJob      Job  = Pass().Jobs.Get(JobIndex);
    const FGpuPoseSkeleton Skel = Pass().Skeletons.Get(Job.Skeleton);
    const bool bBone = Lane < Skel.NumBones;
    const uint BoneEntry = Skel.BoneBase + Lane;

    float4 L0 = 0, L1 = 0, L2 = 0;
    int2 Info = int2(-1, 0);
    if (bBone)
    {
        const float4 BindRotation    = Pass().BindLocal.Get(BoneEntry * 2u);
        const float3 BindTranslation = Pass().BindLocal.Get(BoneEntry * 2u + 1u).xyz;

        // Every sample is sign-aligned to the first, so the weighted sum stays on one hemisphere before it normalizes.
        float4 Q = 0;
        float3 T = 0;
        float4 Reference = BindRotation;
        for (uint s = 0u; s < Job.NumSamples; ++s)
        {
            const FGpuPoseSample S = Pass().Samples.Get(Job.FirstSample + s);
            float4 QS = BindRotation;
            float3 TS = BindTranslation;

            // A root pinned for root motion holds its bind pose in every sample, as the CPU sampler pins it.
            if (S.Clip != kBindClip && int(Lane) != Job.RootLockBone)
            {
                SampleBone(Pass().Clips.Get(S.Clip), S.Time, Lane, QS, TS);
            }
            Reference = s == 0u ? QS : Reference;
            QS = dot(Reference, QS) < 0.0 ? -QS : QS;
            Q += QS * S.Weight;
            T += TS * S.Weight;
        }
        if (Job.NumSamples == 0u)
        {
            Q = BindRotation;
            T = BindTranslation;
        }

        Compose(normalize(Q), T, L0, L1, L2);
        Info = Pass().BoneInfo.Get(BoneEntry);
        if (Info.y == 0)
        {
            gWorld[Lane * 3 + 0] = L0;
            gWorld[Lane * 3 + 1] = L1;
            gWorld[Lane * 3 + 2] = L2;
        }
    }

    // One hierarchy level per step, so every parent is final before its children read it.
    for (uint Depth = 1u; Depth <= Skel.MaxDepth; ++Depth)
    {
        GroupMemoryBarrierWithGroupSync();
        if (bBone && uint(Info.y) == Depth && Info.x >= 0)
        {
            const uint P = uint(Info.x) * 3u;
            const float4 W0 = MulRow(gWorld[P + 0], L0, L1, L2);
            const float4 W1 = MulRow(gWorld[P + 1], L0, L1, L2);
            const float4 W2 = MulRow(gWorld[P + 2], L0, L1, L2);
            gWorld[Lane * 3 + 0] = W0;
            gWorld[Lane * 3 + 1] = W1;
            gWorld[Lane * 3 + 2] = W2;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const uint Out = Job.ArenaBase + Lane;
    if (!bBone || !Pass().OutBones.Contains(Out))
    {
        return;
    }

    const uint B = Lane * 3u;
    const float4 IB0 = Pass().InvBind.Get(BoneEntry * 3u + 0u);
    const float4 IB1 = Pass().InvBind.Get(BoneEntry * 3u + 1u);
    const float4 IB2 = Pass().InvBind.Get(BoneEntry * 3u + 2u);
    const float4 S0 = MulRow(gWorld[B + 0], IB0, IB1, IB2);
    const float4 S1 = MulRow(gWorld[B + 1], IB0, IB1, IB2);
    const float4 S2 = MulRow(gWorld[B + 2], IB0, IB1, IB2);

    // The same half layout PackBoneTransform writes on the CPU, so UnpackBoneRows reads it back unchanged.
    FBoneTransform Packed;
    Packed.Rot[0] = PackHalf2(S0.xy);
    Packed.Rot[1] = PackHalf2(float2(S0.z, S1.x));
    Packed.Rot[2] = PackHalf2(S1.yz);
    Packed.Rot[3] = PackHalf2(S2.xy);
    Packed.Rot[4] = PackHalf2(float2(S2.z, 0.0));
    Packed.Tx = S0.w;
    Packed.Ty = S1.w;
    Packed.Tz = S2.w;
    Pass().OutBones.Set(Out, Packed);
}
