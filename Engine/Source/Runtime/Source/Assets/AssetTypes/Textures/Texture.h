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

    // Filtering a texture asks for when a material samples it with the sampler left at FromTexture.
    REFLECT()
    enum class ETextureFilter : uint8
    {
        // Inherit the group's filter.
        FromGroup,

        // Nearest-neighbor; for pixel art, lookup tables, and anything whose texels must not blend.
        Nearest,

        // Bilinear + trilinear between mips. The engine-wide default.
        Linear,

        // Trilinear plus 16x anisotropy; for surfaces viewed at grazing angles (terrain, floors, foliage).
        Anisotropic,
    };

    // How UVs outside [0,1] resolve. Applies to both axes; the stock sampler table has no per-axis entries.
    REFLECT()
    enum class ETextureAddress : uint8
    {
        Wrap,
        Clamp,
        Mirror,
    };

    // Whether a texture gets a mip chain at cook time.
    REFLECT()
    enum class ETextureMipGenSettings : uint8
    {
        // Inherit the group's policy.
        FromGroup,

        // Force a full chain even for a group that would skip it.
        Generate,

        // Force a single mip. Halves nothing on screen but drops ~33% of the asset and of GPU memory.
        NoMipmaps,
    };

    // Encoder effort. Cook time scales with it; the runtime format and size do not change.
    REFLECT()
    enum class ETextureCompressionQuality : uint8
    {
        Fastest,
        Default,
        High,
        Highest,
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

        Character,

        Weapon,

        Vehicle,

        // Particle and decal sheets; small, and usually alpha-blended where aniso buys nothing.
        Effects,

        // Grazing-angle surfaces, so anisotropic by default.
        Foliage,

        // Grazing-angle surfaces, so anisotropic by default.
        Terrain,

        // Always sampled at a fixed distance and never minified far, so streaming it only causes pop.
        Skybox,

        // Held at full residency because a cutscene cannot afford the pop-in a stream-up would show.
        Cinematic,
    };

    // What a group contributes before the texture's own settings override it, field by field.
    struct FTextureGroupPolicy
    {
        bool           bGenerateMips   = true;
        bool           bAllowStreaming = true;

        // Longest-edge cap applied at cook time; 0 leaves the source size alone.
        uint32         MaxDimension    = 0;

        ETextureFilter Filter          = ETextureFilter::Linear;
    };

    inline FTextureGroupPolicy GetTextureGroupPolicy(ETextureGroup Group)
    {
        FTextureGroupPolicy Policy;
        switch (Group)
        {
        case ETextureGroup::UI:
        case ETextureGroup::EditorIcon:
            Policy.bGenerateMips   = false;
            Policy.bAllowStreaming = false;
            break;

        case ETextureGroup::Foliage:
        case ETextureGroup::Terrain:
        case ETextureGroup::Character:
        case ETextureGroup::Weapon:
        case ETextureGroup::Vehicle:
            Policy.Filter = ETextureFilter::Anisotropic;
            break;

        case ETextureGroup::Skybox:
        case ETextureGroup::Cinematic:
            Policy.bAllowStreaming = false;
            break;

        case ETextureGroup::Effects:
        case ETextureGroup::World:
        default:
            break;
        }
        return Policy;
    }

    // Whether a group's textures get a mip chain at cook time.
    inline bool TextureGroupGeneratesMips(ETextureGroup Group)
    {
        return GetTextureGroupPolicy(Group).bGenerateMips;
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

        /** Read the stored source file back off disk. False if there is none. May block. */
        bool LoadSourceFileBytes();

        /** True when this texture carries the bytes it was imported from, resident or not. */
        bool HasSourceFile() const { return SourceFile.IsValid(); }

        /** First mip currently on the GPU; == GetFirstStreamedMipCount() when fully streamed out, 0 when
         *  fully resident. */
        uint32 GetResidentFirstMip() const { return TextureResource ? TextureResource->ResidentFirstMip : 0u; }

        bool IsStreamable() const { return TextureResource && TextureResource->IsStreamable(); }

        bool IsSRGB() const { return ColorSpace == ETextureColorSpace::SRGB; }

        // Group policy with this texture's own overrides folded in. The cook and the sampler both read it.
        FTextureGroupPolicy GetResolvedPolicy() const
        {
            FTextureGroupPolicy Policy = GetTextureGroupPolicy(Group);

            if (MipGenSettings != ETextureMipGenSettings::FromGroup)
            {
                Policy.bGenerateMips = (MipGenSettings == ETextureMipGenSettings::Generate);
            }
            if (Filter != ETextureFilter::FromGroup)
            {
                Policy.Filter = Filter;
            }
            if (MaxTextureSize > 0)
            {
                Policy.MaxDimension = MaxTextureSize;
            }
            if (bNeverStream)
            {
                Policy.bAllowStreaming = false;
            }

            // A single-mip texture has no chain to stream, so streaming it can only cost a bindless swap.
            if (!Policy.bGenerateMips)
            {
                Policy.bAllowStreaming = false;
            }
            return Policy;
        }

        // Stock heap slot a material samples this texture through when its node is left at FromTexture.
        RHI::EStockSampler GetStockSampler() const;

        // New-RHI global-heap ResourceID for sampling (gTextures2D[id]); -1 if not resident.
        int32 GetResourceID() const
        {
            return (TextureResource.get() && TextureResource->NewTexture.IsValid())
                 ? (int32)TextureResource->NewTexture.ResourceID() : -1;
        }

        PROPERTY(Editable, Category = "Compression", RequiresRecook)
        ETextureColorSpace ColorSpace = ETextureColorSpace::SRGB;

        // Encoder effort. Higher settings cost cook time only; the stored format and size are unchanged.
        PROPERTY(Editable, Category = "Compression", RequiresRecook)
        ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Default;

        // Forces alpha to opaque before encoding, so the encoder spends its whole bit budget on RGB.
        PROPERTY(Editable, Category = "Compression", RequiresRecook)
        bool bCompressWithoutAlpha = false;

        /** Usage class driving cook policy (mip generation). Changing it requires a re-cook to take effect. */
        PROPERTY(Editable, Category = "Level Of Detail", RequiresRecook)
        ETextureGroup Group = ETextureGroup::World;

        PROPERTY(Editable, Category = "Level Of Detail", RequiresRecook)
        ETextureMipGenSettings MipGenSettings = ETextureMipGenSettings::FromGroup;

        // Longest-edge cap applied at cook time; 0 defers to the group, which usually means no cap.
        PROPERTY(Editable, Category = "Level Of Detail", ClampMin = 0, RequiresRecook)
        uint32 MaxTextureSize = 0;

        // Keeps the whole chain resident and out of the streamer. Costs GPU memory, removes all pop-in.
        PROPERTY(Editable, Category = "Level Of Detail")
        bool bNeverStream = false;

        // Applied when a material samples this texture with its sampler left at FromTexture.
        PROPERTY(Editable, Category = "Sampling")
        ETextureFilter Filter = ETextureFilter::FromGroup;

        PROPERTY(Editable, Category = "Sampling")
        ETextureAddress AddressMode = ETextureAddress::Wrap;

        // Inverts G. Converts between OpenGL-style (+Y up) and DirectX-style (+Y down) normal maps.
        PROPERTY(Editable, Category = "Adjustments", RequiresRecook)
        bool bFlipGreenChannel = false;

        // Flips the source image vertically at cook time, for sources whose rows are bottom-up.
        PROPERTY(Editable, Category = "Adjustments", RequiresRecook)
        bool bFlipVertical = false;

        PROPERTY(Editable, Category = "Adjustments", RequiresRecook)
        bool bFlipHorizontal = false;

        PROPERTY(Editable, Category = "Adjustments", ClampMin = 0.0f, RequiresRecook)
        float AdjustBrightness = 1.0f;

        // Gamma-style curve on luminance; values above 1 darken midtones, below 1 lift them.
        PROPERTY(Editable, Category = "Adjustments", ClampMin = 0.0f, RequiresRecook)
        float AdjustBrightnessCurve = 1.0f;

        // Per-channel gamma applied after brightness.
        PROPERTY(Editable, Category = "Adjustments", ClampMin = 0.0f, RequiresRecook)
        float AdjustRGBCurve = 1.0f;

        // 0 is fully desaturated, 1 leaves the source alone, above 1 oversaturates.
        PROPERTY(Editable, Category = "Adjustments", ClampMin = 0.0f, RequiresRecook)
        float AdjustSaturation = 1.0f;

        // Saturation weighted toward the least saturated texels, so already-vivid colors are left alone.
        PROPERTY(Editable, Category = "Adjustments", ClampMin = 0.0f, ClampMax = 1.0f, RequiresRecook)
        float AdjustVibrance = 0.0f;

        PROPERTY(Editable, Category = "Adjustments", ClampMin = 0.0f, ClampMax = 360.0f, Units = "deg", RequiresRecook)
        float AdjustHue = 0.0f;

        // Remaps the alpha range; swapping min above max inverts alpha.
        PROPERTY(Editable, Category = "Adjustments", ClampMin = 0.0f, ClampMax = 1.0f, RequiresRecook)
        float AdjustMinAlpha = 0.0f;

        PROPERTY(Editable, Category = "Adjustments", ClampMin = 0.0f, ClampMax = 1.0f, RequiresRecook)
        float AdjustMaxAlpha = 1.0f;

        // Punches texels near ChromaKeyColor out to transparent black before encoding.
        PROPERTY(Editable, Category = "Adjustments", RequiresRecook)
        bool bChromaKey = false;

        PROPERTY(Editable, Color, Category = "Adjustments", RequiresRecook)
        FVector3 ChromaKeyColor = FVector3(0.0f, 1.0f, 0.0f);

        PROPERTY(Editable, Category = "Adjustments", ClampMin = 0.0f, ClampMax = 1.0f, RequiresRecook)
        float ChromaKeyThreshold = 1.0f / 255.0f;

        /** Source path persisted so the editor can re-cook after ColorSpace changes; empty for embedded. */
        PROPERTY()
        FString SourcePath;

        /** Bytes of the imported file, so every cook setting stays absolute even with the file gone. */
        FTextureSourceFile SourceFile;

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
