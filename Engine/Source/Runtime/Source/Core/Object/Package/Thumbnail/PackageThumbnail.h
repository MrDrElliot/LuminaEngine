#pragma once
#include "Core/Templates/LuminaTemplate.h"
#include "Core/Threading/Atomic.h"
#include "Containers/Array.h"
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

        TAtomic<EState> LoadState{EState::None};

        bool IsReadyForRender() const
        {
            return LoadState.load(std::memory_order_acquire) == EState::Loaded;
        }

        // ImageData is raw RGBA8 in memory; on disk it is PNG-compressed (lossless) to keep packages small.
        // Backward-compatible with legacy uncompressed thumbnails. Impl in PackageThumbnail.cpp.
        RUNTIME_API void Serialize(FArchive& Ar);
    };
}
