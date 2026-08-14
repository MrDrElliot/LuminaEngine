#include "RuntimePCH.h"
#include "Texture.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Memory/MemoryTracking.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHITexture.h"
#include "Renderer/RHIUpload.h"
#include "Renderer/TextureStreamingManager.h"
#include "Core/Profiler/Profile.h"

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

                // The save in progress is copying the bulk region into the new file byte for byte, so these
                // mips are already going across and the BulkRefs naming them stay valid. Reading them here
                // would pull ~21 MiB per 4K texture into memory to produce bytes nobody writes.
                if (Package->IsBulkPassthrough())
                {
                    return;
                }
            }

            if (!Package->ReadBulkData(Mip.BulkRef, Mip.Pixels))
            {
                LOG_ERROR("CTexture: {} could not re-read a streamed mip; saving would drop it", GetName());

                // PreSave has no way to fail a save, and the serializer downstream would write this mip
                // back as a zero-length payload -- destroying bytes that are still intact on disk. Tell
                // the package instead, which refuses to commit rather than lose them.
                Package->FlagUnresolvedBulkData();
            }
        }
    }

    void CTexture::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Textures");

        // PostLoad is also the re-entry point after something REPLACES TextureResource wholesale -- the
        // array factory rebuilds through it. Anything left over from the previous resource describes an
        // image that no longer exists: a half-drained fill cursor would upload into whatever is here now,
        // and a residency refusal earned by the old mips would outlive the mips that earned it.
        PendingFill       = FResidencyFill{};
        bResidencyBlocked = false;

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
        LUMINA_PROFILE_SECTION("Texture::ApplyMipResidency");
        LUMINA_MEMORY_SCOPE("Textures");

        if (TextureResource == nullptr)
        {
            return false;
        }

        // An earlier change had to be given back because this texture's cooked mips cannot fill the image
        // they describe. Nothing about retrying changes that, and the streamer asks every frame, so the
        // refusal is remembered rather than re-derived (and re-logged) each time.
        if (bResidencyBlocked)
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

        // Captured BEFORE Recreate stages a replacement: which mips the outgoing image holds is what
        // decides how much of the new one can be filled on the GPU instead of from the CPU.
        const uint32 OldFirstMip = TextureResource->ResidentFirstMip;
        const bool   bHadImage   = TextureResource->NewTexture.IsValid()
                                && TextureResource->NewTexture.SampledSlot != RHI::kInvalidHeapSlot;

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

        // Everything the PREVIOUS image already holds moves across on the GPU instead of being staged again
        // from the CPU. Recreate hands back an empty image, so without this every residency change
        // re-uploaded the whole resident chain -- ~21 MiB of game-thread memcpy to promote one 4K texture,
        // paid again at every mip step. A demotion becomes pure GPU copy and costs no host bandwidth at all.
        const uint32 RetainedFrom = bHadImage ? Math::Max(OldFirstMip, InFirstMip) : NumMips;

        for (uint32 Layer = 0; Layer < NumLayers; ++Layer)
        {
            for (uint32 Mip = RetainedFrom; Mip < NumMips; ++Mip)
            {
                const FUIntVector2 MipExtent = TextureResource->MipExtent(Mip);

                // Chain mip M sits at (M - FirstMip) in each image, and the two images have different
                // FirstMips -- which is the entire reason this is a copy and not a blit.
                if (!RHI::Textures::CopyMipFromCurrent(TextureResource->NewTexture, Layer,
                        Mip - OldFirstMip, Mip - InFirstMip, MipExtent.x, MipExtent.y))
                {
                    // No staged source (first load): fall back to the host upload range below.
                    break;
                }
            }
        }

        // The rest is host data, drained over frames by TickResidencyFill. The staged image is invisible
        // until it is complete, so there is no artifact in spreading it -- and one 4K mip is 16 MiB of
        // memcpy, which is exactly the spike that has to stop landing in a single frame.
        PendingFill.FirstMip     = InFirstMip;
        PendingFill.PrevFirstMip = OldFirstMip;
        PendingFill.NextMip      = InFirstMip;
        PendingFill.NextLayer    = 0;
        PendingFill.NextRow      = 0;
        PendingFill.CpuEndMip    = RetainedFrom;
        PendingFill.bActive      = true;

        TextureResource->ResidentFirstMip = (uint8)InFirstMip;

        // A pure demotion has nothing to stage -- every mip came across on the GPU -- so there is nothing
        // to meter and it can publish now rather than waiting a frame for a tick with no work to do.
        if (PendingFill.NextMip >= PendingFill.CpuEndMip)
        {
            RHI::Textures::CommitRecreate(TextureResource->NewTexture);
            PendingFill.bActive = false;
            return true;
        }

        // Only textures the streamer registered get their fill drained by TickResidencyFills, and it
        // registers streamable ones only -- so for everything else this is the one and only chance to
        // upload. Left to the streamer, a non-streamable texture would sit at an image that never
        // received a single texel, and a staged one would never be committed.
        if (!TextureResource->IsStreamable())
        {
            DrainResidencyFillNow();
        }

        return true;
    }

    void CTexture::DrainResidencyFillNow()
    {
        // Bounded, not a spin: TickResidencyFill only reports work left with an unlimited budget when an
        // upload was DROPPED, and the sole reason for that is a full staging ring, which a flush clears.
        // Anything still refusing after that is not going to resolve by asking again.
        constexpr uint32 kMaxFlushRetries = 4;

        for (uint32 Attempt = 0; PendingFill.bActive && Attempt <= kMaxFlushRetries; ++Attempt)
        {
            uint64 Unmetered = UINT64_MAX;
            if (!TickResidencyFill(Unmetered, true))
            {
                return;   // committed, or abandoned with the reason already logged
            }

            RHI::FlushUploadsAndWait();
        }

        LOG_ERROR("CTexture::DrainResidencyFillNow: {} could not stage mip {} layer {}; the upload staging "
                  "ring stayed full across {} flushes. Keeping the previous image rather than publishing "
                  "one with a hole.",
            GetName(), PendingFill.NextMip, PendingFill.NextLayer, kMaxFlushRetries);

        AbandonResidencyFill();
    }

    void CTexture::AbandonResidencyFill()
    {
        if (TextureResource == nullptr)
        {
            return;
        }

        PendingFill.bActive = false;

        // Order matters: the residency the texture reports has to match the image the slot actually names
        // again, and AbandonRecreate is what puts NewTexture back on that image. Nothing is staged on a
        // first load (Recreate took the eager path), in which case there is no previous image to fall back
        // to and the texture simply stays as created -- incomplete, but the log above says why.
        if (RHI::Textures::HasPendingSwap(TextureResource->NewTexture))
        {
            RHI::Textures::AbandonRecreate(TextureResource->NewTexture);
            TextureResource->ResidentFirstMip = (uint8)PendingFill.PrevFirstMip;
        }
    }

    void CTexture::OnFullyUploadedExternally()
    {
        if (TextureResource == nullptr)
        {
            return;
        }

        // The image holds mips [0, NumMips) and every one of them was just uploaded, so any cursor left
        // over from a streaming change is describing an image that no longer exists. A re-cook is also the
        // one thing that replaces a mip whose block rows did not fill their image, so the refusal that
        // caused goes with it.
        PendingFill       = FResidencyFill{};
        bResidencyBlocked = false;
        TextureResource->ResidentFirstMip = 0;

        // Re-registration refreshes the cached byte counts and inline tail; without it the streamer prices
        // this texture at its pre-cook size and can demote it toward a tail that moved.
        if (FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet())
        {
            Streaming->RegisterTexture(this);
        }
    }

    bool CTexture::TickResidencyFill(uint64& RemainingBytes, bool bMayExceedBudget)
    {
        if (!PendingFill.bActive || TextureResource == nullptr)
        {
            return false;
        }

        LUMINA_PROFILE_SECTION("Texture::ResidencyFill");

        const uint32 NumLayers = TextureResource->GetNumLayers();

        // Rows of a block-compressed mip come in groups of BlockH texels, and both the source offset and
        // the destination band have to land on one. 1 for uncompressed formats, so the same code covers both.
        const uint32 BlockH = Math::Max<uint32>(
            RHI::Format::Info(TextureResource->ImageDescription.Format).BlockSize, 1u);

        // Step the cursor past whatever it is pointing at: layers of a mip in order, then the next mip.
        auto AdvanceSlice = [&]()
        {
            PendingFill.NextRow = 0;
            if (++PendingFill.NextLayer >= NumLayers)
            {
                PendingFill.NextLayer = 0;
                ++PendingFill.NextMip;
            }
        };

        while (PendingFill.NextMip < PendingFill.CpuEndMip)
        {
            const uint32 Index = TextureResource->MipIndex(PendingFill.NextLayer, PendingFill.NextMip);
            if (Index >= TextureResource->Mips.size())
            {
                AdvanceSlice();
                continue;
            }

            const FTextureResource::FMip& Mip = TextureResource->Mips[Index];
            const uint64 SliceBytes = Mip.Pixels.size();
            if (SliceBytes == 0)
            {
                AdvanceSlice();
                continue;
            }

            // The IMAGE's mip height, not the cooked one: they disagree on NPOT chains, and the backend
            // clamps a copy's extent but not its offset.
            const uint32 ImageMipHeight = Math::Max(TextureResource->MipExtent(PendingFill.NextMip).y, 1u);

            // RowPitch counts BLOCK rows. A pitch that does not divide the payload describes something
            // other than a tightly packed mip, so that one moves whole rather than being cut up wrongly.
            const bool   bBanded    = Mip.RowPitch != 0 && (SliceBytes % Mip.RowPitch) == 0;
            const uint64 RowBytes   = bBanded ? Mip.RowPitch : SliceBytes;
            const uint32 StoredRows = bBanded ? (uint32)(SliceBytes / RowBytes) : 1u;
            const uint32 NeededRows = bBanded ? (ImageMipHeight + BlockH - 1u) / BlockH : 1u;

            // More stored rows than the image needs is block padding past the bottom edge. Fewer is a hole,
            // and publishing an image with one is what this staging dance exists to prevent.
            if (bBanded && StoredRows < NeededRows)
            {
                LOG_ERROR("CTexture::TickResidencyFill: {} mip {} layer {} holds {} block rows but its image "
                          "needs {} ({} texels tall). Abandoning the staged image rather than publishing it "
                          "with a hole; the texture stays at its previous residency.",
                    GetName(), PendingFill.NextMip, PendingFill.NextLayer, StoredRows, NeededRows, ImageMipHeight);

                // Actually give the staged image back. Just clearing bActive would leave the swap staged
                // and unarmed, which never publishes and never clears -- so ApplyMipResidency would refuse
                // this texture forever after and both images would stay allocated.
                bResidencyBlocked = true;
                AbandonResidencyFill();
                return false;
            }

            const uint32 NumRows = Math::Min(StoredRows, NeededRows);

            // Unreachable today, but NumRows - FirstRow underflowing reads megabytes past the mip.
            if (PendingFill.NextRow >= NumRows)
            {
                AdvanceSlice();
                continue;
            }

            // Priced before the copy, so the budget is never overshot -- except by the single row that
            // bGuaranteeProgress forces through. That exemption is what makes a staged fill unable to
            // stall: an image that is mid-swap is not a candidate for deferral, because nothing samples it
            // until it completes and its slot is frozen until then. Costing one block row per in-flight
            // texture per frame is the price of that, and a block row is kilobytes.
            uint64 AffordableRows = RemainingBytes / RowBytes;
            if (AffordableRows == 0)
            {
                if (!bGuaranteeProgress)
                {
                    ++PendingFill.StalledTicks;
                    return true;
                }
                AffordableRows = 1;
            }

            const uint32 FirstRow  = PendingFill.NextRow;
            const uint32 Rows      = (uint32)Math::Min<uint64>(AffordableRows, NumRows - FirstRow);
            const uint64 BandBytes = (uint64)Rows * RowBytes;

            // Bounded by BOTH ends: past the destination falls outside the subresource, past the source
            // walks the copy engine off the staging buffer. Offsets are texels, the cursor counts blocks.
            const uint32 OffsetY    = bBanded ? FirstRow * BlockH : 0u;
            const uint32 BandHeight = bBanded ? Math::Min(Rows * BlockH, ImageMipHeight - OffsetY)
                                              : Math::Min(Mip.Height, ImageMipHeight);

            // Chain mip M is mip (M - FirstMip) of the image, which only holds the resident range.
            // RowPitchTexels describes the SOURCE row length, not the destination.
            if (!RHI::Textures::UploadLayer(TextureResource->NewTexture, PendingFill.NextLayer,
                    PendingFill.NextMip - PendingFill.FirstMip,
                    Mip.Pixels.data() + (uint64)FirstRow * RowBytes, BandBytes,
                    Mip.Width, Mip.Width, BandHeight, OffsetY))
            {
                // Do NOT advance: committing with this band missing bakes the hole in permanently. Retrying
                // is usually free -- the bytes are in memory and nothing samples the staged image until
                // commit -- but "usually" is not "always", and a rejection that never clears used to hang
                // the texture for the rest of the session. Bounded below instead.
                ++PendingFill.StalledTicks;
                return true;
            }

            PendingFill.StalledTicks = 0;

            RemainingBytes    = BandBytes >= RemainingBytes ? 0ull : RemainingBytes - BandBytes;
            bGuaranteeProgress = false;

            PendingFill.NextRow = FirstRow + Rows;
            if (PendingFill.NextRow >= NumRows)
            {
                AdvanceSlice();
            }
        }

        // Arms the swap against everything queued for it. Until that has executed on the GPU the bindless
        // slot keeps naming the PREVIOUS image -- so a shader sampling this texture during the changeover
        // reads the old resolution rather than an image that has never been written.
        RHI::Textures::CommitRecreate(TextureResource->NewTexture);
        PendingFill.bActive = false;
        return false;
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
