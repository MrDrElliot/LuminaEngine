#pragma once

#include "Core/Object/Object.h"
#include "Memory/RefCounted.h"
#include "Memory/SmartPtr.h"
#include "Renderer/RenderResource.h"
#include "Renderer/TextureData.h"
#include "Texture.generated.h"

namespace Lumina
{
    /** Drives import-time format selection; SRGB textures use *_UNORM_SRGB so the sampler does gamma decode. */
    REFLECT()
    enum class ETextureColorSpace : uint8
    {
        // Filename-derived heuristic; resolved at import time and rewritten to a concrete entry.
        Auto,

        // Linear-encoded data textures (custom masks, etc).
        Linear,

        // sRGB color (albedo, emissive, UI). Default for unrecognized filenames.
        SRGB,

        // Tangent-space normal map; stored BC5_UNORM (RG), shader reconstructs Z.
        NormalMap,

        // Packed PBR data (ORM/MRA/etc); stored BC7_UNORM.
        PackedData,

        // HDR equirectangular panorama; stored uncompressed float16 so IBL convolution sees radiances.
        Environment,
    };

    /**
     * Coarse usage class, in the spirit of Unreal's TextureGroup/LODGroup. Drives cook-time policy
     * rather than runtime behavior, so the savings are in the asset AND in GPU memory: a texture
     * cooked without a mip chain has none to upload.
     */
    REFLECT()
    enum class ETextureGroup : uint8
    {
        // Sampled at varying distance and minified: needs a full mip chain.
        World,

        // Drawn screen-aligned at (or near) 1:1 by the UI pass, so minified mips are never sampled.
        // Skipping them saves ~33% of the texture's memory and the cook time to produce them.
        UI,

        // Same rationale as UI: sampled at a fixed on-screen size.
        EditorIcon,
    };

    // Whether a group's textures get a mip chain at cook time. Kept as a function rather than a field
    // on the enum so adding per-group policy later (max size, compression) has an obvious home.
    inline bool TextureGroupGeneratesMips(ETextureGroup Group)
    {
        switch (Group)
        {
        case ETextureGroup::UI:
        case ETextureGroup::EditorIcon:
            return false;
        case ETextureGroup::World:
        default:
            return true;
        }
    }

    REFLECT()
    class RUNTIME_API CTexture : public CObject
    {
        GENERATED_BODY()

    public:

        void Serialize(FArchive& Ar) override;
        void PreLoad() override;
        void PostLoad() override;
        void PreSave() override;
        void OnDestroy() override;
        bool IsAsset() const override { return true; }


        FTextureResource& GetTextureResource() const { return *TextureResource.get(); }
        uint8 GetNumMips() const { return TextureResource.get() ? TextureResource->Mips.size() : 0u; }

        /** (Re)build the GPU image to hold mips [InFirstMip, NumMips) and upload them. The bindless slot is
         *  preserved, so materials that already baked this texture's ResourceID keep sampling it -- they
         *  just see a smaller image, and normalized UVs make that invisible.
         *
         *  Every mip from InFirstMip up must have its Pixels resident before calling: Recreate makes a NEW
         *  image, so even mips that were already on the GPU have to be re-uploaded. Returns false and leaves
         *  residency untouched if any of them is missing. Game thread only. */
        bool ApplyMipResidency(uint32 InFirstMip);

        /** True while the last ApplyMipResidency has been staged but not published: its image exists and is
         *  being filled, and the bindless slot still names the previous one. ApplyMipResidency refuses to
         *  run again until this clears, so a caller holding data to apply should wait rather than spend it. */
        bool HasPendingGPUResidency() const;

        /** Push more of the staged image's host-uploaded mips, spending at most RemainingBytes. Each mip is
         *  priced before it is copied, so the budget is never overshot -- except when bMayExceedBudget lets
         *  one oversized mip through, which the caller must grant at most once per FRAME or a mip larger
         *  than the whole budget would never converge. Publishes the swap once the last one is queued.
         *  Returns true while there is still work left.
         *
         *  The staged image is invisible until it is complete, so spreading the fill over frames is free:
         *  nothing samples a half-filled image, and the old one keeps being sampled meanwhile. Game thread
         *  only, same as ApplyMipResidency. */
        bool TickResidencyFill(uint64& RemainingBytes, bool bMayExceedBudget = true);

        /** Pull every streamed-out mip's bytes back off disk, so the whole chain is in memory. The
         *  precondition for ANY write: a mip that is only a BulkRef serializes as a zero-length payload.
         *  No-op (and no IO) when nothing has been streamed out. May block. */
        void MakeStreamedMipsResident();

        /** First mip currently on the GPU; == GetFirstStreamedMipCount() when fully streamed out, 0 when
         *  fully resident. */
        uint32 GetResidentFirstMip() const { return TextureResource ? TextureResource->ResidentFirstMip : 0u; }

        bool IsStreamable() const { return TextureResource && TextureResource->IsStreamable(); }

        // New-RHI global-heap ResourceID for sampling (gTextures2D[id]); -1 if not resident.
        int32 GetResourceID() const
        {
            return (TextureResource.get() && TextureResource->NewTexture.IsValid())
                 ? (int32)TextureResource->NewTexture.ResourceID() : -1;
        }

        PROPERTY(Editable)
        ETextureColorSpace ColorSpace = ETextureColorSpace::SRGB;

        /** Usage class driving cook policy (mip generation). Changing it requires a re-cook to take effect. */
        PROPERTY(Editable)
        ETextureGroup Group = ETextureGroup::World;

        /** Source path persisted so the editor can re-cook after ColorSpace changes; empty for embedded. */
        PROPERTY()
        FString SourcePath;

        TUniquePtr<FTextureResource> TextureResource;

        /** The host-uploaded half of a staged residency change, drained over frames by TickResidencyFill.
         *  Mips at or above CpuEndMip came from the previous image by GPU copy and are already there.
         *
         *  The cursor is (mip, layer, block row) rather than just a mip: one 4K mip is 16 MiB of copy into
         *  write-combined memory, so a whole-mip step made MaxUploadMBPerFrame unenforceable -- the budget
         *  could only ever be checked BETWEEN mips, and the smallest possible step was already bigger than
         *  any sane budget. Bands make the budget mean what it says. */
        struct FResidencyFill
        {
            uint32 FirstMip   = 0;   // chain mip the staged image starts at
            uint32 NextMip    = 0;   // next chain mip owing a host upload
            uint32 NextLayer  = 0;   // next array layer of NextMip owing one
            uint32 NextRow    = 0;   // next BLOCK row of that (mip, layer) owing one
            uint32 CpuEndMip  = 0;   // one past the last chain mip owing one
            bool   bActive    = false;
        };
        FResidencyFill PendingFill;
    };
}
