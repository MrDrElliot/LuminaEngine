#include "RuntimePCH.h"
#include "Texture.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Memory/MemoryTracking.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHITexture.h"
#include "Renderer/TextureStreamingManager.h"

namespace Lumina
{
    void CTexture::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Textures");

        Super::Serialize(Ar);

        if (!TextureResource)
        {
            TextureResource = MakeUnique<FTextureResource>();
        }

        // Every write needs the pixels in hand: an archive with a bulk region copies them into it, and one
        // without has no choice but to write them inline. A streamed-out mip has neither. PreSave covers the
        // package saver, but it is a CObject hook and NOTHING else calls it -- so a texture reached through
        // any other writing archive (duplication, transient, network) would serialize its streamed mips as
        // zero-length payloads and come out capped at its inline tail, permanently.
        if (Ar.IsWriting())
        {
            MakeStreamedMipsResident();
        }

        Ar << *TextureResource.get();
    }

    void CTexture::PreLoad()
    {
        if (TextureResource == nullptr)
        {
            TextureResource = MakeUnique<FTextureResource>();
        }
    }

    void CTexture::PreSave()
    {
        // Runs before the export's offset is recorded, so the IO below cannot land between that offset and
        // the object's own bytes. Serialize repeats the call for archives the package saver never touches;
        // by then this has already made everything resident, so that one does no IO at all.
        MakeStreamedMipsResident();
    }

    void CTexture::MakeStreamedMipsResident()
    {
        // A streamed-out mip has no Pixels -- its bytes are still in the package's bulk region on disk. If it
        // reached Serialize like that it would be written back as a zero-length payload, which is silent
        // permanent data loss, so pull everything back in first. Only mips actually missing are read; a
        // freshly imported texture (all Pixels populated, no BulkRefs) does no IO at all.
        if (TextureResource == nullptr)
        {
            return;
        }

        CPackage* Package = nullptr;

        for (FTextureResource::FMip& Mip : TextureResource->Mips)
        {
            if (!Mip.BulkRef.IsValid() || !Mip.Pixels.empty())
            {
                continue;
            }

            // Resolved lazily: a texture with nothing to re-read (the common case by far) must not pay for
            // the lookup, and one with no package at all is only worth complaining about if it actually
            // needs bytes it cannot get.
            if (Package == nullptr)
            {
                Package = GetPackage();
                if (Package == nullptr)
                {
                    LOG_ERROR("CTexture: {} has streamed-out mips but no package to re-read them from; "
                              "writing it now would drop them", GetName());
                    return;
                }
            }

            if (!Package->ReadBulkData(Mip.BulkRef, Mip.Pixels))
            {
                LOG_ERROR("CTexture: {} could not re-read a streamed mip; saving would drop it", GetName());
            }
        }
    }

    void CTexture::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Textures");

        // Only the inline tail is resident at load. For a streamable texture that is the mips at or below
        // kInlineMipMaxDimension -- tiny next to the full chain, which is the entire point: opening fifteen
        // 4K textures now costs fifteen 256px images until something actually asks for more.
        TextureResource->ResidentFirstMip = TextureResource->ImageDescription.FirstInlineMip;

        if (ApplyMipResidency(TextureResource->ResidentFirstMip))
        {
            if (FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet())
            {
                Streaming->RegisterTexture(this);
            }
        }

#if !USING(WITH_EDITOR)
        // CPU pixels are dead after upload in cooked builds; editor retains them for reimport/thumbnails.
        //
        // Except the inline tail of a streamable texture. Demotion re-uploads the tail into a fresh image
        // (Recreate does not preserve contents), and the tail is the one part of the chain that is NOT in
        // the bulk region, so there would be nothing to re-read it from -- dropping it here would make the
        // texture permanently un-demotable and, worse, blank on the first demotion. It is small by
        // construction: ~87 KiB for a 4K BC7 against the ~21 MiB it lets us stream.
        const uint32 KeepFrom = TextureResource->IsStreamable()
            ? (uint32)TextureResource->ImageDescription.FirstInlineMip
            : TextureResource->GetNumMips();

        const uint32 NumMipsTotal = TextureResource->GetNumMips();
        for (uint32 Index = 0; Index < (uint32)TextureResource->Mips.size(); ++Index)
        {
            if (NumMipsTotal > 0 && (Index % NumMipsTotal) >= KeepFrom)
            {
                continue;
            }

            FTextureResource::FMip& Mip = TextureResource->Mips[Index];
            Mip.Pixels.clear();
            Mip.Pixels.shrink_to_fit();
        }
