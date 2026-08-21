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
            // The texture editor reads extent and layer count before any layer exists, and PostLoad dereferences it.
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
            // maxImageArrayLayers is 2048 everywhere we ship, and a larger request fails with nothing in the log.
            LOG_ERROR("TextureArrayFactory: '{0}' asks for {1} layers; the hardware limit is 2048.",
                      Array->GetName().c_str(), LayerCount);
            return false;
        }

        // The real resource is STASHED so every failure path can restore it exactly.
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

            // An array nested inside itself would recurse through layer-major mips and silently produce garbage.
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
                // A streamed source has holes the copy cannot fill, so the array would depend on what the streamer did.
                Source->MakeStreamedMipsResident();

                // COPIED, not moved, so the source texture keeps owning its pixels and stays usable on its own.
                for (uint32 Mip = 0; Mip < SrcMips; ++Mip)
                {
                    const FTextureResource::FMip& SrcMip = Source->TextureResource->Mips[Mip];

                    // An array with a hole survives into the saved asset, so refusing beats rebuilding.
                    if (SrcMip.Pixels.empty())
                    {
                        LOG_ERROR("TextureArrayFactory: '{0}' layer {1} ('{2}') mip {3} could not be made "
                                  "resident, so the array would be built with a hole it can never fill. "
                                  "Leaving the previous array in place; see the errors above.",
                                  Array->GetName().c_str(), Layer, Source->GetName().c_str(), Mip);
                        return Rollback();
                    }

                    AssembledMips.push_back(SrcMip);

                    // The copy carries the SOURCE's BulkRef, which addresses the wrong package, so clear it here.
                    AssembledMips.back().BulkRef = FBulkDataRef{};
                }
                continue;
            }

            if (!Array->bResizeLayersToFirst)
            {
                // Every slice of one VkImage shares extent, format and mip count, so stretching stays opt-in.
                LOG_ERROR("TextureArrayFactory: '{0}' layer {1} ('{2}') is {3}x{4} fmt {5} with {6} mips, but layer 0 "
                          "established {7}x{8} fmt {9} with {10} mips. Every layer must match -- resize the source, "
                          "reorder so the intended size is layer 0, or enable 'Resize Layers To First' on the asset.",
                          Array->GetName().c_str(), Layer, Source->GetName().c_str(),
                          SrcDesc.Extent.x, SrcDesc.Extent.y, (uint32)SrcDesc.Format, SrcMips,
                          Contract.Extent.x, Contract.Extent.y, (uint32)Contract.Format, (uint32)Contract.NumMips);
                return Rollback();
            }

            // Re-cooked from the ORIGINAL source, since rescaling BCn would stack two generations of artifacts.
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
                // Resizing fixes dimensions but never FORMAT, and no rescaling reconciles BC5 with BC7.
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

        // Released and recreated, since the layer count is baked in and no same-shape image exists to swap.
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

        // This took a NEW bindless slot, so every material holding the old ResourceID now samples a stale one.
        AssetEvents::BroadcastAssetDataChanged(Array);

        LOG_INFO("TextureArrayFactory: rebuilt '{0}' with {1} layers at {2}x{3} ({4} mips).",
                 Array->GetName().c_str(), LayerCount,
                 Array->TextureResource->ImageDescription.Extent.x,
                 Array->TextureResource->ImageDescription.Extent.y,
                 (uint32)Array->TextureResource->ImageDescription.NumMips);
        return true;
    }
}
