#include "EditorPCH.h"
#include "TextureArrayFactory.h"
#include "TextureFactory.h"
#include "Assets/AssetEvents.h"
#include "Assets/AssetTypes/Textures/TextureArray.h"
#include "Core/Object/Package/Package.h"
#include "Renderer/RHITexture.h"
#include "Log/Log.h"

namespace Lumina
{
    CObject* CTextureArrayFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CTextureArray* Array = NewObject<CTextureArray>(Package, Name);
        if (Array != nullptr && Array->TextureResource == nullptr)
        {
            // An empty array still needs a resource: the texture editor reads extent/layer count off it
            // before any layer has been added, and CTexture::PostLoad dereferences it unconditionally.
            Array->TextureResource = MakeUnique<FTextureResource>();
        }
        return Array;
    }

    bool CTextureArrayFactory::Rebuild(CTextureArray* Array)
    {
        if (Array == nullptr)
        {
            return false;
        }

        const uint32 LayerCount = (uint32)Array->SourceTextures.size();
        if (LayerCount == 0)
        {
            LOG_ERROR("TextureArrayFactory: '{0}' has no source textures to build from.", Array->GetName().c_str());
            return false;
        }

        if (LayerCount > 2048)
        {
            // maxImageArrayLayers is 2048 on every target we support; a larger request fails at
            // vkCreateImage with nothing useful in the log, so name the real limit here instead.
            LOG_ERROR("TextureArrayFactory: '{0}' asks for {1} layers; the hardware limit is 2048.",
                      Array->GetName().c_str(), LayerCount);
            return false;
        }

        // The asset's real resource is STASHED and a throwaway put in its place for the duration, so
        // every failure path can restore it exactly. It also gives the resize path (which cooks through
        // a CTexture) somewhere to write that is not the live asset.
        TUniquePtr<FTextureResource> Saved = Move(Array->TextureResource);
        Array->TextureResource = MakeUnique<FTextureResource>();

        // Restores the asset untouched. Every failure path below goes through this.
        auto Rollback = [&Array, &Saved]() -> bool
        {
            Array->TextureResource = Move(Saved);
            return false;
        };

        FTextureResource::FDescription          Contract;
        TFixedVector<FTextureResource::FMip, 1> AssembledMips;
        bool                                    bHaveContract = false;

        for (uint32 Layer = 0; Layer < LayerCount; ++Layer)
        {
            const TObjectPtr<CTexture>& Source = Array->SourceTextures[Layer];
            if (!Source.IsValid())
            {
                LOG_ERROR("TextureArrayFactory: '{0}' layer {1} has no texture assigned.",
                          Array->GetName().c_str(), Layer);
                return Rollback();
            }

            // Guarding against an array nested inside itself, which would recurse through mips that
            // are laid out layer-major and produce a silently wrong image rather than an error.
            if (Source->IsA<CTextureArray>())
            {
                LOG_ERROR("TextureArrayFactory: '{0}' layer {1} ('{2}') is itself a texture array; layers must be plain textures.",
                          Array->GetName().c_str(), Layer, Source->GetName().c_str());
                return Rollback();
            }

            if (Source->TextureResource == nullptr || Source->TextureResource->Mips.empty())
            {
                LOG_ERROR("TextureArrayFactory: '{0}' layer {1} ('{2}') has no cooked pixel data.",
                          Array->GetName().c_str(), Layer, Source->GetName().c_str());
                return Rollback();
            }

            const FTextureResource::FDescription& SrcDesc = Source->TextureResource->ImageDescription;
            const uint32 SrcMips = (uint32)Source->TextureResource->Mips.size();

            if (!bHaveContract)
            {
                Contract      = SrcDesc;
                Contract.NumMips = (uint8)SrcMips;
                bHaveContract = true;
            }

            const bool bMatches = (SrcDesc.Extent == Contract.Extent)
                               && (SrcDesc.Format == Contract.Format)
                               && (SrcMips        == (uint32)Contract.NumMips);

            if (bMatches)
            {
                // A streamed source is sitting at its inline tail, so the mips below FirstInlineMip have
                // no Pixels at all -- their bytes are still in the source package's bulk region. Copying
                // in that state assembles the array around holes, and because the copy cannot bring the
                // source's BulkRef with it (see below), those holes are unfillable: the array can never
                // stream up past them, and saving it writes them out as zero-length payloads. Which mips
                // happen to be resident depends on what the streamer did to the source minutes ago, so
                // without this the array you get is a function of the camera, not of the assets.
                Source->MakeStreamedMipsResident();

                // The fast path, and the common one: the layer is already cooked to the right shape, so
                // its mip chain is copied straight across. COPIED, not moved -- the source texture keeps
                // owning its pixels and stays perfectly usable on its own.
                for (uint32 Mip = 0; Mip < SrcMips; ++Mip)
                {
                    const FTextureResource::FMip& SrcMip = Source->TextureResource->Mips[Mip];

                    // MakeStreamedMipsResident logs its own reason. Refusing here is the point: an array
                    // with a hole in it is worse than no rebuild, because the hole survives into the saved
                    // asset and the old array was fine.
                    if (SrcMip.Pixels.empty())
                    {
                        LOG_ERROR("TextureArrayFactory: '{0}' layer {1} ('{2}') mip {3} could not be made "
                                  "resident, so the array would be built with a hole it can never fill. "
                                  "Leaving the previous array in place; see the errors above.",
                                  Array->GetName().c_str(), Layer, Source->GetName().c_str(), Mip);
                        return Rollback();
                    }

                    AssembledMips.push_back(SrcMip);

                    // The copy brings the SOURCE's BulkRef with it, and that addresses the source's
                    // package, not this array's. Left in place, the first demotion would drop these
                    // pixels (a valid BulkRef is what marks them re-readable) and the promotion after it
                    // would read the array's package at the source's offsets -- another texture's bytes.
                    // The array's own refs are written when it is saved.
                    AssembledMips.back().BulkRef = FBulkDataRef{};
                }
                continue;
            }

            if (!Array->bResizeLayersToFirst)
            {
                // Every slice of one VkImage shares extent, format and mip count. Stretching to fit
                // distorts anything with a different aspect ratio, so that stays opt-in -- but say so
                // here, because the option is the answer nearly every time this fires.
                LOG_ERROR("TextureArrayFactory: '{0}' layer {1} ('{2}') is {3}x{4} fmt {5} with {6} mips, but layer 0 "
                          "established {7}x{8} fmt {9} with {10} mips. Every layer must match -- resize the source, "
                          "reorder so the intended size is layer 0, or enable 'Resize Layers To First' on the asset.",
                          Array->GetName().c_str(), Layer, Source->GetName().c_str(),
                          SrcDesc.Extent.x, SrcDesc.Extent.y, (uint32)SrcDesc.Format, SrcMips,
                          Contract.Extent.x, Contract.Extent.y, (uint32)Contract.Format, (uint32)Contract.NumMips);
                return Rollback();
            }

            // Resize path. Re-cooked from the texture's ORIGINAL source file at the contract size:
            // rescaling the cooked BCn blocks instead would mean decode, resample and re-encode, so the
            // layer would carry two generations of block artifacts.
            if (Source->SourcePath.empty())
            {
                LOG_ERROR("TextureArrayFactory: '{0}' layer {1} ('{2}') needs resizing to {3}x{4}, but it has no source "
                          "file on disk to re-cook from (mesh-embedded textures can't be resized).",
                          Array->GetName().c_str(), Layer, Source->GetName().c_str(),
                          Contract.Extent.x, Contract.Extent.y);
                return Rollback();
            }

            // Cooks into the throwaway resource standing in for the asset's own; harvested immediately.
            if (!CTextureFactory::CookLayerFromFile(Array, Source->SourcePath, Array->ColorSpace, Contract.Extent))
            {
                LOG_ERROR("TextureArrayFactory: '{0}' layer {1} ('{2}') failed to re-cook from '{3}'.",
                          Array->GetName().c_str(), Layer, Source->GetName().c_str(), Source->SourcePath.c_str());
                return Rollback();
            }

            const FTextureResource::FDescription& Recooked = Array->TextureResource->ImageDescription;
            const uint32 RecookedMips = (uint32)Array->TextureResource->Mips.size();
            if (Recooked.Extent != Contract.Extent
             || Recooked.Format != Contract.Format
             || RecookedMips    != (uint32)Contract.NumMips)
            {
                // Resizing fixes dimensions, never FORMAT: a normal map cooks to BC5 and an albedo to
                // BC7, and no amount of rescaling reconciles those.
                LOG_ERROR("TextureArrayFactory: '{0}' layer {1} ('{2}') still doesn't match after resizing "
                          "({3}x{4} fmt {5} with {6} mips vs {7}x{8} fmt {9} with {10} mips). Layers must share a "
                          "color space so they cook to the same format.",
                          Array->GetName().c_str(), Layer, Source->GetName().c_str(),
                          Recooked.Extent.x, Recooked.Extent.y, (uint32)Recooked.Format, RecookedMips,
                          Contract.Extent.x, Contract.Extent.y, (uint32)Contract.Format, (uint32)Contract.NumMips);
                return Rollback();
            }

            for (uint32 Mip = 0; Mip < RecookedMips; ++Mip)
            {
                AssembledMips.push_back(Move(Array->TextureResource->Mips[Mip]));
            }
        }

        Contract.LayerCount = (uint16)LayerCount;

        Array->TextureResource->ImageDescription = Contract;
        Array->TextureResource->Mips             = Move(AssembledMips);

        // Success. The STASHED resource is what still owns the previous GPU image, so release it here
        // rather than let Saved go out of scope holding a live handle. Released + recreated rather than
        // repointed: the layer count is baked into the image, so unlike a plain re-cook there is no
        // same-shape image to swap underneath the heap slot.
        if (Saved)
        {
            RHI::Textures::Release(Saved->NewTexture);
        }

        // Creates the Tex2DArray and uploads every layer; the resource currently has no GPU image.
        Array->PostLoad();

        if (CPackage* Package = Array->GetPackage())
        {
            Package->MarkDirty();
        }

        // Unlike a plain re-cook, this took a NEW bindless slot (the layer count is baked into the image,
        // so there was no same-shape image to repoint). Every material that already baked the old
        // ResourceID is now sampling a slot this array no longer owns, and nothing else rewrites them.
        AssetEvents::BroadcastAssetDataChanged(Array);

        LOG_INFO("TextureArrayFactory: rebuilt '{0}' with {1} layers at {2}x{3} ({4} mips).",
                 Array->GetName().c_str(), LayerCount,
                 Array->TextureResource->ImageDescription.Extent.x,
                 Array->TextureResource->ImageDescription.Extent.y,
                 (uint32)Array->TextureResource->ImageDescription.NumMips);
        return true;
    }
}
