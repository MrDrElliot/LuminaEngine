#pragma once

#include "RHI.h"
#include "RHICore.h"

namespace Lumina::RHI
{
    struct FManagedTexture
    {
        FTextureH Texture;
        uint32    SampledSlot = kInvalidHeapSlot;   // ResourceID for FTexture2D

        uint32 ResourceID() const { return SampledSlot; }
        bool   IsValid() const { return RHI::IsValid(Texture); }
    };

    struct FTexture2DDesc
    {
        uint32  Width    = 1;
        uint32  Height   = 1;
        uint32  Mips     = 1;
        EFormat Format   = EFormat::RGBA8_UNORM;
        bool    bStorage = false;        // also allow per-mip UAV slots (compute writes)
        bool    bRenderTarget = false;   // usable as a color attachment (UI widget/brush RTs)

        const char* DebugName = nullptr;

        ETextureSwizzle Swizzle = ETextureSwizzle::Identity;
    };

    struct FTexture2DArrayDesc
    {
        uint32  Width    = 1;
        uint32  Height   = 1;
        uint32  Layers   = 1;
        uint32  Mips     = 1;
        EFormat Format   = EFormat::RGBA8_UNORM;
        bool    bStorage = false;        // also allow per-mip UAV slots (compute writes)

        const char* DebugName = nullptr;

        ETextureSwizzle Swizzle = ETextureSwizzle::Identity;
    };

    struct FTexture3DDesc
    {
        uint32  Width    = 1;
        uint32  Height   = 1;
        uint32  Depth    = 1;
        uint32  Mips     = 1;
        EFormat Format   = EFormat::R8_UNORM;
        bool    bStorage = false;        // also allow per-mip UAV slots (compute writes)

        const char* DebugName = nullptr;
    };

    namespace Textures
    {
        void Initialize();   // creates the 1x1 placeholder
        void Shutdown();


        RUNTIME_API FManagedTexture Create(const FTexture2DDesc& Desc);
        RUNTIME_API FManagedTexture Create(const FTexture2DArrayDesc& Desc);
        RUNTIME_API FManagedTexture Create(const FTexture3DDesc& Desc);

        // Stages a new image that keeps the bindless slot; upload every mip and CommitRecreate on one thread within one frame.
        RUNTIME_API void Recreate(FManagedTexture& Tex, const FTexture2DDesc& Desc);
        RUNTIME_API void Recreate(FManagedTexture& Tex, const FTexture2DArrayDesc& Desc);

        // The slot is repointed and the old image retired once the batch queued just before this completes on the GPU.
        RUNTIME_API void CommitRecreate(FManagedTexture& Tex);

        // A staged swap left unarmed freezes the slot forever, so a caller that cannot fill it must abandon it.
        RUNTIME_API void AbandonRecreate(FManagedTexture& Tex);

        // Copies a mip already on the GPU into the staged image instead of restaging it from the CPU.
        RUNTIME_API bool CopyMipFromCurrent(const FManagedTexture& Tex, uint32 Layer, uint32 SrcMip, uint32 DstMip, uint32 Width, uint32 Height);

        // A residency driver should wait for this to clear rather than stack a second swap on the slot.
        RUNTIME_API bool HasPendingSwap(const FManagedTexture& Tex);

        // Publishes every staged swap whose upload has landed. Driven from BeginFrame.
        void TickPendingSwaps();

        // Pass the mip's own Width and Height past mip 0, since a derived size faults the copy engine on non-power-of-two chains.
        RUNTIME_API bool UploadLayer(const FManagedTexture& Tex, uint32 Layer, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels = 0, uint32 Width = 0, uint32 Height = 0, uint32 OffsetY = 0);

        RUNTIME_API bool Upload(const FManagedTexture& Tex, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels = 0, uint32 Width = 0, uint32 Height = 0);

        // Queue a full-texture clear to an RGBA float value. Same deferred semantics as Upload.
        RUNTIME_API void Clear(const FManagedTexture& Tex, const float Value[4]);

        // Lazily create + register a per-mip storage (UAV) heap slot; index for the FRWTexture handles.
        RUNTIME_API uint32 StorageSlot(const FManagedTexture& Tex, uint32 Mip);

        RUNTIME_API void Release(FManagedTexture& Tex);

        // ResourceID returned for invalid ids (1x1 magenta). Stable for the session.
        RUNTIME_API uint32 DefaultResourceID();
    }
}
