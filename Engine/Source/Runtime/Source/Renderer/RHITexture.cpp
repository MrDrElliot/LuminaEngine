#include "RuntimePCH.h"
#include "RHITexture.h"
#include "Core/Templates/LuminaTemplate.h"

#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "Core/Profiler/Profile.h"

namespace Lumina::RHI::Textures
{
    struct FStorageSlot
    {
        uint64 TextureHandle;
        uint32 Mip;
        uint32 Slot;
    };

    /** A replacement image that exists, is being filled, and is NOT yet reachable by a shader.
     *
     *  Keyed by the bindless slot rather than by a pointer to the owning FManagedTexture: the slot is the
     *  stable identity here (that is the whole premise of Recreate), whereas the owner gets copied by
     *  value into release queues and asset structures, so a pointer to it would dangle. */
    struct FPendingSwap
    {
        uint32    Slot       = kInvalidHeapSlot;
        FTextureH NewTexture;                     // staged; bound once Batch has executed
        FTextureH OldTexture;                     // what the slot still names; retired after the repoint
        uint64    Batch      = 0;                 // the upload batch this swap waits on; only read when armed
        bool      bArmed     = false;             // CommitRecreate has run
        uint32    IdleTicks  = 0;                 // ticks spent unarmed, for the "forgot to commit" warning
    };

    struct FState
    {
        TVector<FStorageSlot> StorageSlots;
        FMutex                StorageMutex;

        TVector<FPendingSwap> PendingSwaps;
        FMutex                SwapMutex;

        FManagedTexture     Default;
        bool                bInitialized = false;
    };

    static FState GState;

