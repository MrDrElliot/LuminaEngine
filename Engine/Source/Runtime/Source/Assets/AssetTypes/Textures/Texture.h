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

        using Super::Serialize;
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

        /** Push more of the staged image's host-uploaded mips, spending at most RemainingBytes. Each band is
         *  priced before it is copied, so the budget is never overshot -- except by the single block row
         *  bGuaranteeProgress forces through when nothing is affordable. Publishes the swap once the last
         *  band is queued. Returns true while there is still work left.
         *
         *  GRANT bGuaranteeProgress unless you have another way to finish the fill. Spreading a fill over
         *  frames is free only while it is still advancing: the staged image is invisible and its bindless
         *  slot is frozen until it completes, so a fill that is deferred indefinitely does not degrade
         *  gracefully -- it hangs the texture at the wrong residency and pins a slot nothing can reuse. A
         *  fill that stops advancing for long enough gives its staged image back rather than hang.
         *
         *  Game thread only, same as ApplyMipResidency. */
        bool TickResidencyFill(uint64& RemainingBytes, bool bGuaranteeProgress = true);

        /** Drive TickResidencyFill to completion right now, with no budget. For textures the streamer does
         *  not drive -- a non-streamable one is never registered, so nothing would ever drain its fill and
         *  its mips would simply never reach the GPU. Their whole chain is inline by definition, which is
         *  what makes an unmetered upload the right answer rather than a hitch. */
        void DrainResidencyFillNow();

        /** Undo a staged residency change that cannot be completed: the staged image is dropped, the slot
         *  keeps naming the one it already had, and ResidentFirstMip goes back to what it described. Doing
         *  nothing instead would strand the swap unarmed, which freezes the texture permanently. */
        void AbandonResidencyFill();

        /** Announce that something outside the streamer (an import, a re-cook) has replaced this texture's
         *  image and uploaded the WHOLE chain eagerly. Any half-drained residency fill describes the
         *  previous image and would upload into the new one at the wrong mip sizes, and the streamer's
         *  cached sizes no longer match the asset. Both are reset here. */
        void OnFullyUploadedExternally();

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

        // Host-uploaded half of a staged residency change, drained over frames by TickResidencyFill. The
        // cursor is (mip, layer, block row): a whole-mip step is 16 MiB, bigger than any sane frame budget.
        struct FResidencyFill
        {
            uint32 FirstMip   = 0;   // chain mip the staged image starts at
            uint32 PrevFirstMip = 0; // residency to restore if the staged image has to be abandoned
            uint32 NextMip    = 0;   // next chain mip owing a host upload
            uint32 NextLayer  = 0;   // next array layer of NextMip owing one
            uint32 NextRow    = 0;   // next BLOCK row of that (mip, layer) owing one
            uint32 CpuEndMip  = 0;   // one past the last chain mip owing one

            /** Consecutive ticks that moved the cursor nowhere. A staged image is invisible until it is
             *  committed, so a fill that stops advancing does not degrade -- it hangs, permanently and
             *  silently, and takes the texture's bindless slot with it. This is what makes that
             *  terminate. Reset by any band that lands. */
            uint32 StalledTicks = 0;

            bool   bActive    = false;
        };
        FResidencyFill PendingFill;

        /** Set when a residency change was abandoned for a reason that asking again cannot fix -- a cooked
         *  mip that is shorter than the image it has to fill. Without it the streamer re-requests the same
         *  level every frame, re-detects the same hole, and logs forever. Cleared by a re-cook, which is
         *  the only thing that actually replaces the bad mip. */
        bool bResidencyBlocked = false;
    };
}
