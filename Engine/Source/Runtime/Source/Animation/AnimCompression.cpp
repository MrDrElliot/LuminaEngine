#include "RuntimePCH.h"
#include "AnimCompression.h"
#include "Core/Math/SIMD/SIMDConfig.h"

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
            if (Times.Num() < 2)
            {
                return 0.0f;
            }

            TVector<float> Deltas;
            Deltas.Reserve(Times.Num() - 1);
            for (int32 i = 1; i < Times.Num(); ++i)
            {
                const float Delta = Times[i] - Times[i - 1];
                if (Delta > 1e-6f)
                {
                    Deltas.Add(Delta);
                }
            }

            if (Deltas.IsEmpty())
            {
                return 0.0f;
            }

            Algo::Sort(Deltas);
            return Deltas[Deltas.Num() / 2];
        }

        bool IsConstantVec3(const TVector<FVector3>& Values, float Tolerance)
        {
            for (int32 i = 1; i < Values.Num(); ++i)
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
            for (int32 i = 1; i < Values.Num(); ++i)
            {
                if (Math::Abs(Math::Dot(Values[i], Values[0])) < 1.0f - Tolerance)
                {
                    return false;
                }
            }
            return true;
        }

        bool IsStillChannel(const FAnimationChannel& Channel)
        {
            switch (Channel.TargetPath)
            {
            case FAnimationChannel::ETargetPath::Rotation:    return IsConstantQuat(Channel.Rotations, ConstantRotationTolerance);
            case FAnimationChannel::ETargetPath::Translation: return IsConstantVec3(Channel.Translations, ConstantTranslationTolerance);
            case FAnimationChannel::ETargetPath::Scale:       return IsConstantVec3(Channel.Scales, ConstantScaleTolerance);
            default:                                          return true;
            }
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
            Track.DataOffset  = (uint32)Pool.Num();
            Track.RangeMin    = Min;
            Track.RangeExtent = Extent;

            Pool.Reserve(Pool.Num() + Values.Num() * 3);
            for (const FVector3& Value : Values)
            {
                Pool.Add(QuantizeUnit((Value.x - Min.x) * InvExtent.x));
                Pool.Add(QuantizeUnit((Value.y - Min.y) * InvExtent.y));
                Pool.Add(QuantizeUnit((Value.z - Min.z) * InvExtent.z));
            }
        }

        void MakeRawVec3Track(FCompressedAnimTrack& Track, const TVector<FVector3>& Values, TVector<float>& Pool)
        {
            Track.Format     = EAnimTrackFormat::Raw;
            Track.DataOffset = (uint32)Pool.Num();

            Pool.Reserve(Pool.Num() + Values.Num() * 3);
            for (const FVector3& Value : Values)
            {
                Pool.Add(Value.x);
                Pool.Add(Value.y);
                Pool.Add(Value.z);
            }
        }

        void MakeQuantizedQuatTrack(FCompressedAnimTrack& Track, TVector<FQuat>& Values, TVector<uint16>& Pool)
        {
            // Aligning here means decode never needs the hemisphere dot that a raw nlerp would.
            for (int32 i = 1; i < Values.Num(); ++i)
            {
                if (Math::Dot(Values[i], Values[i - 1]) < 0.0f)
                {
                    Values[i] = -Values[i];
                }
            }

            Track.Format     = EAnimTrackFormat::Quantized;
            Track.DataOffset = (uint32)Pool.Num();

            Pool.Reserve(Pool.Num() + Values.Num() * 4);
            for (const FQuat& Value : Values)
            {
                Pool.Add(QuantizeSigned(Value.x));
                Pool.Add(QuantizeSigned(Value.y));
                Pool.Add(QuantizeSigned(Value.z));
                Pool.Add(QuantizeSigned(Value.w));
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
            for (int32 i = 0; i < Bones.Num(); ++i)
            {
                if (Bones[i].BoneName == BoneName)
                {
                    return i;
                }
            }

            FCompressedAnimBone Bone;
            Bone.BoneName = BoneName;
            Bones.Add(Bone);
            return Bones.Num() - 1;
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

        // The decoders index the pools by these offsets unchecked, so a corrupt clip loads empty instead.
        if (Ar.IsReading() && !Data.HasConsistentTracks())
        {
            LOG_ERROR("Compressed animation with {} frames and {} bones has tracks outside its data; the clip loads empty", Data.NumFrames, Data.Bones.Num());
            Ar.SetHasError(true);
            Data.Reset();
        }
        return Ar;
    }

    bool FCompressedAnimData::HasConsistentTracks() const
    {
        if (NumFrames > AnimCompression::MaxFrames)
        {
            return false;
        }

        auto Fits = [this](const FCompressedAnimTrack& Track, uint64 QuantizedLanes, uint64 RawLanes)
        {
            switch (Track.Format)
            {
            case EAnimTrackFormat::None:
            case EAnimTrackFormat::Constant:
                return true;
            case EAnimTrackFormat::Quantized:
                return (uint64)Track.DataOffset + (uint64)NumFrames * QuantizedLanes <= static_cast<uint64>(QuantizedData.Num());
            case EAnimTrackFormat::Raw:
                return RawLanes == 0 || (uint64)Track.DataOffset + (uint64)NumFrames * RawLanes <= static_cast<uint64>(RawData.Num());
            default:
                return false;
            }
        };

        for (const FCompressedAnimBone& Bone : Bones)
        {
            if (!Fits(Bone.Translation, 3, 3) || !Fits(Bone.Rotation, 4, 0) || !Fits(Bone.Scale, 3, 0))
            {
                return false;
            }
        }
        return true;
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
            return DecodeQuantizedVec3(QuantizedData.GetData(), Track, Frame0, Frame1, Alpha);

        case EAnimTrackFormat::Raw:
        {
            const float* A = RawData.GetData() + Track.DataOffset + (SIZE_T)Frame0 * 3;
            const float* B = RawData.GetData() + Track.DataOffset + (SIZE_T)Frame1 * 3;
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

        return DecodeQuantizedVec3(QuantizedData.GetData(), Track, Frame0, Frame1, Alpha);
    }

    FQuat FCompressedAnimData::DecodeRotation(const FCompressedAnimTrack& Track, uint32 Frame0, uint32 Frame1, float Alpha) const
    {
        if (Track.Format != EAnimTrackFormat::Quantized)
        {
            return FQuat(Track.Constant.w, Track.Constant.x, Track.Constant.y, Track.Constant.z);
        }

        const uint16* A = QuantizedData.GetData() + Track.DataOffset + (SIZE_T)Frame0 * 4;
        const uint16* B = QuantizedData.GetData() + Track.DataOffset + (SIZE_T)Frame1 * 4;

        // Stored frames are hemisphere-aligned by the builder, so this needs no sign fixup.
        const FQuat Q0(DequantizeSigned(A[3]), DequantizeSigned(A[0]), DequantizeSigned(A[1]), DequantizeSigned(A[2]));
        const FQuat Q1(DequantizeSigned(B[3]), DequantizeSigned(B[0]), DequantizeSigned(B[1]), DequantizeSigned(B[2]));

        return Math::Normalize(Q0 * (1.0f - Alpha) + Q1 * Alpha);
    }

    void FCompressedAnimData::DecodeQuantizedRotationBatch(const uint32* TrackOffsets, uint32 Frame0, uint32 Frame1, float Alpha,
                                                           float (&OutXYZW)[4][RotationBatchSize]) const
    {
        const uint16* Pool = QuantizedData.GetData();
        const SIZE_T First = (SIZE_T)Frame0 * 4;
        const SIZE_T Second = (SIZE_T)Frame1 * 4;

        // Row k holds track k's two frames as eight floats, x y z w of the first then of the second.
        __m256 Rows[RotationBatchSize];
        for (int32 k = 0; k < RotationBatchSize; ++k)
        {
            const __m128i A = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(Pool + TrackOffsets[k] + First));
            const __m128i B = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(Pool + TrackOffsets[k] + Second));
            Rows[k] = _mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(_mm_unpacklo_epi64(A, B)));
        }

        const __m256 T0 = _mm256_unpacklo_ps(Rows[0], Rows[1]); const __m256 T1 = _mm256_unpackhi_ps(Rows[0], Rows[1]);
        const __m256 T2 = _mm256_unpacklo_ps(Rows[2], Rows[3]); const __m256 T3 = _mm256_unpackhi_ps(Rows[2], Rows[3]);
        const __m256 T4 = _mm256_unpacklo_ps(Rows[4], Rows[5]); const __m256 T5 = _mm256_unpackhi_ps(Rows[4], Rows[5]);
        const __m256 T6 = _mm256_unpacklo_ps(Rows[6], Rows[7]); const __m256 T7 = _mm256_unpackhi_ps(Rows[6], Rows[7]);
        const __m256 S0 = _mm256_shuffle_ps(T0, T2, _MM_SHUFFLE(1, 0, 1, 0)); const __m256 S1 = _mm256_shuffle_ps(T0, T2, _MM_SHUFFLE(3, 2, 3, 2));
        const __m256 S2 = _mm256_shuffle_ps(T1, T3, _MM_SHUFFLE(1, 0, 1, 0)); const __m256 S3 = _mm256_shuffle_ps(T1, T3, _MM_SHUFFLE(3, 2, 3, 2));
        const __m256 S4 = _mm256_shuffle_ps(T4, T6, _MM_SHUFFLE(1, 0, 1, 0)); const __m256 S5 = _mm256_shuffle_ps(T4, T6, _MM_SHUFFLE(3, 2, 3, 2));
        const __m256 S6 = _mm256_shuffle_ps(T5, T7, _MM_SHUFFLE(1, 0, 1, 0)); const __m256 S7 = _mm256_shuffle_ps(T5, T7, _MM_SHUFFLE(3, 2, 3, 2));

        const __m256 QInverse = _mm256_set1_ps(QuantizedInverse);
        const __m256 One = _mm256_set1_ps(1.0f);
        const __m256 Two = _mm256_set1_ps(2.0f);
        const auto Dequantize = [&](__m256 V) { return _mm256_sub_ps(_mm256_mul_ps(_mm256_mul_ps(V, QInverse), Two), One); };

        const __m256 FirstWeight = _mm256_set1_ps(1.0f - Alpha);
        const __m256 SecondWeight = _mm256_set1_ps(Alpha);
        const auto Lerp = [&](__m256 A, __m256 B) { return _mm256_add_ps(_mm256_mul_ps(Dequantize(A), FirstWeight), _mm256_mul_ps(Dequantize(B), SecondWeight)); };

        const __m256 X = Lerp(_mm256_permute2f128_ps(S0, S4, 0x20), _mm256_permute2f128_ps(S0, S4, 0x31));
        const __m256 Y = Lerp(_mm256_permute2f128_ps(S1, S5, 0x20), _mm256_permute2f128_ps(S1, S5, 0x31));
        const __m256 Z = Lerp(_mm256_permute2f128_ps(S2, S6, 0x20), _mm256_permute2f128_ps(S2, S6, 0x31));
        const __m256 W = Lerp(_mm256_permute2f128_ps(S3, S7, 0x20), _mm256_permute2f128_ps(S3, S7, 0x31));

        // Math::Normalize's dot order and reciprocal, with the identity where the length is not positive.
        const __m256 LengthSquared = _mm256_add_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(X, X), _mm256_mul_ps(Y, Y)), _mm256_mul_ps(Z, Z)), _mm256_mul_ps(W, W));
        const __m256 Length = _mm256_sqrt_ps(LengthSquared);
        const __m256 InvLength = _mm256_div_ps(One, Length);
        const __m256 Positive = _mm256_cmp_ps(Length, _mm256_setzero_ps(), _CMP_GT_OQ);
        _mm256_storeu_ps(OutXYZW[0], _mm256_and_ps(_mm256_mul_ps(X, InvLength), Positive));
        _mm256_storeu_ps(OutXYZW[1], _mm256_and_ps(_mm256_mul_ps(Y, InvLength), Positive));
        _mm256_storeu_ps(OutXYZW[2], _mm256_and_ps(_mm256_mul_ps(Z, InvLength), Positive));
        _mm256_storeu_ps(OutXYZW[3], _mm256_blendv_ps(One, _mm256_mul_ps(W, InvLength), Positive));
    }

