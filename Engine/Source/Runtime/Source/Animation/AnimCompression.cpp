#include "RuntimePCH.h"
#include "AnimCompression.h"

#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Serialization/Archiver.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        constexpr float ConstantTranslationTolerance = 1e-5f;
        constexpr float ConstantScaleTolerance       = 1e-5f;
        constexpr float ConstantRotationTolerance    = 1e-6f;

        constexpr float QuantizedScale   = 65535.0f;
        constexpr float QuantizedInverse = 1.0f / 65535.0f;

        uint16 QuantizeUnit(float Normalized)
        {
            return (uint16)(Math::Saturate(Normalized) * QuantizedScale + 0.5f);
        }

        float DequantizeUnit(uint16 Value)
        {
            return (float)Value * QuantizedInverse;
        }

        uint16 QuantizeSigned(float Value)
        {
            return QuantizeUnit(Value * 0.5f + 0.5f);
        }

        float DequantizeSigned(uint16 Value)
        {
            return DequantizeUnit(Value) * 2.0f - 1.0f;
        }

        float MedianKeyDelta(const TVector<float>& Times)
        {
            if (Times.size() < 2)
            {
                return 0.0f;
            }

            TVector<float> Deltas;
            Deltas.reserve(Times.size() - 1);
            for (size_t i = 1; i < Times.size(); ++i)
            {
                const float Delta = Times[i] - Times[i - 1];
                if (Delta > 1e-6f)
                {
                    Deltas.push_back(Delta);
                }
            }

            if (Deltas.empty())
            {
                return 0.0f;
            }

            Algo::Sort(Deltas.begin(), Deltas.end());
            return Deltas[Deltas.size() / 2];
        }

        bool IsConstantVec3(const TVector<FVector3>& Values, float Tolerance)
        {
            for (size_t i = 1; i < Values.size(); ++i)
            {
                const FVector3 Delta = Values[i] - Values[0];
                if (Math::Abs(Delta.x) > Tolerance || Math::Abs(Delta.y) > Tolerance || Math::Abs(Delta.z) > Tolerance)
                {
                    return false;
                }
            }
            return true;
        }

        bool IsConstantQuat(const TVector<FQuat>& Values, float Tolerance)
        {
            for (size_t i = 1; i < Values.size(); ++i)
            {
                if (Math::Abs(Math::Dot(Values[i], Values[0])) < 1.0f - Tolerance)
                {
                    return false;
                }
            }
            return true;
        }

        void MakeConstantTrack(FCompressedAnimTrack& Track, const FVector3& Value)
        {
            Track.Format   = EAnimTrackFormat::Constant;
            Track.Constant = FVector4(Value.x, Value.y, Value.z, 0.0f);
        }

        void MakeConstantTrack(FCompressedAnimTrack& Track, const FQuat& Value)
        {
            Track.Format   = EAnimTrackFormat::Constant;
            Track.Constant = FVector4(Value.x, Value.y, Value.z, Value.w);
        }

        void MakeQuantizedVec3Track(FCompressedAnimTrack& Track, const TVector<FVector3>& Values, TVector<uint16>& Pool)
        {
            FVector3 Min = Values[0];
            FVector3 Max = Values[0];
            for (const FVector3& Value : Values)
            {
                Min = FVector3(Math::Min(Min.x, Value.x), Math::Min(Min.y, Value.y), Math::Min(Min.z, Value.z));
                Max = FVector3(Math::Max(Max.x, Value.x), Math::Max(Max.y, Value.y), Math::Max(Max.z, Value.z));
            }

            const FVector3 Extent = Max - Min;
            const FVector3 InvExtent(Extent.x > 0.0f ? 1.0f / Extent.x : 0.0f,
                                     Extent.y > 0.0f ? 1.0f / Extent.y : 0.0f,
                                     Extent.z > 0.0f ? 1.0f / Extent.z : 0.0f);

            Track.Format      = EAnimTrackFormat::Quantized;
            Track.DataOffset  = (uint32)Pool.size();
            Track.RangeMin    = Min;
            Track.RangeExtent = Extent;

            Pool.reserve(Pool.size() + Values.size() * 3);
            for (const FVector3& Value : Values)
            {
                Pool.push_back(QuantizeUnit((Value.x - Min.x) * InvExtent.x));
                Pool.push_back(QuantizeUnit((Value.y - Min.y) * InvExtent.y));
                Pool.push_back(QuantizeUnit((Value.z - Min.z) * InvExtent.z));
            }
        }

        void MakeRawVec3Track(FCompressedAnimTrack& Track, const TVector<FVector3>& Values, TVector<float>& Pool)
        {
            Track.Format     = EAnimTrackFormat::Raw;
            Track.DataOffset = (uint32)Pool.size();

            Pool.reserve(Pool.size() + Values.size() * 3);
            for (const FVector3& Value : Values)
            {
                Pool.push_back(Value.x);
                Pool.push_back(Value.y);
                Pool.push_back(Value.z);
            }
        }

        void MakeQuantizedQuatTrack(FCompressedAnimTrack& Track, TVector<FQuat>& Values, TVector<uint16>& Pool)
        {
            // Aligning here means decode never needs the hemisphere dot that a raw nlerp would.
            for (size_t i = 1; i < Values.size(); ++i)
            {
                if (Math::Dot(Values[i], Values[i - 1]) < 0.0f)
                {
                    Values[i] = -Values[i];
                }
            }

            Track.Format     = EAnimTrackFormat::Quantized;
            Track.DataOffset = (uint32)Pool.size();

            Pool.reserve(Pool.size() + Values.size() * 4);
            for (const FQuat& Value : Values)
            {
                Pool.push_back(QuantizeSigned(Value.x));
                Pool.push_back(QuantizeSigned(Value.y));
                Pool.push_back(QuantizeSigned(Value.z));
                Pool.push_back(QuantizeSigned(Value.w));
            }
        }

        FVector3 DecodeQuantizedVec3(const uint16* Pool, const FCompressedAnimTrack& Track,
                                     uint32 Frame0, uint32 Frame1, float Alpha)
        {
            const uint16* A = Pool + Track.DataOffset + (SIZE_T)Frame0 * 3;
            const uint16* B = Pool + Track.DataOffset + (SIZE_T)Frame1 * 3;

            const FVector3 Normalized(Math::Mix(DequantizeUnit(A[0]), DequantizeUnit(B[0]), Alpha),
                                      Math::Mix(DequantizeUnit(A[1]), DequantizeUnit(B[1]), Alpha),
                                      Math::Mix(DequantizeUnit(A[2]), DequantizeUnit(B[2]), Alpha));

            return Track.RangeMin + Normalized * Track.RangeExtent;
        }

        // acos(dot) collapses to noise near zero, so measure the relative rotation's half-angle instead.
        float AngleBetween(const FQuat& A, const FQuat& B)
        {
            const FQuat Delta = A * Math::Conjugate(B);
            return 2.0f * std::atan2(Math::Length(FVector3(Delta.x, Delta.y, Delta.z)), Math::Abs(Delta.w));
        }

        int32 FindOrAddBone(TVector<FCompressedAnimBone>& Bones, const FName& BoneName)
        {
            for (int32 i = 0; i < (int32)Bones.size(); ++i)
            {
                if (Bones[i].BoneName == BoneName)
                {
                    return i;
                }
            }

            FCompressedAnimBone Bone;
            Bone.BoneName = BoneName;
            Bones.push_back(Bone);
            return (int32)Bones.size() - 1;
        }
    }

    FArchive& operator << (FArchive& Ar, FCompressedAnimTrack& Data)
    {
        Ar << Data.Format;
        Ar << Data.DataOffset;
        Ar << Data.Constant;
        Ar << Data.RangeMin;
        Ar << Data.RangeExtent;
        return Ar;
    }

    FArchive& operator << (FArchive& Ar, FCompressedAnimBone& Data)
    {
        Ar << Data.BoneName;
        Ar << Data.Translation;
        Ar << Data.Rotation;
        Ar << Data.Scale;
        return Ar;
    }

    FArchive& operator << (FArchive& Ar, FCompressedAnimData& Data)
    {
        Ar << Data.NumFrames;
        Ar << Data.SampleRate;
        Ar << Data.Bones;
        Ar << Data.QuantizedData;
        Ar << Data.RawData;
        return Ar;
    }

    void FCompressedAnimData::GetFrameBlend(float Time, float Duration, uint32& OutFrame0, uint32& OutFrame1, float& OutAlpha) const
    {
        if (NumFrames <= 1 || Duration <= 0.0f)
        {
            OutFrame0 = 0;
            OutFrame1 = 0;
            OutAlpha  = 0.0f;
            return;
        }

        const float LastFrame = (float)(NumFrames - 1);
        const float Position  = Math::Clamp(Time / Duration, 0.0f, 1.0f) * LastFrame;
        const float Floor     = Math::Floor(Position);

        OutFrame0 = (uint32)Floor;
        OutFrame1 = Math::Min(OutFrame0 + 1u, NumFrames - 1u);
        OutAlpha  = Position - Floor;
    }

    FVector3 FCompressedAnimData::DecodeTranslation(const FCompressedAnimTrack& Track, uint32 Frame0, uint32 Frame1, float Alpha) const
    {
        switch (Track.Format)
        {
        case EAnimTrackFormat::Constant:
            return FVector3(Track.Constant.x, Track.Constant.y, Track.Constant.z);

        case EAnimTrackFormat::Quantized:
            return DecodeQuantizedVec3(QuantizedData.data(), Track, Frame0, Frame1, Alpha);

        case EAnimTrackFormat::Raw:
        {
            const float* A = RawData.data() + Track.DataOffset + (SIZE_T)Frame0 * 3;
            const float* B = RawData.data() + Track.DataOffset + (SIZE_T)Frame1 * 3;
            return Math::Mix(FVector3(A[0], A[1], A[2]), FVector3(B[0], B[1], B[2]), Alpha);
        }

        default:
            return FVector3(0.0f);
        }
    }

    FVector3 FCompressedAnimData::DecodeScale(const FCompressedAnimTrack& Track, uint32 Frame0, uint32 Frame1, float Alpha) const
    {
        if (Track.Format != EAnimTrackFormat::Quantized)
        {
            return FVector3(Track.Constant.x, Track.Constant.y, Track.Constant.z);
        }

        return DecodeQuantizedVec3(QuantizedData.data(), Track, Frame0, Frame1, Alpha);
    }

    FQuat FCompressedAnimData::DecodeRotation(const FCompressedAnimTrack& Track, uint32 Frame0, uint32 Frame1, float Alpha) const
    {
        if (Track.Format != EAnimTrackFormat::Quantized)
        {
            return FQuat(Track.Constant.w, Track.Constant.x, Track.Constant.y, Track.Constant.z);
        }

        const uint16* A = QuantizedData.data() + Track.DataOffset + (SIZE_T)Frame0 * 4;
        const uint16* B = QuantizedData.data() + Track.DataOffset + (SIZE_T)Frame1 * 4;

        // Stored frames are hemisphere-aligned by the builder, so this needs no sign fixup.
        const FQuat Q0(DequantizeSigned(A[3]), DequantizeSigned(A[0]), DequantizeSigned(A[1]), DequantizeSigned(A[2]));
        const FQuat Q1(DequantizeSigned(B[3]), DequantizeSigned(B[0]), DequantizeSigned(B[1]), DequantizeSigned(B[2]));

        return Math::Normalize(Q0 * (1.0f - Alpha) + Q1 * Alpha);
    }

