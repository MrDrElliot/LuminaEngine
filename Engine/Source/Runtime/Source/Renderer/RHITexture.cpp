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

    struct FPendingRelease
    {
        FManagedTexture Texture;
        TVector<uint32> StorageSlots;
        uint32          TicksRemaining;
    };

    struct FState
    {
        TVector<FStorageSlot> StorageSlots;
        FMutex                StorageMutex;

        TVector<FPendingRelease> PendingReleases;
        FMutex                   ReleaseMutex;

        FManagedTexture     Default;
        bool                bInitialized = false;
    };

    static FState GState;

    void Initialize()
    {
        GState.bInitialized = true;

        // 1x1 magenta placeholder: distinct enough that a missing/invalid texture is obvious.
        // It backs every invalid ResourceID, so it must be resident before any sampling.
        GState.Default = Create(FTexture2DDesc{ .Width = 1, .Height = 1, .Format = EFormat::RGBA8_UNORM });
        const uint8 Magenta[4] = { 255, 0, 255, 255 };
        Upload(GState.Default, 0, Magenta, sizeof(Magenta), 1);
        FlushUploadsAndWait();

        // Every slot the heap hands back gets repointed here, so a stale ResourceID held past its
        // texture's death samples magenta instead of faulting on freed memory.
        HeapSetFallbackTexture(Core::GetGlobalHeap(), GState.Default.Texture);
    }

    void Shutdown()
    {
        if (!GState.bInitialized)
        {
            return;
        }

        WaitDeviceIdle();

        // Drop the fallback before the texture backing it dies, or the frees below would repoint
        // slots at a view this function is about to destroy.
        HeapSetFallbackTexture(Core::GetGlobalHeap(), FTextureH{});

        // Flush every deferred release immediately; the device is idle.
        {
            FScopeLock Lock(GState.ReleaseMutex);
            for (FPendingRelease& Pending : GState.PendingReleases)
            {
                for (uint32 Slot : Pending.StorageSlots)
                {
                    HeapFreeRWTexture(Core::GetGlobalHeap(), Slot);
                }
                if (Pending.Texture.SampledSlot != kInvalidHeapSlot)
                {
                    HeapFreeTexture(Core::GetGlobalHeap(), Pending.Texture.SampledSlot);
                }
                FreeH(Pending.Texture.Texture);
            }
            GState.PendingReleases.clear();
        }

        if (GState.Default.SampledSlot != kInvalidHeapSlot)
        {
            HeapFreeTexture(Core::GetGlobalHeap(), GState.Default.SampledSlot);
        }
        FreeH(GState.Default.Texture);
        GState.Default = FManagedTexture{};

        GState.bInitialized = false;
    }

    void Tick()
    {
        FScopeLock Lock(GState.ReleaseMutex);

        for (size_t i = 0; i < GState.PendingReleases.size(); )
        {
            FPendingRelease& Pending = GState.PendingReleases[i];
            if (Pending.TicksRemaining > 0)
            {
                --Pending.TicksRemaining;
                ++i;
                continue;
            }

            for (uint32 Slot : Pending.StorageSlots)
            {
                HeapFreeRWTexture(Core::GetGlobalHeap(), Slot);
            }
            if (Pending.Texture.SampledSlot != kInvalidHeapSlot)
            {
                HeapFreeTexture(Core::GetGlobalHeap(), Pending.Texture.SampledSlot);
            }
            FreeH(Pending.Texture.Texture);

            GState.PendingReleases[i] = Move(GState.PendingReleases.back());
            GState.PendingReleases.pop_back();
        }
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

    FManagedTexture Create(const FTexture3DDesc& Desc)
    {
        FManagedTexture Out;
        Out.Texture     = CreateTexture(MakeTexture3DDesc(Desc));
        SetDebugName(Out.Texture, Desc.DebugName);
        Out.SampledSlot = HeapWriteTexture(Core::GetGlobalHeap(), Out.Texture);
        return Out;
    }

    void Recreate(FManagedTexture& Tex, const FTexture2DDesc& Desc)
    {
        if (!Tex.IsValid())
        {
            Tex = Create(Desc);
            return;
        }

        // Detached from the slot before release, so the deferred drain frees the IMAGE and leaves the
        // slot alone (Tick skips HeapFreeTexture on kInvalidHeapSlot). The slot now belongs to the
        // replacement, exactly as DetachSampledSlot works for scene images.
        FManagedTexture Old = Tex;
        const uint32 Slot = Old.SampledSlot;
        Old.SampledSlot = kInvalidHeapSlot;

        const FTextureH NewTexture = CreateTexture(MakeTexture2DDesc(Desc));
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

    void Upload(const FManagedTexture& Tex, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels)
    {
        UploadTexture(Tex.Texture, Mip, Data, Size, RowPitchTexels);
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

        FPendingRelease Pending;
        Pending.Texture        = Tex;
        Pending.TicksRemaining = kFramesInFlight;

        // Collect (and forget) any storage slots this texture registered.
        {
            const uint64 Handle = Tex.Texture.Handle;
            FScopeLock Lock(GState.StorageMutex);
            for (size_t i = 0; i < GState.StorageSlots.size(); )
            {
                if (GState.StorageSlots[i].TextureHandle == Handle)
                {
                    Pending.StorageSlots.push_back(GState.StorageSlots[i].Slot);
                    GState.StorageSlots[i] = GState.StorageSlots.back();
                    GState.StorageSlots.pop_back();
                }
                else
                {
                    ++i;
                }
            }
        }

        {
            FScopeLock Lock(GState.ReleaseMutex);
            GState.PendingReleases.push_back(Move(Pending));
        }

        Tex = FManagedTexture{};
    }

    uint32 DefaultResourceID()
    {
        return GState.Default.SampledSlot;
    }
}
