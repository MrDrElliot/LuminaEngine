#pragma once

#include "RHI.h"
#include "RHICore.h"

// New-RHI texture system: the destination for all engine textures. A managed
// texture owns its FTextureH, its sampled heap slot (= the shader ResourceID),
// and any per-mip storage slots. This is what replaces the old
// CreateImage + FTextureManager::AddTexture + WriteImage path; sampling resolves
// against RHI::Core::GetGlobalHeap(), so a ResourceID is a heap index usable
// directly as gTextures2D[ResourceID] in GlobalRHI.slang shaders.

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
    };

    namespace Textures
    {
        void Initialize();   // creates the 1x1 placeholder
        void Shutdown();

        // Render thread, per frame: retires heap slots/textures whose deferred
        // window has elapsed (called from RHI::Core::BeginFrame).
        void Tick();

        RUNTIME_API FManagedTexture Create(const FTexture2DDesc& Desc);

        // Upload tight pixel data for one mip. RowPitchTexels = 0 -> mip width.
        // Synchronous (asset-load time): stages, copies, waits, frees the staging.
        RUNTIME_API void Upload(const FManagedTexture& Tex, uint32 Mip, const void* Data, uint64 Size, uint32 RowPitchTexels = 0);

        // Synchronous clear of the whole texture to an RGBA float value (asset-load time).
        RUNTIME_API void Clear(const FManagedTexture& Tex, const float Value[4]);

        // Lazily create + register a per-mip storage (UAV) heap slot; index for gRWTextures*[].
        RUNTIME_API uint32 StorageSlot(const FManagedTexture& Tex, uint32 Mip);

        // Frame-deferred: the slot/texture are freed only after kFramesInFlight Ticks,
        // so an in-flight frame sampling the ResourceID never dangles.
        RUNTIME_API void Release(FManagedTexture& Tex);

        // ResourceID returned for invalid ids (1x1 magenta). Stable for the session.
        RUNTIME_API uint32 DefaultResourceID();
    }
}
