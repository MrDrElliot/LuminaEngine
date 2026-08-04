#include "EditorPCH.h"
#include "AudioStreamFactory.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Audio/AudioDecode.h"
#include "Core/Object/Package/Package.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Thumbnails/ThumbnailUtils.h"
#include "Log/Log.h"

namespace Lumina
{
    CObject* CAudioStreamFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CAudioStream>(Package, Name);
    }

#if USING(WITH_EDITOR)
    // Renders a min/max waveform (channels mixed down) into the package thumbnail.
    static void CreateWaveformThumbnail(CAudioStream* Stream)
    {
        Audio::FAudioInfo Info;
        TVector<float> Samples;
        if (!Audio::DecodePCM(Stream->AudioData->Bytes.data(), Stream->AudioData->Bytes.size(), Info, Samples))
        {
            return;
        }

        constexpr uint32 Res = ThumbnailUtils::kThumbnailResolution;
        TVector<uint8> Pixels((size_t)Res * Res * 4);

        constexpr uint8 BgR = 24,  BgG = 24,  BgB = 28;
        constexpr uint8 WvR = 96,  WvG = 200, WvB = 255;
        for (size_t i = 0; i < (size_t)Res * Res; ++i)
        {
            Pixels[i * 4 + 0] = BgR;
            Pixels[i * 4 + 1] = BgG;
            Pixels[i * 4 + 2] = BgB;
            Pixels[i * 4 + 3] = 255;
        }

        const uint64 Frames   = Info.NumFrames;
        const uint32 Channels = Info.NumChannels;
        const float HalfH     = Res * 0.5f;

        for (uint32 X = 0; X < Res; ++X)
        {
            const uint64 Begin = Frames * X / Res;
            const uint64 End   = Math::Max(Frames * (X + 1) / Res, Begin + 1);

            float MinS = 0.0f, MaxS = 0.0f;
            for (uint64 F = Begin; F < End && F < Frames; ++F)
            {
                float Mixed = 0.0f;
                for (uint32 C = 0; C < Channels; ++C)
                {
                    Mixed += Samples[(size_t)F * Channels + C];
                }
                Mixed /= (float)Channels;
                MinS = Math::Min(MinS, Mixed);
                MaxS = Math::Max(MaxS, Mixed);
            }

            const int32 Y0 = Math::Clamp((int32)(HalfH - MaxS * (HalfH - 2.0f)), 0, (int32)Res - 1);
            const int32 Y1 = Math::Clamp((int32)(HalfH - MinS * (HalfH - 2.0f)), 0, (int32)Res - 1);
            for (int32 Y = Y0; Y <= Y1; ++Y)
            {
                uint8* P = &Pixels[((size_t)Y * Res + X) * 4];
                P[0] = WvR;
                P[1] = WvG;
                P[2] = WvB;
            }
        }

        ThumbnailUtils::StoreDownsampledRGBA(*Stream->GetPackage()->GetPackageThumbnail(),
            Pixels.data(), Res, Res, (size_t)Res * 4);
    }
#endif

    void CAudioStreamFactory::TryImport(const FFixedString& RawPath, const FFixedString& DestinationPath, const Import::FImportSettings* Settings)
    {
        TVector<uint8> Bytes;
        if (!FileHelper::LoadFileToArray(Bytes, RawPath.c_str()) || Bytes.empty())
        {
            LOG_ERROR("AudioStreamFactory: failed to read audio file '{0}'", RawPath.c_str());
            return;
        }

        Audio::FAudioInfo Info;
        if (!Audio::Probe(Bytes.data(), Bytes.size(), Info))
        {
            LOG_ERROR("AudioStreamFactory: '{0}' is not a decodable audio file", RawPath.c_str());
            return;
        }

        CAudioStream* NewStream = TryCreateNew<CAudioStream>(DestinationPath);
        if (NewStream == nullptr)
        {
            LOG_ERROR("AudioStreamFactory: could not create '{0}' for '{1}'; a package already exists at that path",
                DestinationPath.c_str(), RawPath.c_str());
            return;
        }

        NewStream->SourcePath  = FString(RawPath.c_str());
        NewStream->SampleRate  = Info.SampleRate;
        NewStream->NumChannels = Info.NumChannels;
        NewStream->NumFrames   = Info.NumFrames;
        NewStream->AudioData   = MakeShared<FAudioData>();
        NewStream->AudioData->Bytes = Move(Bytes);

#if USING(WITH_EDITOR)
        CreateWaveformThumbnail(NewStream);
#endif

        CPackage* NewPackage = NewStream->GetPackage();
        if (CPackage::SavePackage(NewPackage, NewPackage->GetPackagePath()))
        {
            FAssetRegistry::Get().AssetCreated(NewStream);
        }
        else
        {
            LOG_ERROR("AudioStreamFactory: failed to save imported audio; asset will not be registered");
        }

        NewStream->ConditionalBeginDestroy();
    }
}