FVector3 AnimCompression::SampleKeysVec3(const TVector<float>& Times, const TVector<FVector3>& Values, float Time)
    {
        const int32 N = Times.Num();
        if (N == 0 || Values.IsEmpty())
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
        int32 Lo = 0;
        int32 Hi = N - 1;
        while (Lo + 1 < Hi)
        {
            const int32 Mid = (Lo + Hi) >> 1;
            (Time < Times[Mid] ? Hi : Lo) = Mid;
        }

        const float Dt = Times[Lo + 1] - Times[Lo];
        const float t  = Dt > 0.0f ? (Time - Times[Lo]) / Dt : 0.0f;
        return Math::Mix(Values[Lo], Values[Lo + 1], t);
    }

FQuat AnimCompression::SampleKeysQuat(const TVector<float>& Times, const TVector<FQuat>& Values, float Time)
    {
        const int32 N = Times.Num();
        if (N == 0 || Values.IsEmpty())
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

        int32 Lo = 0;
        int32 Hi = N - 1;
        while (Lo + 1 < Hi)
        {
            const int32 Mid = (Lo + Hi) >> 1;
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
        // nlerp, since adjacent keys are dense enough that a normalized lerp is visually identical.
        return Math::Normalize(Q0 * (1.0f - t) + Q1 * t);
    }

    float AnimCompression::InferSampleRate(const FAnimationResource& Resource)
    {
        // Exporters key a still channel only at its ends, so only moving channels say how densely the clip was authored.
        float MovingDelta = 0.0f;
        float AnyDelta    = 0.0f;

        for (const FAnimationChannel& Channel : Resource.Channels)
        {
            const float Delta = MedianKeyDelta(Channel.Timestamps);
            if (Delta <= 0.0f)
            {
                continue;
            }

            AnyDelta = AnyDelta > 0.0f ? Math::Min(AnyDelta, Delta) : Delta;
            if (!IsStillChannel(Channel))
            {
                MovingDelta = MovingDelta > 0.0f ? Math::Min(MovingDelta, Delta) : Delta;
            }
        }

        const float Delta = MovingDelta > 0.0f ? MovingDelta : AnyDelta;
        if (Delta <= 0.0f)
        {
            return 0.0f;
        }

        // Authoring rates are whole numbers, so rounding absorbs the float drift in the source timestamps.
        return Math::Max(1.0f, Math::Round(1.0f / Delta));
    }

    void AnimCompression::Build(FAnimationResource& Resource)
    {
        FCompressedAnimData& Compressed = Resource.Compressed;
        Compressed.Reset();

        if (Resource.Channels.IsEmpty())
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
                         *Resource.Name, (uint32)Requested, (uint32)Rate, MaxFrames);
            }
            NumFrames = Math::Clamp((uint32)Math::Min(Requested, (float)MaxFrames), 2u, MaxFrames);
        }

        Compressed.NumFrames  = NumFrames;
        Compressed.SampleRate = NumFrames > 1 && Duration > 0.0f ? (float)(NumFrames - 1) / Duration : 0.0f;
        Compressed.Bones.Reserve(Resource.Channels.Num() / 3 + 1);

        const float FrameStep = NumFrames > 1 ? Duration / (float)(NumFrames - 1) : 0.0f;

        TVector<FVector3> SampledVectors;
        TVector<FQuat> SampledQuats;

        for (const FAnimationChannel& Channel : Resource.Channels)
        {
            const int32 BoneIndex = FindOrAddBone(Compressed.Bones, Channel.TargetBone);
            FCompressedAnimBone& Bone = Compressed.Bones[BoneIndex];

            if (Channel.TargetPath == FAnimationChannel::ETargetPath::Rotation)
            {
                SampledQuats.SetNum(NumFrames);
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
            SampledVectors.SetNum(NumFrames);
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

    }

    AnimCompression::FValidationReport AnimCompression::Validate(const FAnimationResource& Resource, uint32 SamplesPerFrame)
    {
        const FCompressedAnimData& Compressed = Resource.Compressed;

        FValidationReport Report;
        Report.bHasCompressedData = Compressed.IsValid();
        Report.bHasSourceChannels = !Resource.Channels.IsEmpty();
        Report.NumFrames  = Compressed.NumFrames;
        Report.SampleRate = Compressed.SampleRate;
        Report.CompressedBytes = Compressed.GetMemoryUsage();

        for (const FAnimationChannel& Channel : Resource.Channels)
        {
            Report.SourceBytes += Channel.Timestamps.Num() * sizeof(float)
                                + Channel.Translations.Num() * sizeof(FVector3)
                                + Channel.Rotations.Num() * sizeof(FQuat)
                                + Channel.Scales.Num() * sizeof(FVector3);
        }

        if (!Report.bHasCompressedData || !Report.bHasSourceChannels)
        {
            return Report;
        }

        struct FBoneChannels
        {
            int32 Translation = Constants::kIndexNone;
            int32 Rotation    = Constants::kIndexNone;
            int32 Scale       = Constants::kIndexNone;
        };

        TVector<FBoneChannels> BoneChannels(Compressed.Bones.Num());
        for (int32 c = 0; c < Resource.Channels.Num(); ++c)
        {
            const FAnimationChannel& Channel = Resource.Channels[c];
            for (int32 b = 0; b < Compressed.Bones.Num(); ++b)
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

        Report.Bones.SetNum(Compressed.Bones.Num());
        for (int32 b = 0; b < Compressed.Bones.Num(); ++b)
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

            for (int32 b = 0; b < Compressed.Bones.Num(); ++b)
            {
                const FCompressedAnimBone& Bone = Compressed.Bones[b];
                const FBoneChannels& Sources = BoneChannels[b];
                FValidationBoneError& Error = Report.Bones[b];

                if (Bone.Translation.Format != EAnimTrackFormat::None && Sources.Translation != Constants::kIndexNone)
                {
                    const FAnimationChannel& Channel = Resource.Channels[Sources.Translation];
                    const FVector3 Reference = SampleKeysVec3(Channel.Timestamps, Channel.Translations, Time);
                    const FVector3 Decoded   = Compressed.DecodeTranslation(Bone.Translation, Frame0, Frame1, Alpha);
                    Error.MaxTranslationError = Math::Max(Error.MaxTranslationError, Math::Length(Decoded - Reference));
                }

                if (Bone.Rotation.Format != EAnimTrackFormat::None && Sources.Rotation != Constants::kIndexNone)
                {
                    const FAnimationChannel& Channel = Resource.Channels[Sources.Rotation];
                    const FQuat Reference = SampleKeysQuat(Channel.Timestamps, Channel.Rotations, Time);
                    const FQuat Decoded   = Compressed.DecodeRotation(Bone.Rotation, Frame0, Frame1, Alpha);
                    Error.MaxRotationRadians = Math::Max(Error.MaxRotationRadians, AngleBetween(Decoded, Reference));
                }

                if (Bone.Scale.Format != EAnimTrackFormat::None && Sources.Scale != Constants::kIndexNone)
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
            LOG_WARN("Animation '{}': no compressed data, re-import to build it.", *ClipName);
            return;
        }

        if (!Report.bHasSourceChannels)
        {
            LOG_WARN("Animation '{}': source channels were dropped on load, so re-import it to measure error.",
                     *ClipName);
            return;
        }

        const float Ratio = Report.CompressedBytes > 0 ? (float)Report.SourceBytes / (float)Report.CompressedBytes : 0.0f;

        LOG_INFO("Animation '{}': {} KB -> {} KB ({:.2f}x), {} frames at {:.1f} Hz, {} samples over {} bones.",
                 *ClipName, (uint32)(Report.SourceBytes / 1024), (uint32)(Report.CompressedBytes / 1024),
                 Ratio, Report.NumFrames, Report.SampleRate, Report.NumSamples, (uint32)Report.Bones.Num());

        LOG_INFO("  Worst translation {:.6f} on '{}', rotation {:.4f} deg on '{}', scale {:.6f} on '{}'.",
                 Report.MaxTranslationError, *Report.WorstTranslationBone,
                 Math::Degrees(Report.MaxRotationRadians), *Report.WorstRotationBone,
                 Report.MaxScaleError, *Report.WorstScaleBone);
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
