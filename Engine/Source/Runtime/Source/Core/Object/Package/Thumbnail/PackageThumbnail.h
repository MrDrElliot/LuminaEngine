#pragma once
#include "Core/Templates/LuminaTemplate.h"
#include "Core/Threading/Atomic.h"
#include "Containers/Vector.h"
#include "Core/Serialization/Archiver.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/RHITexture.h"

namespace Lumina
{
    struct FPackageThumbnail
    {
        enum class EState : uint8
        {
            None,        // No request issued
            Requested,   // Async task enqueued
            Loading,     // GPU upload in progress
            Loaded,
            Failed
        };
        
        uint32 ImageWidth = 0;
        uint32 ImageHeight = 0;
        TVector<uint8> ImageData;
        RHI::FManagedTexture LoadedImage;

        ~FPackageThumbnail()
        {
            if (LoadedImage.IsValid())
            {
                RHI::Textures::Release(LoadedImage);
            }
        }
        FPackageThumbnail() = default;
        FPackageThumbnail(const FPackageThumbnail&) = delete;
        FPackageThumbnail& operator=(const FPackageThumbnail&) = delete;
        FPackageThumbnail(FPackageThumbnail&&) = delete;
        FPackageThumbnail& operator=(FPackageThumbnail&&) = delete;

        TAtomic<EState> LoadState{EState::None};

        bool IsReadyForRender() const
        {
            return LoadState.load(std::memory_order_acquire) == EState::Loaded;
        }

        // Raw RGBA8 in memory and lossless PNG on disk, while legacy uncompressed thumbnails still load.
        RUNTIME_API void Serialize(FArchive& Ar);
    };
}
