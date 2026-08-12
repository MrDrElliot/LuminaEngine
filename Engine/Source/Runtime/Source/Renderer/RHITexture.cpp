#include "RuntimePCH.h"
#include "RHITexture.h"
#include "Core/Templates/LuminaTemplate.h"

#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"

namespace Lumina::RHI::Textures
{
    struct FStorageSlot
    {
        uint64 TextureHandle;
        uint32 Mip;
        uint32 Slot;
    };

    struct FState
    {
        TVector<FStorageSlot> StorageSlots;
        FMutex                StorageMutex;

        FManagedTexture     Default;
        bool                bInitialized = false;
    };

    static FState GState;

    void Initialize()
    {
        GState.bInitialized = true;

        GState.Default = Create(FTexture2DDesc{ .Width = 1, .Height = 1, .Format = EFormat::RGBA8_UNORM,
                                                .DebugName = "RHI.FallbackTexture" });
        const uint8 Magenta[4] = { 255, 0, 255, 255 };
        Upload(GState.Default, 0, Magenta, sizeof(Magenta), 1);
        FlushUploadsAndWait();

        HeapSetFallbackTexture(Core::GetGlobalHeap(), GState.Default.Texture);
    }

    void Shutdown()
    {
        if (!GState.bInitialized)
        {
            return;
        }

        WaitDeviceIdle();

        HeapSetFallbackTexture(Core::GetGlobalHeap(), FTextureH{});

        // Anything already retired is drained by Core::Shutdown, which runs immediately after this.

        if (GState.Default.SampledSlot != kInvalidHeapSlot)
        {
            HeapFreeTexture(Core::GetGlobalHeap(), GState.Default.SampledSlot);
        }
        FreeH(GState.Default.Texture);
        GState.Default = FManagedTexture{};

        GState.bInitialized = false;
    }

    static FTextureDesc MakeTexture2DDesc(const FTexture2DDesc& Desc)
    {
        FTextureDesc TextureDesc;
        TextureDesc.Type      = ETextureType::Tex2D;
        TextureDesc.Dimension = FUIntVector3(Math::Max(Desc.Width, 1u), Math::Max(Desc.Height, 1u), 1u);
        TextureDesc.MipCount  = Math::Max(Desc.Mips, 1u);
        TextureDesc.Format    = Desc.Format;
        TextureDesc.Usage     = EImageUsageFlags::Sampled | EImageUsageFlags::TransferDst | EImageUsageFlags::TransferSrc;
        if (Desc.bStorage)
        {
            TextureDesc.Usage |= EImageUsageFlags::Storage;
        }
        if (Desc.bRenderTarget)
        {
            TextureDesc.Usage |= EImageUsageFlags::ColorAttachment;
        }
        return TextureDesc;
    }

    static FTextureDesc MakeTexture2DArrayDesc(const FTexture2DArrayDesc& Desc)
    {
        FTextureDesc TextureDesc;
        TextureDesc.Type       = ETextureType::Tex2DArray;
        TextureDesc.Dimension  = FUIntVector3(Math::Max(Desc.Width, 1u), Math::Max(Desc.Height, 1u), 1u);
        TextureDesc.MipCount   = Math::Max(Desc.Mips, 1u);
        TextureDesc.LayerCount = Math::Max(Desc.Layers, 1u);
        TextureDesc.Format     = Desc.Format;
        TextureDesc.Usage      = EImageUsageFlags::Sampled | EImageUsageFlags::TransferDst | EImageUsageFlags::TransferSrc;
        if (Desc.bStorage)
        {
            TextureDesc.Usage |= EImageUsageFlags::Storage;
        }
        return TextureDesc;
    }

    static FTextureDesc MakeTexture3DDesc(const FTexture3DDesc& Desc)
    {
        FTextureDesc TextureDesc;
        TextureDesc.Type      = ETextureType::Tex3D;
        TextureDesc.Dimension = FUIntVector3(Math::Max(Desc.Width, 1u), Math::Max(Desc.Height, 1u), Math::Max(Desc.Depth, 1u));
        TextureDesc.MipCount  = Math::Max(Desc.Mips, 1u);
        TextureDesc.Format    = Desc.Format;
        TextureDesc.Usage     = EImageUsageFlags::Sampled | EImageUsageFlags::TransferDst | EImageUsageFlags::TransferSrc;
        if (Desc.bStorage)
        {
            TextureDesc.Usage |= EImageUsageFlags::Storage;
        }
        return TextureDesc;
    }

    FManagedTexture Create(const FTexture2DDesc& Desc)
    {
        FManagedTexture Out;
        Out.Texture     = CreateTexture(MakeTexture2DDesc(Desc));
        SetDebugName(Out.Texture, Desc.DebugName);
        Out.SampledSlot = HeapWriteTexture(Core::GetGlobalHeap(), Out.Texture);
        return Out;
    }

