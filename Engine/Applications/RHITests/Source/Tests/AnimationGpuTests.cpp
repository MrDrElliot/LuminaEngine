#include "RHITestHarness.h"

#include "Animation/AnimCompression.h"
#include "Animation/Pose.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Log/Log.h"
#include "Renderer/SkeletonResource.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace Lumina::RHITests
{
    namespace AnimGpu
    {
        // A humanoid-sized rig laid out as a binary heap, the same one AnimationBench measures on the CPU.
        constexpr int32  kBones      = 115;
        constexpr uint32 kGroupSize  = 128;
        constexpr uint32 kNoTrack    = 0xFFFFFFFFu;
        // Enough distinct clips that a crowd's samples spread over more clip data than one character touches.
        constexpr int32  kClips      = 24;
        constexpr uint32 kMaxGroupsX = 65535u;

        // Set on a translation base that indexes the raw float pool rather than the quantized one.
        constexpr uint32 kRawTrackBit = 0x80000000u;

        // One clip's view of one skeleton bone, three 16-byte loads, with the bind local standing in for a track the clip does not animate.
        struct FTrackGPU
        {
            FVector4 RotConst;
            FVector3 TransMin;
            uint32   RotBase = kNoTrack;
            FVector3 TransExtent;
            uint32   TransBase = kNoTrack;
        };
        static_assert(sizeof(FTrackGPU) == 48);

        struct FClipGPU
        {
            uint32 TrackBase = 0;
            uint32 NumFrames = 0;
            float  Duration  = 0.0f;
            uint32 _Pad      = 0;
        };

        // One clip a character samples with its final weight, as a crowd record flattens to.
        struct FSampleGPU
        {
            uint32 Clip   = 0;
            float  Time   = 0.0f;
            float  Weight = 0.0f;
            uint32 _Pad   = 0;
        };
        static_assert(sizeof(FSampleGPU) == 16);

        constexpr uint32 kNoSlot      = 0xFFFFFFFFu;
        constexpr uint32 kCaptureFlag = 1u;

        // A character's sample range and, mid-transition, the pooled slot holding its decaying offset.
        struct FInstanceGPU
        {
            uint32 FirstSample = 0;
            uint32 NumSamples  = 0;
            uint32 Slot        = kNoSlot;
            uint32 Flags       = 0;
            float  Decay       = 0.0f;
            uint32 _Pad0       = 0;
            uint32 _Pad1       = 0;
            uint32 _Pad2       = 0;
        };
        static_assert(sizeof(FInstanceGPU) == 32);

        struct FArgsGPU
        {
            RHI::GPUPtr Clips;
            RHI::GPUPtr Tracks;
            RHI::GPUPtr Pool;
            RHI::GPUPtr RawPool;
            RHI::GPUPtr Samples;
            RHI::GPUPtr Instances;
            RHI::GPUPtr Slots;
            RHI::GPUPtr BoneInfo;
            RHI::GPUPtr InvBind;
            RHI::GPUPtr Out;
            uint32      NumBones;
            uint32      MaxDepth;
            uint32      NumInstances;
            // 0 writes float rows, 1 half-precision rows, 2 nothing, which isolates what the output costs.
            uint32      OutMode;
        };

        // Sums a character's weighted clip samples, walks the hierarchy one depth level at a time, applies a pooled transition offset and writes skinning rows.
        constexpr const char* kKernel = R"SLANG(
            struct FRHIRoot { uint64_t Args; };
            [[vk::push_constant]] FRHIRoot gRHI;

            struct FTrack { float4 RotConst; float4 MinAndRot; float4 ExtentAndTrans; };
            struct FClip  { uint TrackBase; uint NumFrames; float Duration; uint _Pad; };
            struct FSample { uint Clip; float Time; float Weight; uint _Pad; };
            struct FInstance { uint FirstSample; uint NumSamples; uint Slot; uint Flags; float Decay; uint _Pad0; uint _Pad1; uint _Pad2; };
            struct FArgs
            {
                FClip* Clips; FTrack* Tracks; uint2* Pool; float4* RawPool; FSample* Samples; FInstance* Instances; float4* Slots;
                int2* BoneInfo; float4* InvBind; float4* Out;
                uint NumBones; uint MaxDepth; uint NumInstances; uint OutMode;
            };

            static const uint kNoSlot = 0xFFFFFFFFu;
            static const uint kCaptureFlag = 1u;

            static const uint kNoTrack = 0xFFFFFFFFu;
            static const uint kRawTrackBit = 0x80000000u;
            static const uint kGroupSize = 128;

            groupshared float4 gWorld[kGroupSize * 3];

            float Unit(uint V) { return float(V) * (1.0 / 65535.0); }
            // The device has no Float16 capability, so halves pack through the GLSL extended instruction the engine uses.
            uint PackHalf2(float2 V) { return spirv_asm { result:$$uint = OpExtInst glsl450 PackHalf2x16 $V }; }
            uint2 PackHalf4(float4 V) { return uint2(PackHalf2(V.xy), PackHalf2(V.zw)); }

            // A frame is one 8-byte load, four 16-bit components packed two to a word.
            float4 Unpack4(uint2 P) { return float4(Unit(P.x & 0xFFFFu), Unit(P.x >> 16), Unit(P.y & 0xFFFFu), Unit(P.y >> 16)); }

            float3 DecodeTrans(FArgs* A, FTrack T, uint Base, uint Frame)
            {
                if ((Base & kRawTrackBit) != 0u)
                {
                    return A.RawPool[(Base & ~kRawTrackBit) + Frame].xyz;
                }
                return T.MinAndRot.xyz + Unpack4(A.Pool[Base + Frame]).xyz * T.ExtentAndTrans.xyz;
            }

            void SampleBone(FArgs* A, uint ClipIndex, float Time, uint Bone, out float4 Q, out float3 T)
            {
                const FClip  Clip  = A.Clips[ClipIndex];
                const FTrack Track = A.Tracks[Clip.TrackBase + Bone];
                float Pos = 0.0; uint F0 = 0u; uint F1 = 0u;
                if (Clip.NumFrames > 1u && Clip.Duration > 0.0)
                {
                    Pos = saturate(Time / Clip.Duration) * float(Clip.NumFrames - 1u);
                    F0  = uint(floor(Pos));
                    F1  = min(F0 + 1u, Clip.NumFrames - 1u);
                    Pos = Pos - floor(Pos);
                }
                const uint RotBase   = asuint(Track.MinAndRot.w);
                const uint TransBase = asuint(Track.ExtentAndTrans.w);
                Q = Track.RotConst;
                if (RotBase != kNoTrack)
                {
                    Q = normalize(lerp(Unpack4(A.Pool[RotBase + F0]), Unpack4(A.Pool[RotBase + F1]), Pos) * 2.0 - 1.0);
                }
                T = Track.MinAndRot.xyz;
                if (TransBase != kNoTrack)
                {
                    T = lerp(DecodeTrans(A, Track, TransBase, F0), DecodeTrans(A, Track, TransBase, F1), Pos);
                }
            }

            void Compose(float4 Q, float3 T, out float4 R0, out float4 R1, out float4 R2)
            {
                const float x = Q.x, y = Q.y, z = Q.z, w = Q.w;
                R0 = float4(1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - w * z),       2.0 * (x * z + w * y),       T.x);
                R1 = float4(2.0 * (x * y + w * z),       1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - w * x),       T.y);
                R2 = float4(2.0 * (x * z - w * y),       2.0 * (y * z + w * x),       1.0 - 2.0 * (x * x + y * y), T.z);
            }

            float4 MulRow(float4 PR, float4 L0, float4 L1, float4 L2)
            {
                return float4(PR.x * L0.xyz + PR.y * L1.xyz + PR.z * L2.xyz, PR.x * L0.w + PR.y * L1.w + PR.z * L2.w + PR.w);
            }

            [shader("compute")]
            [numthreads(kGroupSize, 1, 1)]
            void main(uint3 Group : SV_GroupID, uint Lane : SV_GroupIndex)
            {
                FArgs* A = (FArgs*)gRHI.Args;
                const uint Instance = Group.y * 65535u + Group.x;
                if (Instance >= A.NumInstances)
                {
                    return;
                }
                const bool bBone = Lane < A.NumBones;
                const FInstance I = A.Instances[Instance];

                float4 L0 = 0, L1 = 0, L2 = 0;
                int2 Info = int2(-1, 0);
                if (bBone)
                {
                    // Every sample is sign-aligned to the first, so the weighted sum stays on one hemisphere before it normalizes.
                    float4 Q = 0; float3 T = 0; float4 Reference = 0;
                    for (uint s = 0u; s < I.NumSamples; ++s)
                    {
                        const FSample S = A.Samples[I.FirstSample + s];
                        float4 QS; float3 TS;
                        SampleBone(A, S.Clip, S.Time, Lane, QS, TS);
                        Reference = s == 0u ? QS : Reference;
                        QS = dot(Reference, QS) < 0.0 ? -QS : QS;
                        Q += QS * S.Weight;
                        T += TS * S.Weight;
                    }
                    Compose(normalize(Q), T, L0, L1, L2);
                    Info = A.BoneInfo[Lane];
                    if (Info.y == 0)
                    {
                        gWorld[Lane * 3 + 0] = L0; gWorld[Lane * 3 + 1] = L1; gWorld[Lane * 3 + 2] = L2;
                    }
                }

                for (uint Depth = 1u; Depth <= A.MaxDepth; ++Depth)
                {
                    GroupMemoryBarrierWithGroupSync();
                    if (bBone && uint(Info.y) == Depth)
                    {
                        const uint P = uint(Info.x) * 3u;
                        const float4 W0 = MulRow(gWorld[P + 0], L0, L1, L2);
                        const float4 W1 = MulRow(gWorld[P + 1], L0, L1, L2);
                        const float4 W2 = MulRow(gWorld[P + 2], L0, L1, L2);
                        gWorld[Lane * 3 + 0] = W0; gWorld[Lane * 3 + 1] = W1; gWorld[Lane * 3 + 2] = W2;
                    }
                }
                GroupMemoryBarrierWithGroupSync();

                if (bBone)
                {
                    const uint B = Lane * 3u;
                    const float4 IB0 = A.InvBind[B + 0], IB1 = A.InvBind[B + 1], IB2 = A.InvBind[B + 2];
                    const uint O = (Instance * A.NumBones + Lane) * 3u;
                    float4 S0 = MulRow(gWorld[B + 0], IB0, IB1, IB2);
                    float4 S1 = MulRow(gWorld[B + 1], IB0, IB1, IB2);
                    float4 S2 = MulRow(gWorld[B + 2], IB0, IB1, IB2);

                    // A transition captures last frame's rows against this one's into its slot, then the difference decays away.
                    if (I.Slot != kNoSlot)
                    {
                        const uint SlotRow = (I.Slot * A.NumBones + Lane) * 3u;
                        if ((I.Flags & kCaptureFlag) != 0u)
                        {
                            A.Slots[SlotRow + 0] = A.Out[O + 0] - S0;
                            A.Slots[SlotRow + 1] = A.Out[O + 1] - S1;
                            A.Slots[SlotRow + 2] = A.Out[O + 2] - S2;
                        }
                        S0 += A.Slots[SlotRow + 0] * I.Decay;
                        S1 += A.Slots[SlotRow + 1] * I.Decay;
                        S2 += A.Slots[SlotRow + 2] * I.Decay;
                    }
                    if (A.OutMode == 0u)
                    {
                        A.Out[O + 0] = S0; A.Out[O + 1] = S1; A.Out[O + 2] = S2;
                    }
                    else if (A.OutMode == 1u)
                    {
                        uint2* Half = (uint2*)A.Out;
                        Half[O + 0] = PackHalf4(S0); Half[O + 1] = PackHalf4(S1); Half[O + 2] = PackHalf4(S2);
                    }
                    else if (asuint(S0.x + S1.y + S2.z) == 0x7FC00001u)
                    {
                        // A NaN payload no pose produces, so the whole evaluation stays live with no store.
                        A.Out[O] = S0;
                    }
                }
            }
        )SLANG";

        struct FScene
        {
            FSkeletonResource          Skeleton;
            FAnimationResource         Clips[kClips];
            TVector<int32>             Depths;
            int32                      MaxDepth = 0;
        };

        // Wide is AnimationBench's binary heap, depth 6; deep is five interleaved chains, depth 22, the shape of a rig with fingers.
        enum class ERigShape : uint8 { Wide, Deep };

        int32 ParentOf(ERigShape Shape, int32 Bone)
        {
            if (Bone == 0)
            {
                return Constants::kIndexNone;
            }
            return Shape == ERigShape::Wide ? (Bone - 1) / 2 : Math::Max(0, Bone - 5);
        }

        void BuildSkeleton(FSkeletonResource& Skeleton, ERigShape Shape)
        {
            for (int32 i = 0; i < kBones; ++i)
            {
                FSkeletonResource::FBoneInfo Bone;
                Bone.Name           = FName(("Bone" + std::to_string(i)).c_str());
                Bone.ParentIndex    = ParentOf(Shape, i);
                Bone.InvBindMatrix  = FMatrix4::Identity();
                Bone.LocalTransform = AnimPose::ComposeTRS(FVector3(0.03f * (float)(i % 3), 0.12f, 0.02f),
                                                           FQuat(FVector3(0.1f, 0.05f * (float)(i % 4), 0.0f)), FVector3(1.0f));
                Skeleton.BoneNameToIndex[Bone.Name] = i;
                Skeleton.Bones.Add(Bone);
            }
            Skeleton.BuildBindPoseCache();
        }

        // Every bone rotates and a quarter of them translate, at 30 keys a second, matching AnimationBench's clips.
        void BuildClip(const FSkeletonResource& Skeleton, float Duration, float Seed, FAnimationResource& Resource)
        {
            Resource.Duration = Duration;
            const uint32 NumKeys = (uint32)(Duration * 30.0f) + 1;
            for (int32 b = 0; b < kBones; ++b)
            {
                FAnimationChannel Rotation;
                Rotation.TargetBone = Skeleton.Bones[b].Name;
                Rotation.TargetPath = FAnimationChannel::ETargetPath::Rotation;
                for (uint32 k = 0; k < NumKeys; ++k)
                {
                    const float T = Duration * (float)k / (float)(NumKeys - 1);
                    Rotation.Timestamps.Add(T);
                    Rotation.Rotations.Emplace(FVector3(Math::Sin(T * 3.0f + Seed + b) * 0.4f, Math::Cos(T * 2.0f + b) * 0.3f, 0.1f * Seed));
                }
                Resource.Channels.Add(Rotation);

                if (b % 4 == 0)
                {
                    FAnimationChannel Translation;
                    Translation.TargetBone = Skeleton.Bones[b].Name;
                    Translation.TargetPath = FAnimationChannel::ETargetPath::Translation;
                    for (uint32 k = 0; k < NumKeys; ++k)
                    {
                        const float T = Duration * (float)k / (float)(NumKeys - 1);
                        Translation.Timestamps.Add(T);
                        Translation.Translations.Emplace(0.03f, 0.12f + 0.02f * Math::Sin(T * 5.0f + Seed), 0.02f);
                    }
                    Resource.Channels.Add(Translation);
                }
            }
            AnimCompression::Build(Resource);
        }

        void BuildScene(FScene& Scene, ERigShape Shape)
        {
            BuildSkeleton(Scene.Skeleton, Shape);
            for (int32 c = 0; c < kClips; ++c)
            {
                BuildClip(Scene.Skeleton, 1.2f + 0.1f * (float)c, (float)c, Scene.Clips[c]);
            }
            Scene.Depths.SetNum(kBones);
            for (int32 b = 0; b < kBones; ++b)
            {
                const int32 Parent = Scene.Skeleton.Bones[b].ParentIndex;
                Scene.Depths[b] = Parent < 0 ? 0 : Scene.Depths[Parent] + 1;
                Scene.MaxDepth  = Math::Max(Scene.MaxDepth, Scene.Depths[b]);
            }
        }

        const FCompressedAnimBone* FindClipBone(const FCompressedAnimData& Data, FName Bone)
        {
            for (const FCompressedAnimBone& Entry : Data.Bones)
            {
                if (Entry.BoneName == Bone)
                {
                    return &Entry;
                }
            }
            return nullptr;
        }

        // The runtime layout, built from the serialized clip so every frame is one aligned vector load.
        void BuildTracks(const FScene& Scene, TVector<FClipGPU>& OutClips, TVector<FTrackGPU>& OutTracks, TVector<FUIntVector2>& OutPool, TVector<FVector4>& OutRawPool)
        {
            for (const FAnimationResource& Clip : Scene.Clips)
            {
                const FCompressedAnimData& Data = Clip.Compressed;
                FClipGPU& GpuClip = OutClips.EmplaceGetRef();
                GpuClip.TrackBase = (uint32)OutTracks.Num();
                GpuClip.NumFrames = Data.NumFrames;
                GpuClip.Duration  = Clip.Duration;

                for (int32 b = 0; b < kBones; ++b)
                {
                    FTrackGPU& Track = OutTracks.EmplaceGetRef();
                    const FQuat    BindR = Scene.Skeleton.BindLocalRotations[b];
                    const FVector3 BindT = Scene.Skeleton.BindLocalTranslations[b];
                    Track.RotConst = FVector4(BindR.x, BindR.y, BindR.z, BindR.w);
                    Track.TransMin = BindT;

                    const FCompressedAnimBone* Bone = FindClipBone(Data, Scene.Skeleton.Bones[b].Name);
                    if (Bone == nullptr)
                    {
                        continue;
                    }
                    if (Bone->Rotation.Format == EAnimTrackFormat::Quantized)
                    {
                        Track.RotBase = (uint32)OutPool.Num();
                        for (uint32 f = 0; f < Data.NumFrames; ++f)
                        {
                            const uint16* Q = Data.QuantizedData.GetData() + Bone->Rotation.DataOffset + (SIZE_T)f * 4;
                            OutPool.Emplace((uint32)Q[0] | ((uint32)Q[1] << 16), (uint32)Q[2] | ((uint32)Q[3] << 16));
                        }
                    }
                    else if (Bone->Rotation.Format == EAnimTrackFormat::Constant)
                    {
                        const FVector4 C = Bone->Rotation.Constant;
                        Track.RotConst = C;
                    }
                    if (Bone->Translation.Format == EAnimTrackFormat::Quantized)
                    {
                        Track.TransBase   = (uint32)OutPool.Num();
                        Track.TransMin    = Bone->Translation.RangeMin;
                        Track.TransExtent = Bone->Translation.RangeExtent;
                        for (uint32 f = 0; f < Data.NumFrames; ++f)
                        {
                            const uint16* Q = Data.QuantizedData.GetData() + Bone->Translation.DataOffset + (SIZE_T)f * 3;
                            OutPool.Emplace((uint32)Q[0] | ((uint32)Q[1] << 16), (uint32)Q[2]);
                        }
                    }
                    else if (Bone->Translation.Format == EAnimTrackFormat::Raw)
                    {
                        Track.TransBase = (uint32)OutRawPool.Num() | kRawTrackBit;
                        for (uint32 f = 0; f < Data.NumFrames; ++f)
                        {
                            const float* R = Data.RawData.GetData() + Bone->Translation.DataOffset + (SIZE_T)f * 3;
                            OutRawPool.Emplace(R[0], R[1], R[2], 0.0f);
                        }
                    }
                    else if (Bone->Translation.Format == EAnimTrackFormat::Constant)
                    {
                        Track.TransMin = FVector3(Bone->Translation.Constant);
                    }
                }
            }
        }

        // About one character in eighteen sits mid-transition and one in 276 starts one, the measured share of a 0.25 second blend.
        constexpr uint32 kTransitionEvery = 18;
        constexpr uint32 kCaptureEvery    = 276;

        // Weighted samples per character in the measured shares, out of a thousand.
        uint32 CrowdSampleCount(uint32 Index)
        {
            constexpr uint32 kThresholds[] = { 313, 476, 478, 749, 752, 978, 994, 999, 1000 };
            constexpr uint32 kCounts[]     = {   1,   2,   3,   4,   5,   6,   8,  12,   16 };
            const uint32 Pick = ((Index * 2654435761u) >> 8) % 1000u;
            uint32 Bucket = 0;
            while (Pick >= kThresholds[Bucket])
            {
                ++Bucket;
            }
            return kCounts[Bucket];
        }

        void AddSamples(uint32 Index, uint32 NumSamples, TVector<FSampleGPU>& Out)
        {
            float Total = 0.0f;
            const int32 First = Out.Num();
            for (uint32 s = 0; s < NumSamples; ++s)
            {
                FSampleGPU& Sample = Out.EmplaceGetRef();
                Sample.Clip   = (Index + s * 5u) % kClips;
                Sample.Time   = 0.0137f * (float)((Index + s * 13u) % 97u);
                Sample.Weight = 0.25f + 0.5f * (float)((Index + s) % 7u) / 7.0f;
                Total += Sample.Weight;
            }
            for (int32 s = First; s < Out.Num(); ++s)
            {
                Out[s].Weight /= Total;
            }
        }

        // The same math on the CPU, decoding through the engine's own track decoders.
        void ReferenceRows(const FScene& Scene, TSpan<const FSampleGPU> Samples, TVector<FVector4>& OutRows)
        {
            const auto Sample = [&](uint32 ClipIndex, float Time, int32 Bone, FQuat& Q, FVector3& T)
            {
                const FAnimationResource&  Clip = Scene.Clips[ClipIndex];
                const FCompressedAnimData& Data = Clip.Compressed;
                uint32 F0, F1; float Alpha;
                Data.GetFrameBlend(Time, Clip.Duration, F0, F1, Alpha);
                Q = Scene.Skeleton.BindLocalRotations[Bone];
                T = Scene.Skeleton.BindLocalTranslations[Bone];
                if (const FCompressedAnimBone* Entry = FindClipBone(Data, Scene.Skeleton.Bones[Bone].Name))
                {
                    if (Entry->Rotation.Format != EAnimTrackFormat::None)
                    {
                        Q = Data.DecodeRotation(Entry->Rotation, F0, F1, Alpha);
                    }
                    if (Entry->Translation.Format != EAnimTrackFormat::None)
                    {
                        T = Data.DecodeTranslation(Entry->Translation, F0, F1, Alpha);
                    }
                }
            };

            TVector<FMatrix4> World(kBones);
            OutRows.SetNum(kBones * 3);
            for (int32 b = 0; b < kBones; ++b)
            {
                FQuat Q(0.0f, 0.0f, 0.0f, 0.0f);
                FVector3 T(0.0f);
                FQuat Reference;
                for (int32 s = 0; s < Samples.Num(); ++s)
                {
                    FQuat QS; FVector3 TS;
                    Sample(Samples[s].Clip, Samples[s].Time, b, QS, TS);
                    Reference = s == 0 ? QS : Reference;
                    if (Math::Dot(Reference, QS) < 0.0f)
                    {
                        QS = -QS;
                    }
                    Q = Q + QS * Samples[s].Weight;
                    T = T + TS * Samples[s].Weight;
                }
                Q = Math::Normalize(Q);
                const FMatrix4 Local = AnimPose::ComposeTRS(T, Q, FVector3(1.0f));
                const int32 Parent = Scene.Skeleton.Bones[b].ParentIndex;
                World[b] = Parent < 0 ? Local : World[Parent] * Local;
                const FMatrix4 Skin = World[b] * Scene.Skeleton.Bones[b].InvBindMatrix;
                for (int32 r = 0; r < 3; ++r)
                {
                    OutRows[b * 3 + r] = FVector4(Skin[0][r], Skin[1][r], Skin[2][r], Skin[3][r]);
                }
            }
        }

        template<typename T>
        RHI::FGPUAllocation Upload(FTestContext& Ctx, const TVector<T>& Data, const char* Name)
        {
            const uint64 Bytes = Math::Max<uint64>(Data.Num() * sizeof(T), 16u);
            const RHI::FGPUAllocation Buffer = Ctx.Malloc(Bytes, RHI::EMemoryType::CPUWrite, Name);
            if (Buffer.Cpu != nullptr && !Data.IsEmpty())
            {
                Memory::Memcpy(Buffer.Cpu, Data.GetData(), Data.Num() * sizeof(T));
            }
            return Buffer;
        }
    }

    // GPU evaluation of a character's folded clip samples with FK, checked against the CPU decoders and timed up to the 100k crowd target.
    RHI_TEST(AnimationGpu, BlendAndHierarchyAtCrowdScale)
    {
        using namespace AnimGpu;

        const TVector<uint32> Spirv = Ctx.CompileShader(kKernel, "RHITests.AnimGpuEval");
        if (Spirv.IsEmpty())
        {
            return;
        }
        const RHI::FPipelineH Pipeline = Ctx.TrackPipeline(RHI::CreateComputePipeline(ShaderSource(Spirv)));

        const bool bTimed = RHI::SupportsTimestamps();
        const RHI::FQueryPoolH Timestamps = bTimed ? RHI::CreateTimestampPool(2) : RHI::FQueryPoolH{};
        RHI::SetTimestampCollection(bTimed);

        for (ERigShape Shape : { ERigShape::Wide, ERigShape::Deep })
        {
            FScene Scene;
            BuildScene(Scene, Shape);

            TVector<FClipGPU>  Clips;
            TVector<FTrackGPU> Tracks;
            TVector<FUIntVector2> Pool;
            TVector<FVector4>     RawPool;
            BuildTracks(Scene, Clips, Tracks, Pool, RawPool);

            TVector<FIntVector2> BoneInfo;
            TVector<FVector4>    InvBind;
            for (int32 b = 0; b < kBones; ++b)
            {
                BoneInfo.Emplace(Scene.Skeleton.Bones[b].ParentIndex, Scene.Depths[b]);
                const FMatrix4& M = Scene.Skeleton.Bones[b].InvBindMatrix;
                for (int32 r = 0; r < 3; ++r)
                {
                    InvBind.Emplace(M[0][r], M[1][r], M[2][r], M[3][r]);
                }
            }

            const RHI::FGPUAllocation ClipBuffer  = Upload(Ctx, Clips, "AnimGpu.Clips");
            const RHI::FGPUAllocation TrackBuffer = Upload(Ctx, Tracks, "AnimGpu.Tracks");
            const RHI::FGPUAllocation PoolBuffer  = Upload(Ctx, Pool, "AnimGpu.Pool");
            const RHI::FGPUAllocation RawBuffer   = Upload(Ctx, RawPool, "AnimGpu.RawPool");
            const RHI::FGPUAllocation InfoBuffer  = Upload(Ctx, BoneInfo, "AnimGpu.BoneInfo");
            const RHI::FGPUAllocation BindBuffer  = Upload(Ctx, InvBind, "AnimGpu.InvBind");

            struct FResult
            {
                double FloatMs  = 0.0;
                double HalfMs   = 0.0;
                double NoneMs   = 0.0;
                float  MaxError = 0.0f;
                double MeanSamples = 0.0;
                uint32 Slots       = 0;
            };

            // A zero sample count draws each character's count from the crowd distribution and gives a share of them transition slots.
            const auto Measure = [&](uint32 Count, uint32 NumSamples)
            {
                const bool bCrowd = NumSamples == 0;
                TVector<FSampleGPU>   Samples;
                TVector<FInstanceGPU> Instances;
                Instances.Reserve((int32)Count);
                uint32 NumSlots = 0;
                for (uint32 i = 0; i < Count; ++i)
                {
                    FInstanceGPU& Instance = Instances.EmplaceGetRef();
                    Instance.FirstSample = (uint32)Samples.Num();
                    Instance.NumSamples  = bCrowd ? CrowdSampleCount(i) : NumSamples;
                    AddSamples(i, Instance.NumSamples, Samples);

                    // The two characters checked against the CPU stay clear of transitions, whose offset has no CPU reference.
                    const bool bChecked = i == 0 || i == Count - 1;
                    if (bCrowd && !bChecked && i % kTransitionEvery == kTransitionEvery / 2)
                    {
                        Instance.Slot  = NumSlots++;
                        Instance.Decay = 0.5f;
                        Instance.Flags = (i % kCaptureEvery == kTransitionEvery / 2) ? kCaptureFlag : 0u;
                    }
                }
                const RHI::FGPUAllocation SampleBuffer   = Upload(Ctx, Samples, "AnimGpu.Samples");
                const RHI::FGPUAllocation InstanceBuffer = Upload(Ctx, Instances, "AnimGpu.Instances");
                const RHI::FGPUAllocation SlotBuffer     = Ctx.Malloc(Math::Max<uint64>((uint64)NumSlots * kBones * 3u * sizeof(FVector4), 16u), RHI::EMemoryType::GPUOnly, "AnimGpu.Slots");
                const uint64 OutBytes = (uint64)Count * kBones * 3u * sizeof(FVector4);
                const RHI::FGPUAllocation OutBuffer = Ctx.Malloc(OutBytes, RHI::EMemoryType::GPUOnly, "AnimGpu.Out");

                FArgsGPU Args;
                Args.Clips        = ClipBuffer.Gpu;
                Args.Tracks       = TrackBuffer.Gpu;
                Args.Pool         = PoolBuffer.Gpu;
                Args.RawPool      = RawBuffer.Gpu;
                Args.Samples      = SampleBuffer.Gpu;
                Args.Instances    = InstanceBuffer.Gpu;
                Args.Slots        = SlotBuffer.Gpu;
                Args.BoneInfo     = InfoBuffer.Gpu;
                Args.InvBind      = BindBuffer.Gpu;
                Args.Out          = OutBuffer.Gpu;
                Args.NumBones     = kBones;
                Args.MaxDepth     = (uint32)Scene.MaxDepth;
                Args.NumInstances = Count;
                Args.OutMode      = 0;

                const uint32 GroupsX = Math::Min(Count, kMaxGroupsX);
                const uint32 GroupsY = (Count + kMaxGroupsX - 1) / kMaxGroupsX;

                // One warm dispatch, then the average of several back to back, each ordered after the last.
                const auto TimeMode = [&](uint32 Mode)
                {
                    constexpr uint32 kRepeats = 8;
                    FArgsGPU ModeArgs = Args;
                    ModeArgs.OutMode = Mode;
                    const RHI::GPUPtr ArgsPtr = RHI::CopyTransient(ModeArgs);
                    const RHI::FCmdListH CL = Ctx.OpenCL();
                    RHI::CmdSetPipeline(CL, Pipeline);
                    RHI::CmdDispatch(CL, ArgsPtr, GroupsX, GroupsY, 1);
                    RHI::CmdResetTimestamps(CL, Timestamps, 0, 2);
                    RHI::CmdWriteTimestamp(CL, Timestamps, 0, RHI::EStageFlags::Compute);
                    // Named so a GPU Trace of this headless run attributes counters to each configuration.
                    const std::string Label = "AnimGpu " + std::to_string(Count) + (Shape == ERigShape::Wide ? " wide" : " deep") + " samples " + std::to_string(NumSamples) + " mode " + std::to_string(Mode);
                    RHI::CmdBeginDebugLabel(CL, Label.c_str());
                    for (uint32 r = 0; r < kRepeats; ++r)
                    {
                        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite);
                        RHI::CmdDispatch(CL, ArgsPtr, GroupsX, GroupsY, 1);
                    }
                    RHI::CmdEndDebugLabel(CL);
                    RHI::CmdWriteTimestamp(CL, Timestamps, 1, RHI::EStageFlags::Compute);
                    Ctx.SubmitAndWait(CL);
                    uint64 Ticks[2] = { RHI::kTimestampUnavailable, RHI::kTimestampUnavailable };
                    if (!RHI::ReadTimestamps(Timestamps, 0, 2, TSpan<uint64>(Ticks, 2)))
                    {
                        return 0.0;
                    }
                    return (double)(Ticks[1] - Ticks[0]) * RHI::GetTimestampPeriodNs() / 1e6 / kRepeats;
                };

                // Two instances spread across the batch are read back and checked against the CPU path.
                const uint32 Checked[2] = { 0u, Count - 1u };
                const uint64 RowBytes = (uint64)kBones * 3u * sizeof(FVector4);
                const RHI::FGPUAllocation Readback = Ctx.Malloc(RowBytes * 2u, RHI::EMemoryType::CPURead, "AnimGpu.Readback");
                {
                    const RHI::FCmdListH CL = Ctx.OpenCL();
                    RHI::CmdSetPipeline(CL, Pipeline);
                    RHI::CmdDispatch(CL, RHI::CopyTransient(Args), GroupsX, GroupsY, 1);
                    RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
                    for (uint32 k = 0; k < 2; ++k)
                    {
                        RHI::CmdMemcpy(CL, { Readback.Gpu + RowBytes * k, RowBytes }, { OutBuffer.Gpu + RowBytes * Checked[k], RowBytes });
                    }
                    RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite, RHI::EStageFlags::Host, RHI::EAccessFlags::HostRead);
                    Ctx.SubmitAndWait(CL);
                }

                FResult Result;
                const FVector4* Rows = Readback.CpuAs<const FVector4>();
                TVector<FVector4> Reference;
                for (uint32 k = 0; k < 2 && Rows != nullptr; ++k)
                {
                    const FInstanceGPU& Instance = Instances[Checked[k]];
                    ReferenceRows(Scene, TSpan<const FSampleGPU>(Samples.GetData() + Instance.FirstSample, (int32)Instance.NumSamples), Reference);
                    for (int32 i = 0; i < Reference.Num(); ++i)
                    {
                        const FVector4 Delta = Math::Abs(Rows[k * kBones * 3 + i] - Reference[i]);
                        Result.MaxError = Math::Max(Result.MaxError, Math::Max(Math::Max(Delta.x, Delta.y), Math::Max(Delta.z, Delta.w)));
                    }
                }
                RHI_CHECK(Result.MaxError < 1e-3f);

                if (bTimed)
                {
                    Result.FloatMs = TimeMode(0);
                    Result.HalfMs  = TimeMode(1);
                    Result.NoneMs  = TimeMode(2);
                }
                Result.MeanSamples = (double)Samples.Num() / Count;
                Result.Slots       = NumSlots;
                return Result;
            };

            std::printf("\n  %d bones, depth %d, two clips blended per character\n", kBones, Scene.MaxDepth);
            std::printf("  %10s %12s %12s %12s %12s %14s\n", "count", "float ms", "ns/char", "half ms", "no out ms", "max abs error");
            for (uint32 Count : { 1000u, 10000u, 50000u, 100000u })
            {
                const FResult Result = Measure(Count, 2);
                std::printf("  %10u %12.3f %12.1f %12.3f %12.3f %14.2e\n", Count, Result.FloatMs, Result.FloatMs * 1e6 / Count, Result.HalfMs, Result.NoneMs, Result.MaxError);
            }

            // The crowd plan of a real graph samples about ten clips per character and the full plan about twenty-three.
            std::printf("\n  100000 characters, depth %d, clips sampled per character\n", Scene.MaxDepth);
            std::printf("  %10s %12s %12s %12s %14s\n", "samples", "float ms", "half ms", "no out ms", "max abs error");
            for (uint32 NumSamples : { 1u, 2u, 4u, 10u, 23u })
            {
                const FResult Result = Measure(100000u, NumSamples);
                std::printf("  %10u %12.3f %12.3f %12.3f %14.2e\n", NumSamples, Result.FloatMs, Result.HalfMs, Result.NoneMs, Result.MaxError);
            }

            // The sample counts and transition rate a real graph's crowd plan produced, measured by AnimationBench.GraphLogicCrowdSamples.
            const FResult Crowd = Measure(100000u, 0u);
            std::printf("  crowd mix %.2f samples, %u transition slots %12.3f %12.3f %12.3f %14.2e\n", Crowd.MeanSamples, Crowd.Slots, Crowd.FloatMs, Crowd.HalfMs, Crowd.NoneMs, Crowd.MaxError);
        }
        RHI::SetTimestampCollection(false);
        std::printf("\n");
    }
}