#endif
    }

    bool CTexture::HasPendingGPUResidency() const
    {
        return TextureResource != nullptr && RHI::Textures::HasPendingSwap(TextureResource->NewTexture);
    }

    bool CTexture::ApplyMipResidency(uint32 InFirstMip)
    {
        LUMINA_MEMORY_SCOPE("Textures");

        if (TextureResource == nullptr)
        {
            return false;
        }

        // A previous residency change for this texture has been staged but is not visible yet, because its
        // upload has not executed on the GPU. Stacking a second one would abandon that image mid-flight
        // and start the clock again; the caller retries, which is free -- the pixels are already in memory.
        if (RHI::Textures::HasPendingSwap(TextureResource->NewTexture))
        {
            return false;
        }

        const FTextureResource::FDescription& Desc = TextureResource->ImageDescription;

        const uint32 NumMips   = TextureResource->GetNumMips();
        const uint32 NumLayers = TextureResource->GetNumLayers();

        if (InFirstMip >= NumMips)
        {
            InFirstMip = NumMips - 1;
        }

        // Every mip the new image will hold has to be uploadable, because Recreate hands back a fresh image
        // with nothing in it. Bail before touching the GPU rather than leave a half-filled texture.
        for (uint32 Layer = 0; Layer < NumLayers; ++Layer)
        {
            for (uint32 Mip = InFirstMip; Mip < NumMips; ++Mip)
            {
                const uint32 Index = TextureResource->MipIndex(Layer, Mip);
                if (Index < TextureResource->Mips.size() && TextureResource->Mips[Index].Pixels.empty())
                {
                    LOG_ERROR("CTexture::ApplyMipResidency: {} asked for mip {} but its pixels are not resident",
                        GetName(), Mip);
                    return false;
                }
            }
        }

        // Named after the asset so a GPU crash report identifies which texture a faulting address
        // belongs to. Read during Create only, so the local outliving the call is enough.
        const FString DebugName = "Texture." + GetName().ToString();

        const FUIntVector2 Extent      = TextureResource->MipExtent(InFirstMip);
        const uint32       ResidentNum = NumMips - InFirstMip;

        // New RHI: create the sampled texture in the global heap + upload every mip of every layer.
        // Both paths land in the same heap, so the ResourceID is interchangeable -- what decides
        // whether a shader may read it as gTextures2DArray[] is the VIEW type chosen here.
        // This is NOT once-per-object: the texture editor re-runs it after an in-place edit (flip
        // H/V), the array factory re-runs it after re-cooking layers, and the streamer re-runs it on every
        // promotion/demotion. Creating over the top left the previous image and its heap slot orphaned --
        // one leaked texture per edit -- and moved the ResourceID, which every material that samples this
        // texture has already baked into its uniform block. Both branches therefore have to deal with an
        // existing image.
        if (TextureResource->IsArray())
        {
            // Same contract as the 2D path: the slot is repointed, never dropped. Layer count is fixed
            // for the life of the asset, so only the mip count moves under the streamer -- which is what
            // lets arrays stream at all (they used to Release+Create here, take a new ResourceID, and be
            // excluded from streaming outright to avoid it).
            RHI::Textures::Recreate(TextureResource->NewTexture, RHI::FTexture2DArrayDesc
            {
                .Width  = Extent.x,
                .Height = Extent.y,
                .Layers = NumLayers,
                .Mips   = ResidentNum,
                .Format = Desc.Format,
                .DebugName = DebugName.c_str(),
            });
        }
        else
        {
            // Repoints the existing heap slot and frame-defers the old image, so a re-run keeps the
            // published ResourceID valid. Falls back to a plain Create when the handle is not yet valid,
            // which is the normal first-load path.
            RHI::Textures::Recreate(TextureResource->NewTexture, RHI::FTexture2DDesc
            {
                .Width  = Extent.x,
                .Height = Extent.y,
                .Mips   = ResidentNum,
                .Format = Desc.Format,
                .DebugName = DebugName.c_str(),
            });
        }

        for (uint32 Layer = 0; Layer < NumLayers; ++Layer)
        {
            for (uint32 Mip = InFirstMip; Mip < NumMips; ++Mip)
            {
                const uint32 Index = TextureResource->MipIndex(Layer, Mip);
                if (Index >= TextureResource->Mips.size())
                {
                    continue;
                }

                const FTextureResource::FMip& Mip1 = TextureResource->Mips[Index];

                // The image only holds the resident range, so mip M of the chain is mip (M - InFirstMip) of
                // the image. Getting this wrong writes the wrong-sized data into the wrong level.
                const uint32 ImageMip = Mip - InFirstMip;

                // RowPitchTexels = mip width: pixel rows are tightly packed at the mip's width.
                RHI::Textures::UploadLayer(TextureResource->NewTexture, Layer, ImageMip, Mip1.Pixels.data(), Mip1.Pixels.size(), Mip1.Width, Mip1.Width, Mip1.Height);
            }
        }

        // Arms the swap against the uploads just queued. Until they have executed, the bindless slot keeps
        // naming the PREVIOUS image -- so a shader sampling this texture during the changeover reads the
        // old resolution rather than an image that has never been written. Recreate, uploads and this call
        // are one sequence: splitting them across a frame boundary is what reintroduces the hazard.
        RHI::Textures::CommitRecreate(TextureResource->NewTexture);

        TextureResource->ResidentFirstMip = (uint8)InFirstMip;
        return true;
    }

    void CTexture::OnDestroy()
    {
        if (FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet())
        {
            Streaming->UnregisterTexture(this);
        }

        if (TextureResource == nullptr || !TextureResource->NewTexture.IsValid())
        {
            return;
        }

        // POSTED, not released. Release unbinds the bindless slot immediately -- correct, and load-bearing
        // for the use-after-free it was added to close -- but a material that named this slot is still
        // being drawn by the retained scene for another frame or two, and an unbound slot resolves to the
        // magenta fallback. The queue holds image and slot together until no recordable frame names them,
        // by which point the material slot it belonged to has been released the same way.
        if (FRenderManager* RenderManager = TryRender())
        {
            RHI::FRenderRelease Release;
            Release.Texture = TextureResource->NewTexture;
            RenderManager->GetReleaseQueue().Post(Release);

            // Handed over: this object must not release it a second time.
            TextureResource->NewTexture = RHI::FManagedTexture{};
            return;
        }

        // No renderer to defer through (shutdown, or a headless context that still built textures).
        RHI::Textures::Release(TextureResource->NewTexture);
    }
}