FVector3 AnimCompression::SampleKeysVec3(const TVector<float>& Times, const TVector<FVector3>& Values, float Time)
    {
        const size_t N = Times.size();
        if (N == 0 || Values.empty())
        {
            return FVector3(0.0f);
        }
        if (N == 1 || Time <= Times[0])
        {
            return Values[0];
        }
        if (Time >= Times[N - 1])
        {
            return Values[N - 1];
        }

        // Binary search keyframe interval; clips routinely have hundreds of keys.
        size_t Lo = 0;
        size_t Hi = N - 1;
        while (Lo + 1 < Hi)
        {
            const size_t Mid = (Lo + Hi) >> 1;
            (Time < Times[Mid] ? Hi : Lo) = Mid;
        }

        const float Dt = Times[Lo + 1] - Times[Lo];
        const float t  = Dt > 0.0f ? (Time - Times[Lo]) / Dt : 0.0f;
        return Math::Mix(Values[Lo], Values[Lo + 1], t);
    }

FQuat AnimCompression::SampleKeysQuat(const TVector<float>& Times, const TVector<FQuat>& Values, float Time)
    {
        const size_t N = Times.size();
        if (N == 0 || Values.empty())
        {
            return FQuat(1.0f, 0.0f, 0.0f, 0.0f);
        }
        if (N == 1 || Time <= Times[0])
        {
            return Values[0];
        }
        if (Time >= Times[N - 1])
        {
            return Values[N - 1];
        }

        size_t Lo = 0;
        size_t Hi = N - 1;
        while (Lo + 1 < Hi)
        {
            const size_t Mid = (Lo + Hi) >> 1;
            (Time < Times[Mid] ? Hi : Lo) = Mid;
        }

        const float Dt = Times[Lo + 1] - Times[Lo];
        const float t  = Dt > 0.0f ? (Time - Times[Lo]) / Dt : 0.0f;

        FQuat Q0 = Values[Lo];
        FQuat Q1 = Values[Lo + 1];
        if (Math::Dot(Q0, Q1) < 0.0f)
        {
            Q1 = -Q1;
        }
        // nlerp, not slerp: adjacent keys are dense enough that a normalized lerp is visually identical.
        return Math::Normalize(Q0 * (1.0f - t) + Q1 * t);
    }

    float AnimCompression::InferSampleRate(const FAnimationResource& Resource)
    {
        TVector<float> ChannelDeltas;
        ChannelDeltas.reserve(Resource.Channels.size());

        for (const FAnimationChannel& Channel : Resource.Channels)
        {
            const float Delta = MedianKeyDelta(Channel.Timestamps);
            if (Delta > 0.0f)
            {
                ChannelDeltas.push_back(Delta);
            }
        }

        if (ChannelDeltas.empty())
        {
            return 0.0f;
        }

        Algo::Sort(ChannelDeltas.begin(), ChannelDeltas.end());
        const float Delta = ChannelDeltas[ChannelDeltas.size() / 2];

        // Authoring rates are whole numbers, so rounding absorbs the float drift in the source timestamps.
        return Math::Max(1.0f, Math::Round(1.0f / Delta));
    }

    void AnimCompression::Build(FAnimationResource& Resource)
    {
        FCompressedAnimData& Compressed = Resource.Compressed;
        Compressed.Reset();

        if (Resource.Channels.empty())
        {
            return;
        }

        const float Duration = Resource.Duration;
        const float Rate     = InferSampleRate(Resource);

        uint32 NumFrames = 1;
        if (Duration > 0.0f && Rate > 0.0f)
        {
            const float Requested = Math::Round(Duration * Rate) + 1.0f;
            if (Requested > (float)MaxFrames)
            {
                LOG_WARN("Animation '{}': {} frames at {} Hz exceeds the {} frame cap, clamping.",
                         Resource.Name.c_str(), (uint32)Requested, (uint32)Rate, MaxFrames);
            }
            NumFrames = Math::Clamp((uint32)Math::Min(Requested, (float)MaxFrames), 2u, MaxFrames);
        }

        Compressed.NumFrames  = NumFrames;
        Compressed.SampleRate = NumFrames > 1 && Duration > 0.0f ? (float)(NumFrames - 1) / Duration : 0.0f;
        Compressed.Bones.reserve(Resource.Channels.size() / 3 + 1);

        const float FrameStep = NumFrames > 1 ? Duration / (float)(NumFrames - 1) : 0.0f;

        TVector<FVector3> SampledVectors;
        TVector<FQuat> SampledQuats;

        for (const FAnimationChannel& Channel : Resource.Channels)
        {
            const int32 BoneIndex = FindOrAddBone(Compressed.Bones, Channel.TargetBone);
            FCompressedAnimBone& Bone = Compressed.Bones[BoneIndex];

            if (Channel.TargetPath == FAnimationChannel::ETargetPath::Rotation)
            {
                SampledQuats.resize(NumFrames);
                for (uint32 Frame = 0; Frame < NumFrames; ++Frame)
                {
                    SampledQuats[Frame] = SampleKeysQuat(Channel.Timestamps, Channel.Rotations, (float)Frame * FrameStep);
                }

                if (IsConstantQuat(SampledQuats, ConstantRotationTolerance))
                {
                    MakeConstantTrack(Bone.Rotation, SampledQuats[0]);
                }
                else
                {
                    MakeQuantizedQuatTrack(Bone.Rotation, SampledQuats, Compressed.QuantizedData);
                }
                continue;
            }

            const bool bTranslation = Channel.TargetPath == FAnimationChannel::ETargetPath::Translation;
            if (!bTranslation && Channel.TargetPath != FAnimationChannel::ETargetPath::Scale)
            {
                continue;
            }

            const TVector<FVector3>& Source = bTranslation ? Channel.Translations : Channel.Scales;
            SampledVectors.resize(NumFrames);
            for (uint32 Frame = 0; Frame < NumFrames; ++Frame)
            {
                SampledVectors[Frame] = SampleKeysVec3(Channel.Timestamps, Source, (float)Frame * FrameStep);
            }

            FCompressedAnimTrack& Track = bTranslation ? Bone.Translation : Bone.Scale;
            const float Tolerance = bTranslation ? ConstantTranslationTolerance : ConstantScaleTolerance;

            if (IsConstantVec3(SampledVectors, Tolerance))
            {
                MakeConstantTrack(Track, SampledVectors[0]);
            }
            else if (bTranslation)
            {
                // Bone lengths are fixed, so the only bones that really translate are the ones driving root motion.
                MakeRawVec3Track(Track, SampledVectors, Compressed.RawData);
            }
            else
            {
                MakeQuantizedVec3Track(Track, SampledVectors, Compressed.QuantizedData);
            }
        }

        LOG_INFO("Animation '{}': compressed {} channels into {} bones, {} frames at {:.1f} Hz ({} KB).",
                 Resource.Name.c_str(), (uint32)Resource.Channels.size(), (uint32)Compressed.Bones.size(),
                 Compressed.NumFrames, Compressed.SampleRate, (uint32)(Compressed.GetMemoryUsage() / 1024));
    }

    AnimCompression::FValidationReport AnimCompression::Validate(const FAnimationResource& Resource, uint32 SamplesPerFrame)
    {
        const FCompressedAnimData& Compressed = Resource.Compressed;

        FValidationReport Report;
        Report.bHasCompressedData = Compressed.IsValid();
        Report.bHasSourceChannels = !Resource.Channels.empty();
        Report.NumFrames  = Compressed.NumFrames;
        Report.SampleRate = Compressed.SampleRate;
        Report.CompressedBytes = Compressed.GetMemoryUsage();

        for (const FAnimationChannel& Channel : Resource.Channels)
        {
            Report.SourceBytes += Channel.Timestamps.size() * sizeof(float)
                                + Channel.Translations.size() * sizeof(FVector3)
                                + Channel.Rotations.size() * sizeof(FQuat)
                                + Channel.Scales.size() * sizeof(FVector3);
        }

        if (!Report.bHasCompressedData || !Report.bHasSourceChannels)
        {
            return Report;
        }

        struct FBoneChannels
        {
            int32 Translation = INDEX_NONE;
            int32 Rotation    = INDEX_NONE;
            int32 Scale       = INDEX_NONE;
        };

        TVector<FBoneChannels> BoneChannels(Compressed.Bones.size());
        for (int32 c = 0; c < (int32)Resource.Channels.size(); ++c)
        {
            const FAnimationChannel& Channel = Resource.Channels[c];
            for (int32 b = 0; b < (int32)Compressed.Bones.size(); ++b)
            {
                if (Compressed.Bones[b].BoneName != Channel.TargetBone)
                {
                    continue;
                }

                switch (Channel.TargetPath)
                {
                case FAnimationChannel::ETargetPath::Translation: BoneChannels[b].Translation = c; break;
                case FAnimationChannel::ETargetPath::Rotation:    BoneChannels[b].Rotation    = c; break;
                case FAnimationChannel::ETargetPath::Scale:       BoneChannels[b].Scale       = c; break;
                default: break;
                }
                break;
            }
        }

        Report.Bones.resize(Compressed.Bones.size());
        for (SIZE_T b = 0; b < Compressed.Bones.size(); ++b)
        {
            Report.Bones[b].BoneName = Compressed.Bones[b].BoneName;
        }

        const uint32 Steps = Math::Max(1u, SamplesPerFrame);
        Report.NumSamples = Compressed.NumFrames > 1 ? (Compressed.NumFrames - 1) * Steps + 1 : 1;

        const float SampleStep = Report.NumSamples > 1 ? Resource.Duration / (float)(Report.NumSamples - 1) : 0.0f;

        for (uint32 Sample = 0; Sample < Report.NumSamples; ++Sample)
        {
            const float Time = (float)Sample * SampleStep;

            uint32 Frame0, Frame1;
            float Alpha;
            Compressed.GetFrameBlend(Time, Resource.Duration, Frame0, Frame1, Alpha);

            for (SIZE_T b = 0; b < Compressed.Bones.size(); ++b)
            {
                const FCompressedAnimBone& Bone = Compressed.Bones[b];
                const FBoneChannels& Sources = BoneChannels[b];
                FValidationBoneError& Error = Report.Bones[b];

                if (Bone.Translation.Format != EAnimTrackFormat::None && Sources.Translation != INDEX_NONE)
                {
                    const FAnimationChannel& Channel = Resource.Channels[Sources.Translation];
                    const FVector3 Reference = SampleKeysVec3(Channel.Timestamps, Channel.Translations, Time);
                    const FVector3 Decoded   = Compressed.DecodeTranslation(Bone.Translation, Frame0, Frame1, Alpha);
                    Error.MaxTranslationError = Math::Max(Error.MaxTranslationError, Math::Length(Decoded - Reference));
                }

                if (Bone.Rotation.Format != EAnimTrackFormat::None && Sources.Rotation != INDEX_NONE)
                {
                    const FAnimationChannel& Channel = Resource.Channels[Sources.Rotation];
                    const FQuat Reference = SampleKeysQuat(Channel.Timestamps, Channel.Rotations, Time);
                    const FQuat Decoded   = Compressed.DecodeRotation(Bone.Rotation, Frame0, Frame1, Alpha);
                    Error.MaxRotationRadians = Math::Max(Error.MaxRotationRadians, AngleBetween(Decoded, Reference));
                }

                if (Bone.Scale.Format != EAnimTrackFormat::None && Sources.Scale != INDEX_NONE)
                {
                    const FAnimationChannel& Channel = Resource.Channels[Sources.Scale];
                    const FVector3 Reference = SampleKeysVec3(Channel.Timestamps, Channel.Scales, Time);
                    const FVector3 Decoded   = Compressed.DecodeScale(Bone.Scale, Frame0, Frame1, Alpha);
                    Error.MaxScaleError = Math::Max(Error.MaxScaleError, Math::Length(Decoded - Reference));
                }
            }
        }

        for (const FValidationBoneError& Error : Report.Bones)
        {
            if (Error.MaxTranslationError > Report.MaxTranslationError)
            {
                Report.MaxTranslationError  = Error.MaxTranslationError;
                Report.WorstTranslationBone = Error.BoneName;
            }
            if (Error.MaxRotationRadians > Report.MaxRotationRadians)
            {
                Report.MaxRotationRadians = Error.MaxRotationRadians;
                Report.WorstRotationBone  = Error.BoneName;
            }
            if (Error.MaxScaleError > Report.MaxScaleError)
            {
                Report.MaxScaleError = Error.MaxScaleError;
                Report.WorstScaleBone = Error.BoneName;
            }
        }

        return Report;
    }

    void AnimCompression::LogValidationReport(const FName& ClipName, const FValidationReport& Report)
    {
        if (!Report.bHasCompressedData)
        {
            LOG_WARN("Animation '{}': no compressed data, re-import to build it.", ClipName.c_str());
            return;
        }

        if (!Report.bHasSourceChannels)
        {
            LOG_WARN("Animation '{}': source channels were dropped on load, so re-import it to measure error.",
                     ClipName.c_str());
            return;
        }

        const float Ratio = Report.CompressedBytes > 0 ? (float)Report.SourceBytes / (float)Report.CompressedBytes : 0.0f;

        LOG_INFO("Animation '{}': {} KB -> {} KB ({:.2f}x), {} frames at {:.1f} Hz, {} samples over {} bones.",
                 ClipName.c_str(), (uint32)(Report.SourceBytes / 1024), (uint32)(Report.CompressedBytes / 1024),
                 Ratio, Report.NumFrames, Report.SampleRate, Report.NumSamples, (uint32)Report.Bones.size());

        LOG_INFO("  Worst translation {:.6f} on '{}', rotation {:.4f} deg on '{}', scale {:.6f} on '{}'.",
                 Report.MaxTranslationError, Report.WorstTranslationBone.c_str(),
                 Math::Degrees(Report.MaxRotationRadians), Report.WorstRotationBone.c_str(),
                 Report.MaxScaleError, Report.WorstScaleBone.c_str());
    }

    namespace
    {
        void ValidateLoadedAnimations()
        {
            uint32 NumClips = 0;
            for (TObjectIterator<CAnimation> It; It; ++It)
            {
                const FAnimationResource* Resource = (*It)->GetAnimationResource();
                if (Resource == nullptr)
                {
                    continue;
                }

                AnimCompression::LogValidationReport(Resource->Name, AnimCompression::Validate(*Resource));
                ++NumClips;
            }

            LOG_INFO("Anim.ValidateCompression: {} loaded clips checked.", NumClips);
        }

        FAutoConsoleCommand GValidateCompressionCommand(
            "Anim.ValidateCompression",
            "Decode every loaded clip's compressed tracks and report the worst error against its source channels.",
            &ValidateLoadedAnimations);
    }
}
