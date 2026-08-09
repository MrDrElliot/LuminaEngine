#include "EditorPCH.h"
#include "AudioImporter.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Assets/Factories/Factory.h"
#include "Audio/AudioDecode.h"
#include "Core/Object/Package/Package.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Core/Progress/SlowTask.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Thumbnails/ThumbnailUtils.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
#if USING(WITH_EDITOR)
        // Renders a min/max waveform (channels mixed down) into the package thumbnail.
        void CreateWaveformThumbnail(CAudioStream* Stream)
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

        bool LoadStreamFromFile(CAudioStream* Stream, const FFixedString& SourcePath, FString& OutError)
        {
            TVector<uint8> Bytes;
            if (!FileHelper::LoadFileToArray(Bytes, SourcePath.c_str()) || Bytes.empty())
            {
                OutError = FString("Failed to read audio file ") + FString(SourcePath.c_str());
                return false;
            }

            Audio::FAudioInfo Info;
            if (!Audio::Probe(Bytes.data(), Bytes.size(), Info))
            {
                OutError = FString(SourcePath.c_str()) + FString(" is not a decodable audio file");
                return false;
            }

            Stream->SourcePath  = FString(SourcePath.c_str());
            Stream->SampleRate  = Info.SampleRate;
            Stream->NumChannels = Info.NumChannels;
            Stream->NumFrames   = Info.NumFrames;
            Stream->AudioData   = MakeShared<FAudioData>();
            Stream->AudioData->Bytes = Move(Bytes);

#if USING(WITH_EDITOR)
            CreateWaveformThumbnail(Stream);
#endif
            return true;
        }
    }

    void CAudioImporter::BuildAssets(const FImportRequest& Request, FImportResult& OutResult, FScopedSlowTask* Progress)
    {
        if (Progress)
        {
            Progress->EnterProgressFrame(0.5f, "Reading audio...");
        }

        FFixedString PackagePath = Request.DestinationPath;
        CPackage::AddPackageExt(PackagePath);

        CAudioStream* Stream = CFactory::CreateNewOf<CAudioStream>(PackagePath);
        if (Stream == nullptr)
        {
            OutResult.Error = FString("A package already exists at ") + FString(PackagePath.c_str());
            return;
        }

        if (!LoadStreamFromFile(Stream, Request.SourcePath, OutResult.Error))
        {
            Stream->ConditionalBeginDestroy();
            return;
        }

        if (Progress)
        {
            Progress->EnterProgressFrame(0.5f, "Saving package...");
        }

        CPackage* Package = Stream->GetPackage();
        if (CPackage::SavePackage(Package, Package->GetPackagePath()))
        {
            FAssetRegistry::Get().AssetCreated(Stream);
        }
        else
        {
            LOG_ERROR("[AudioImport] failed to save '{0}'; asset will not be registered", Package->GetPackagePath());
        }

        OutResult.CreatedObjects.push_back(Stream);
    }

    bool CAudioImporter::CanReimport(const CStruct* AssetClass) const
    {
        return AssetClass != nullptr && AssetClass->IsChildOf(CAudioStream::StaticClass());
    }

    FString CAudioImporter::GetReimportSourcePath(const CObject* Asset) const
    {
        const CAudioStream* Stream = Cast<CAudioStream>(Asset);
        return Stream != nullptr ? Stream->SourcePath : FString();
    }

    bool CAudioImporter::ReimportAsset(CObject* Asset, const FImportRequest& Request, FScopedSlowTask* Progress)
    {
        CAudioStream* Stream = Cast<CAudioStream>(Asset);
        if (Stream == nullptr)
        {
            return false;
        }

        FString Error;
        if (!LoadStreamFromFile(Stream, Request.SourcePath, Error))
        {
            LOG_ERROR("[AudioImport] reimport failed: {0}", Error);
            return false;
        }

        if (CPackage* Package = Stream->GetPackage())
        {
            Package->MarkDirty();
        }
        return true;
    }
}