    FManagedTexture Create(const FTexture2DArrayDesc& Desc)
    {
        FManagedTexture Out;
        Out.Texture     = CreateTexture(MakeTexture2DArrayDesc(Desc));
        SetDebugName(Out.Texture, Desc.DebugName);
        Out.SampledSlot = HeapWriteTexture(Core::GetGlobalHeap(), Out.Texture);
        return Out;
    }

    FManagedTexture Create(const FTexture3DDesc& Desc)
    {
        FManagedTexture Out;
        Out.Texture     = CreateTexture(MakeTexture3DDesc(Desc));
        SetDebugName(Out.Texture, Desc.DebugName);
        Out.SampledSlot = HeapWriteTexture(Core::GetGlobalHeap(), Out.Texture);
        return Out;
    }

    // Both overloads are the same three steps -- build the replacement, move the slot onto it, retire the
    // old image -- differing only in which description makes the image.
    template<typename TDesc>
    static void RecreateInternal(FManagedTexture& Tex, const TDesc& Desc, const FTextureDesc& ImageDesc)
    {
        if (!Tex.IsValid())
        {
            Tex = Create(Desc);
            return;
        }

        FManagedTexture Old = Tex;
        const uint32 Slot = Old.SampledSlot;
        Old.SampledSlot = kInvalidHeapSlot;

        const FTextureH NewTexture = CreateTexture(ImageDesc);
        SetDebugName(NewTexture, Desc.DebugName);

        Tex.Texture = NewTexture;
        if (Slot != kInvalidHeapSlot)
        {
            HeapRepointTexture(Core::GetGlobalHeap(), Slot, NewTexture);
            Tex.SampledSlot = Slot;
        }
        else
        {
            Tex.SampledSlot = HeapWriteTexture(Core::GetGlobalHeap(), NewTexture);
        }

        // Deferred by kFramesInFlight, so frames already recorded against the old image still resolve.
        Release(Old);
    }

    void Recreate(FManagedTexture& Tex, const FTexture2DDesc& Desc)
    {
        RecreateInternal(Tex, Desc, MakeTexture2DDesc(Desc));
    }

    void Recreate(FManagedTexture& Tex, const FTexture2DArrayDesc& Desc)
    {
        RecreateInternal(Tex, Desc, MakeTexture2DArrayDesc(Desc));
    }

    void Upload(const FManagedTexture& Tex, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels, uint32 Width, uint32 Height)
    {
        UploadTexture(Tex.Texture, 0, Mip, Data, Size, RowPitchTexels, Width, Height);
    }

    void UploadLayer(const FManagedTexture& Tex, uint32 Layer, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels, uint32 Width, uint32 Height)
    {
        UploadTexture(Tex.Texture, Layer, Mip, Data, Size, RowPitchTexels, Width, Height);
    }

    void Clear(const FManagedTexture& Tex, const float Value[4])
    {
        UploadClearTexture(Tex.Texture, Value);
    }

    uint32 StorageSlot(const FManagedTexture& Tex, uint32 Mip)
    {
        const uint64 Handle = Tex.Texture.Handle;

        FScopeLock Lock(GState.StorageMutex);
        for (const FStorageSlot& Existing : GState.StorageSlots)
        {
            if (Existing.TextureHandle == Handle && Existing.Mip == Mip)
            {
                return Existing.Slot;
            }
        }

        const uint32 Slot = HeapWriteRWTexture(Core::GetGlobalHeap(), Tex.Texture, Mip);
        if (Slot != kInvalidHeapSlot)
        {
            GState.StorageSlots.push_back(FStorageSlot{ Handle, Mip, Slot });
        }
        return Slot;
    }

    void Release(FManagedTexture& Tex)
    {
        if (!Tex.IsValid())
        {
            return;
        }

        // Collect (and forget) any storage slots this texture registered. Retired outside the lock so
        // the ordering StorageMutex -> RetireMutex never has to be reasoned about.
        TVector<uint32> Storage;
        {
            const uint64 Handle = Tex.Texture.Handle;
            FScopeLock Lock(GState.StorageMutex);
            for (size_t i = 0; i < GState.StorageSlots.size(); )
            {
                if (GState.StorageSlots[i].TextureHandle == Handle)
                {
                    Storage.push_back(GState.StorageSlots[i].Slot);
                    GState.StorageSlots[i] = GState.StorageSlots.back();
                    GState.StorageSlots.pop_back();
                }
                else
                {
                    ++i;
                }
            }
        }

        for (uint32 Slot : Storage)
        {
            Core::RetireStorageSlot(Slot);
        }
        Core::RetireSampledSlot(Tex.SampledSlot);
        
        Upload::CancelTexture(Tex.Texture);
        Core::Retire(Tex.Texture);

        Tex = FManagedTexture{};
    }

    uint32 DefaultResourceID()
    {
        return GState.Default.SampledSlot;
    }
}
