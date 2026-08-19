#pragma once

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    class FArchive;
    struct FAnimationResource;

    enum class EAnimTrackFormat : uint8
    {
        None,
        Constant,
        Quantized,
        Raw,
    };

    struct FCompressedAnimTrack
    {
        EAnimTrackFormat Format = EAnimTrackFormat::None;

        // Index of frame 0 in the owning block's quantized pool, or its raw pool for EAnimTrackFormat::Raw.
        uint32 DataOffset = 0;

        // Constant holds the whole track; a quaternion uses all four lanes, a vector the first three.
        FVector4 Constant = FVector4(0.0f);

        // Quantized decode range. Rotations span a fixed -1..1 and leave these zero.
        FVector3 RangeMin = FVector3(0.0f);
        FVector3 RangeExtent = FVector3(0.0f);

        bool IsAnimated() const { return Format == EAnimTrackFormat::Quantized || Format == EAnimTrackFormat::Raw; }

        friend RUNTIME_API FArchive& operator << (FArchive& Ar, FCompressedAnimTrack& Data);
    };

    struct FCompressedAnimBone
    {
        FName BoneName;
        FCompressedAnimTrack Translation;
        FCompressedAnimTrack Rotation;
        FCompressedAnimTrack Scale;

        friend RUNTIME_API FArchive& operator << (FArchive& Ar, FCompressedAnimBone& Data);
    };

    // Frame N sits at N * Duration / (NumFrames - 1), so a time indexes a frame pair without any search.
    struct FCompressedAnimData
    {
        uint32 NumFrames = 0;
        float SampleRate = 0.0f;
        TVector<FCompressedAnimBone> Bones;
        TVector<uint16> QuantizedData;
        TVector<float> RawData;

        bool IsValid() const { return NumFrames > 0 && !Bones.empty(); }

        void Reset()
        {
            NumFrames = 0;
            SampleRate = 0.0f;
            Bones.clear();
            QuantizedData.clear();
            RawData.clear();
        }

        SIZE_T GetMemoryUsage() const
        {
            return Bones.size() * sizeof(FCompressedAnimBone)
                 + QuantizedData.size() * sizeof(uint16)
                 + RawData.size() * sizeof(float);
        }

        RUNTIME_API void GetFrameBlend(float Time, float Duration, uint32& OutFrame0, uint32& OutFrame1, float& OutAlpha) const;

        RUNTIME_API FVector3 DecodeTranslation(const FCompressedAnimTrack& Track, uint32 Frame0, uint32 Frame1, float Alpha) const;
        RUNTIME_API FQuat DecodeRotation(const FCompressedAnimTrack& Track, uint32 Frame0, uint32 Frame1, float Alpha) const;
        RUNTIME_API FVector3 DecodeScale(const FCompressedAnimTrack& Track, uint32 Frame0, uint32 Frame1, float Alpha) const;

        friend RUNTIME_API FArchive& operator << (FArchive& Ar, FCompressedAnimData& Data);
    };

    namespace AnimCompression
    {
        // Frames past this are refused so a mis-detected rate cannot allocate without bound.
        inline constexpr uint32 MaxFrames = 65536;

        RUNTIME_API void Build(FAnimationResource& Resource);

        RUNTIME_API float InferSampleRate(const FAnimationResource& Resource);

        // Import-time reference sampler for raw source keys, and what Build resamples through.
        RUNTIME_API FVector3 SampleKeysVec3(const TVector<float>& Times, const TVector<FVector3>& Values, float Time);
        RUNTIME_API FQuat SampleKeysQuat(const TVector<float>& Times, const TVector<FQuat>& Values, float Time);

        struct FValidationBoneError
        {
            FName BoneName;
            float MaxTranslationError = 0.0f;
            float MaxRotationRadians  = 0.0f;
            float MaxScaleError       = 0.0f;
        };

        // Decoded tracks measured against the source channels they were built from.
        struct FValidationReport
        {
            bool bHasCompressedData = false;
            bool bHasSourceChannels = false;
            uint32 NumSamples = 0;
            uint32 NumFrames = 0;
            float SampleRate = 0.0f;

            SIZE_T SourceBytes = 0;
            SIZE_T CompressedBytes = 0;

            float MaxTranslationError = 0.0f;
            float MaxRotationRadians  = 0.0f;
            float MaxScaleError       = 0.0f;

            FName WorstTranslationBone;
            FName WorstRotationBone;
            FName WorstScaleBone;

            TVector<FValidationBoneError> Bones;
        };

        // Import-time only: a loaded clip has dropped its source channels, leaving nothing to compare against.
        RUNTIME_API FValidationReport Validate(const FAnimationResource& Resource, uint32 SamplesPerFrame = 4);

        RUNTIME_API void LogValidationReport(const FName& ClipName, const FValidationReport& Report);
    }
}