    // Everything a texture owns, handed to the retire queue together: the storage (UAV) slots it
    // registered, any upload still queued against it, and the image itself. The sampled slot is NOT
    // included -- a swap keeps it and only a release gives it up.
    static void RetireTextureAndSlots(FTextureH Texture)
    {
        if (!IsValid(Texture))
        {
            return;
        }

        // Collected (and forgotten) under the lock, retired outside it, so the ordering
        // StorageMutex -> RetireMutex never has to be reasoned about.
        TVector<uint32> Storage;
        {
            FScopeLock Lock(GState.StorageMutex);
            for (size_t i = 0; i < GState.StorageSlots.size(); )
            {
                if (GState.StorageSlots[i].TextureHandle == Texture.Handle)
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

        Upload::CancelTexture(Texture);
        Core::Retire(Texture);
    }

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

        // Staged images were never bound, so nothing has to be repointed -- but they are real allocations
        // and the only reference to them lives here.
        {
            FScopeLock Lock(GState.SwapMutex);
            for (const FPendingSwap& Pending : GState.PendingSwaps)
            {
                Upload::CancelTexture(Pending.NewTexture);
                FreeH(Pending.NewTexture);
            }
            GState.PendingSwaps.clear();
        }

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

    // Both overloads are the same steps -- build the replacement and stage it against the slot -- differing
    // only in which description makes the image. Nothing here is visible to a shader; see the header.
    template<typename TDesc>
    static void RecreateInternal(FManagedTexture& Tex, const TDesc& Desc, const FTextureDesc& ImageDesc)
    {
        LUMINA_PROFILE_SECTION("Textures::Recreate");

        if (!Tex.IsValid() || Tex.SampledSlot == kInvalidHeapSlot)
        {
            // Nothing published to keep valid, so there is nothing to stage against: this is a first load
            // (or a texture that never got a slot), and the eager path is both correct and cheaper.
            Release(Tex);
            Tex = Create(Desc);
            return;
        }

        const FTextureH NewTexture = CreateTexture(ImageDesc);
        SetDebugName(NewTexture, Desc.DebugName);

        const FTextureH Previous = Tex.Texture;
        Tex.Texture = NewTexture;

        FTextureH Abandoned;
        {
            FScopeLock Lock(GState.SwapMutex);

            bool bMerged = false;
            for (FPendingSwap& Existing : GState.PendingSwaps)
            {
                if (Existing.Slot != Tex.SampledSlot)
                {
                    continue;
                }

                // A second Recreate before the first became visible. The image staged by that one was
                // never bound, so it can go; OldTexture is untouched because it is still what is sampled.
                Abandoned           = Existing.NewTexture;
                Existing.NewTexture = NewTexture;
                Existing.Batch      = 0;
                Existing.bArmed     = false;
                Existing.IdleTicks  = 0;
                bMerged             = true;
                break;
            }

            if (!bMerged)
            {
                GState.PendingSwaps.push_back(FPendingSwap{ Tex.SampledSlot, NewTexture, Previous, 0, false, 0 });
            }
        }

        // Outside the lock: retiring re-enters the upload and storage-slot bookkeeping.
        RetireTextureAndSlots(Abandoned);
    }

    void Recreate(FManagedTexture& Tex, const FTexture2DDesc& Desc)
    {
        RecreateInternal(Tex, Desc, MakeTexture2DDesc(Desc));
    }

    void Recreate(FManagedTexture& Tex, const FTexture2DArrayDesc& Desc)
    {
        RecreateInternal(Tex, Desc, MakeTexture2DArrayDesc(Desc));
    }

    void CommitRecreate(FManagedTexture& Tex)
    {
        if (Tex.SampledSlot == kInvalidHeapSlot)
        {
            return;
        }

        // Read AFTER the caller queued its uploads, which is what makes this the batch those uploads are
        // in (or, if a flush raced in between, a later one -- conservative, not wrong). It is legitimately
        // ZERO before the first flush of the session, which is why "armed" is its own flag rather than a
        // batch sentinel: 0 there means "already complete", not "never committed".
        const uint64 Batch = Upload::BatchForQueuedOps();

        FScopeLock Lock(GState.SwapMutex);
        for (FPendingSwap& Pending : GState.PendingSwaps)
        {
            if (Pending.Slot == Tex.SampledSlot)
            {
                Pending.Batch     = Batch;
                Pending.bArmed    = true;
                Pending.IdleTicks = 0;
                return;
            }
        }
    }

    void AbandonRecreate(FManagedTexture& Tex)
    {
        if (Tex.SampledSlot == kInvalidHeapSlot)
        {
            return;
        }

        FTextureH Staged;
        {
            FScopeLock Lock(GState.SwapMutex);
            for (size_t i = 0; i < GState.PendingSwaps.size(); ++i)
            {
                if (GState.PendingSwaps[i].Slot != Tex.SampledSlot)
                {
                    continue;
                }

                // Tex.Texture is the staged image -- Recreate pointed it there. Hand it back the one the
                // slot never stopped naming, or the caller would be left describing an image that is about
                // to be retired while its ResourceID still resolves to a different one.
                Staged      = GState.PendingSwaps[i].NewTexture;
                Tex.Texture = GState.PendingSwaps[i].OldTexture;

                GState.PendingSwaps[i] = GState.PendingSwaps.back();
                GState.PendingSwaps.pop_back();
                break;
            }
        }

        // Outside the lock: retiring re-enters the upload and storage-slot bookkeeping. Also cancels the
        // partial uploads already queued against it, which would otherwise copy into a dead image.
        RetireTextureAndSlots(Staged);
    }

    bool CopyMipFromCurrent(const FManagedTexture& Tex, uint32 Layer, uint32 SrcMip, uint32 DstMip, uint32 Width, uint32 Height)
    {
        if (Tex.SampledSlot == kInvalidHeapSlot)
        {
            return false;
        }

        FTextureH Source;
        {
            FScopeLock Lock(GState.SwapMutex);
            for (const FPendingSwap& Pending : GState.PendingSwaps)
            {
                if (Pending.Slot == Tex.SampledSlot)
                {
                    Source = Pending.OldTexture;
                    break;
                }
            }
        }

        if (!IsValid(Source))
        {
            return false;
        }

        UploadTextureCopy(Tex.Texture, Layer, DstMip, Source, Layer, SrcMip, Width, Height);
        return true;
    }

    bool HasPendingSwap(const FManagedTexture& Tex)
    {
        if (Tex.SampledSlot == kInvalidHeapSlot)
        {
            return false;
        }

        FScopeLock Lock(GState.SwapMutex);
        for (const FPendingSwap& Pending : GState.PendingSwaps)
        {
            if (Pending.Slot == Tex.SampledSlot)
            {
                return true;
            }
        }
        return false;
    }

    void TickPendingSwaps()
    {
        if (!GState.bInitialized)
        {
            return;
        }

        // Retired outside the lock: Core::Retire re-enters the upload system, which takes its own locks.
        TVector<FTextureH> Superseded;
        {
            FScopeLock Lock(GState.SwapMutex);
            for (size_t i = 0; i < GState.PendingSwaps.size(); )
            {
                FPendingSwap& Pending = GState.PendingSwaps[i];

                if (!Pending.bArmed)
                {
                    // Recreate ran and CommitRecreate never did. Deliberately NOT auto-armed: guessing a
                    // batch could publish an image whose upload has not happened, which is the exact
                    // failure this machinery exists to prevent. Stuck-and-loud beats visibly wrong.
                    // A caller that cannot finish what it staged should say so with AbandonRecreate.
                    if (++Pending.IdleTicks == 60)
                    {
                        LOG_ERROR("RHI: bindless slot {} has a staged texture that was never committed. "
                                  "Recreate must be followed by uploads and CommitRecreate on the same "
                                  "thread; until it is, the slot keeps sampling the previous image.",
                            Pending.Slot);
                    }
                    ++i;
                    continue;
                }

                if (!Upload::IsBatchComplete(Pending.Batch))
                {
                    ++i;
                    continue;
                }

                // The copies have executed, so the image now holds its texels. Only now does the slot move
                // -- and only after that does the image it moved off become garbage.
                HeapRepointTexture(Core::GetGlobalHeap(), Pending.Slot, Pending.NewTexture);
                Superseded.push_back(Pending.OldTexture);

                GState.PendingSwaps[i] = GState.PendingSwaps.back();
                GState.PendingSwaps.pop_back();
            }
        }

        for (FTextureH Texture : Superseded)
        {
            RetireTextureAndSlots(Texture);
        }
    }

    bool Upload(const FManagedTexture& Tex, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels, uint32 Width, uint32 Height)
    {
        return UploadTexture(Tex.Texture, 0, Mip, Data, Size, RowPitchTexels, Width, Height);
    }

    bool UploadLayer(const FManagedTexture& Tex, uint32 Layer, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels, uint32 Width, uint32 Height, uint32 OffsetY)
    {
        return UploadTexture(Tex.Texture, Layer, Mip, Data, Size, RowPitchTexels, Width, Height, OffsetY);
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
        // BOTH halves, not just the image. A managed texture with a live slot and a dead image is exactly
        // what a half-finished swap leaves behind, and bailing on the image alone leaked the heap slot AND
        // orphaned the pending-swap entry keyed on it -- which then sits unarmed forever, because the only
        // thing that could have cleared it was this call.
        if (!Tex.IsValid() && Tex.SampledSlot == kInvalidHeapSlot)
        {
            return;
        }

        // A staged swap owns the image the slot still names -- Tex.Texture is the replacement, which was
        // never bound. Both die here, and the entry has to go with them or the tick would repoint a slot
        // that no longer exists at an image that has been retired.
        FTextureH StillBound;
        if (Tex.SampledSlot != kInvalidHeapSlot)
        {
            FScopeLock Lock(GState.SwapMutex);
            for (size_t i = 0; i < GState.PendingSwaps.size(); ++i)
            {
                if (GState.PendingSwaps[i].Slot == Tex.SampledSlot)
                {
                    StillBound = GState.PendingSwaps[i].OldTexture;
                    GState.PendingSwaps[i] = GState.PendingSwaps.back();
                    GState.PendingSwaps.pop_back();
                    break;
                }
            }
        }

        // Unbinds the descriptor immediately (pointing it at the fallback) and defers only the index free,
        // so neither image is still named by the heap once they are retired below.
        Core::RetireSampledSlot(Tex.SampledSlot);

        RetireTextureAndSlots(StillBound);
        RetireTextureAndSlots(Tex.Texture);

        Tex = FManagedTexture{};
    }

    uint32 DefaultResourceID()
    {
        return GState.Default.SampledSlot;
    }
}
