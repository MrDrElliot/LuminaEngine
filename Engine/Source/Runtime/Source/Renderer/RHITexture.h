#pragma once

#include "RHI.h"
#include "RHICore.h"

namespace Lumina::RHI
{
    struct FManagedTexture
    {
        FTextureH Texture;
        uint32    SampledSlot = kInvalidHeapSlot;   // ResourceID for gTextures2D[]

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

        /** Rebuild the image behind Tex at a new size/mip count, KEEPING its bindless slot: the slot is
         *  repointed rather than freed, because its ResourceID is baked into every material uniform block
         *  that samples the texture and is never revisited. This is what makes mip streaming possible --
         *  see the residency invariants in CTexture::ApplyMipResidency.
         *
         *  The array overload changes the mip count only; layer count is fixed at cook time and a caller
         *  that wants a different one is describing a different texture. */
        RUNTIME_API void Recreate(FManagedTexture& Tex, const FTexture2DDesc& Desc);
        RUNTIME_API void Recreate(FManagedTexture& Tex, const FTexture2DArrayDesc& Desc);

        // Width/Height are the MIP's own dimensions and must be passed past mip 0: the copy otherwise derives
        // (Base >> Mip), which disagrees with a cooked chain and faults the copy engine on non-power-of-two.
        RUNTIME_API void UploadLayer(const FManagedTexture& Tex, uint32 Layer, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels = 0, uint32 Width = 0, uint32 Height = 0);

        RUNTIME_API void Upload(const FManagedTexture& Tex, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels = 0, uint32 Width = 0, uint32 Height = 0);

        // Queue a full-texture clear to an RGBA float value. Same deferred semantics as Upload.
        RUNTIME_API void Clear(const FManagedTexture& Tex, const float Value[4]);

        // Lazily create + register a per-mip storage (UAV) heap slot; index for gRWTextures*[].
        RUNTIME_API uint32 StorageSlot(const FManagedTexture& Tex, uint32 Mip);

        RUNTIME_API void Release(FManagedTexture& Tex);

        // ResourceID returned for invalid ids (1x1 magenta). Stable for the session.
        RUNTIME_API uint32 DefaultResourceID();
    }
}
