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

        // The only window between Super::Serialize settling the property and operator<< recomputing the split.
        TextureResource->ImageDescription.bNeverStream = !GetResolvedPolicy().bAllowStreaming;

        // PreSave is a CObject hook nothing else calls, so any other writing archive would lose streamed mips.
        if (Ar.IsWriting())
        {
            MakeStreamedMipsResident();
        }

        Ar << *TextureResource.get();

        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::TEXTURE_SOURCE_FILE)
        {
            // Flagged first, since a cooked package drops the payload and the reader cannot tell otherwise.
            bool bHasSourceFile = Ar.IsWriting() ? (!Ar.IsCooking() && SourceFile.IsValid()) : false;
            Ar << bHasSourceFile;

            if (bHasSourceFile)
            {
                Ar << SourceFile;
            }
            else if (Ar.IsReading())
            {
                SourceFile.Reset();
            }
        }
    }

    bool CTexture::LoadSourceFileBytes()
    {
        if (SourceFile.IsResident())
        {
            return true;
        }
        if (!SourceFile.BulkRef.IsValid())
        {
            return false;
        }

        CPackage* Package = GetPackage();
        if (Package == nullptr)
        {
            return false;
        }

        if (!Package->ReadBulkData(SourceFile.BulkRef, SourceFile.Bytes))
        {
            LOG_ERROR("CTexture: {} could not re-read its stored source bytes", GetName());
            return false;
        }
        return true;
    }

    RHI::EStockSampler CTexture::GetStockSampler() const
    {
        const ETextureAddress Address = AddressMode;

        switch (GetResolvedPolicy().Filter)
        {
        case ETextureFilter::Nearest:
            return Address == ETextureAddress::Clamp  ? RHI::EStockSampler::PointClamp
                 : Address == ETextureAddress::Mirror ? RHI::EStockSampler::PointMirror
                 :                                      RHI::EStockSampler::PointWrap;

        case ETextureFilter::Anisotropic:
            return Address == ETextureAddress::Clamp  ? RHI::EStockSampler::AnisoClamp
                 : Address == ETextureAddress::Mirror ? RHI::EStockSampler::AnisoMirror
                 :                                      RHI::EStockSampler::AnisoWrap;

        case ETextureFilter::FromGroup:
        case ETextureFilter::Linear:
        default:
            return Address == ETextureAddress::Clamp  ? RHI::EStockSampler::LinearClamp
                 : Address == ETextureAddress::Mirror ? RHI::EStockSampler::LinearMirror
                 :                                      RHI::EStockSampler::LinearWrap;
        }
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
        // Runs before the export offset is recorded, so its IO cannot land between the offset and the bytes.
        MakeStreamedMipsResident();
    }

    void CTexture::MakeStreamedMipsResident()
    {
        // Only missing mips are read, so a freshly imported texture does no IO at all.
        if (TextureResource == nullptr)
        {
            return;
        }

        CPackage* Package = nullptr;

        // The same trap as a mip, writing the source back while it is only a BulkRef replaces it with nothing.
        if (SourceFile.BulkRef.IsValid() && !SourceFile.IsResident())
        {
            if (CPackage* Owner = GetPackage(); Owner != nullptr && !Owner->IsBulkPassthrough())
            {
                if (!Owner->ReadBulkData(SourceFile.BulkRef, SourceFile.Bytes))
                {
                    LOG_ERROR("CTexture: {} could not re-read its stored source; saving would drop it", GetName());
                    Owner->FlagUnresolvedBulkData();
                }
            }
        }

        for (FTextureResource::FMip& Mip : TextureResource->Mips)
        {
            if (!Mip.BulkRef.IsValid() || !Mip.Pixels.empty())
            {
                continue;
            }

            // The common case must not pay for the lookup, and a missing package only matters if bytes are needed.
            if (Package == nullptr)
            {
                Package = GetPackage();
                if (Package == nullptr)
                {
                    LOG_ERROR("CTexture: {} has streamed-out mips but no package to re-read them from; "
                              "writing it now would drop them", GetName());
                    return;
                }

                // The save is copying the bulk region byte for byte, so reading here would pull megabytes for nothing.
                if (Package->IsBulkPassthrough())
                {
                    return;
                }
            }

            if (!Package->ReadBulkData(Mip.BulkRef, Mip.Pixels))
            {
                LOG_ERROR("CTexture: {} could not re-read a streamed mip; saving would drop it", GetName());

                // PreSave cannot fail a save, so tell the package, which refuses to commit rather than lose bytes.
                Package->FlagUnresolvedBulkData();
            }
        }
    }

    void CTexture::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Textures");

        // A half-drained fill cursor would upload into whatever resource is here now.
        PendingFill       = FResidencyFill{};
        bResidencyBlocked = false;

        // Opening fifteen 4K textures costs fifteen 256px images until something asks for more.
        TextureResource->ResidentFirstMip = TextureResource->ImageDescription.FirstInlineMip;

        (void)ApplyMipResidency(TextureResource->ResidentFirstMip);

        // Registration states whether this texture STREAMS, and an unregistered one never drains its fill.
        if (FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet())
        {
            Streaming->RegisterTexture(this);
        }

#if !USING(WITH_EDITOR)
        // The inline tail is not in the bulk region, so dropping it makes the texture permanently un-demotable.
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

        // The streamer asks every frame, so the refusal is remembered rather than re-derived and re-logged.
        if (bResidencyBlocked)
        {
            return false;
        }

        // Stacking a second swap would abandon the first mid-flight, and the caller's retry is free.
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

        // Recreate hands back an empty image, so bail before the GPU rather than leave it half filled.
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

        // Named after the asset so a GPU crash report identifies which texture faulted.
        const FString DebugName = "Texture." + GetName().ToString();

        const FUIntVector2 Extent      = TextureResource->MipExtent(InFirstMip);
        const uint32       ResidentNum = NumMips - InFirstMip;

        // What the outgoing image holds decides how much of the new one fills on the GPU.
        const uint32 OldFirstMip = TextureResource->ResidentFirstMip;
        const bool   bHadImage   = TextureResource->NewTexture.IsValid()
                                && TextureResource->NewTexture.SampledSlot != RHI::kInvalidHeapSlot;

        // Creating over the top orphaned the old image and moved the ResourceID materials had baked.
        if (TextureResource->IsArray())
        {
            // Layer count is fixed for the asset's life, so only the mip count moves under the streamer.
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
            // Falls back to a plain Create when the handle is not yet valid, the normal first-load path.
            RHI::Textures::Recreate(TextureResource->NewTexture, RHI::FTexture2DDesc
            {
                .Width  = Extent.x,
                .Height = Extent.y,
                .Mips   = ResidentNum,
                .Format = Desc.Format,
                .DebugName = DebugName.c_str(),
            });
        }

        // Without this every residency change re-uploaded the whole resident chain from the CPU.
        const uint32 RetainedFrom = bHadImage ? Math::Max(OldFirstMip, InFirstMip) : NumMips;

        for (uint32 Layer = 0; Layer < NumLayers; ++Layer)
        {
            for (uint32 Mip = RetainedFrom; Mip < NumMips; ++Mip)
            {
                const FUIntVector2 MipExtent = TextureResource->MipExtent(Mip);

                // The two images have different FirstMips, which is why this is a copy and not a blit.
                if (!RHI::Textures::CopyMipFromCurrent(TextureResource->NewTexture, Layer,
                        Mip - OldFirstMip, Mip - InFirstMip, MipExtent.x, MipExtent.y))
                {
                    // No staged source on a first load, so fall back to the host upload range below.
                    break;
                }
            }
        }

        // The staged image is invisible until complete, and one 4K mip is 16 MiB of memcpy.
        PendingFill.FirstMip     = InFirstMip;
        PendingFill.PrevFirstMip = OldFirstMip;
        PendingFill.NextMip      = InFirstMip;
        PendingFill.NextLayer    = 0;
        PendingFill.NextRow      = 0;
        PendingFill.CpuEndMip    = RetainedFrom;
        PendingFill.StalledTicks = 0;   // a fresh fill, not a continuation of whatever stalled before it
        PendingFill.bActive      = true;

        TextureResource->ResidentFirstMip = (uint8)InFirstMip;

        // A pure demotion has nothing to stage, so it publishes now rather than waiting a frame.
        if (PendingFill.NextMip >= PendingFill.CpuEndMip)
        {
            RHI::Textures::CommitRecreate(TextureResource->NewTexture);
            PendingFill.bActive = false;
            return true;
        }

        // The streamer registers only streamable textures, so this is everything else's one chance.
        if (!TextureResource->IsStreamable())
        {
            DrainResidencyFillNow();
        }

        return true;
    }

    void CTexture::DrainResidencyFillNow()
    {
        // An unlimited budget only reports work left when an upload was DROPPED, which a flush clears.
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

        // AbandonRecreate puts NewTexture back on the image the slot names, so order matters here.
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

        // A re-cook also replaces the mip whose short rows caused a refusal, so that refusal goes too.
        PendingFill       = FResidencyFill{};
        bResidencyBlocked = false;
        TextureResource->ResidentFirstMip = 0;

        // Without it the streamer prices this at its pre-cook size and can demote toward a moved tail.
        if (FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet())
        {
            Streaming->RegisterTexture(this);
        }
    }

    bool CTexture::TickResidencyFill(uint64& RemainingBytes, bool bGuaranteeProgress)
    {
        if (!PendingFill.bActive || TextureResource == nullptr)
        {
            return false;
        }

        LUMINA_PROFILE_SECTION("Texture::ResidencyFill");

        // The fill gives up on itself rather than adding a guard per cause, and a slow fill resets this.
        constexpr uint32 kMaxStalledTicks = 300;

        if (PendingFill.StalledTicks >= kMaxStalledTicks)
        {
            LOG_ERROR("CTexture::TickResidencyFill: {} made no progress for {} ticks at mip {} layer {}; "
                      "abandoning the staged image rather than leaving its bindless slot frozen. The "
                      "texture stays at its previous residency and the change can be retried.",
                GetName(), kMaxStalledTicks, PendingFill.NextMip, PendingFill.NextLayer);

            AbandonResidencyFill();
            return false;
        }

        const uint32 NumLayers = TextureResource->GetNumLayers();

        // BlockH is 1 for uncompressed formats, so the same code covers both cases.
        const uint32 BlockH = Math::Max<uint32>(
            RHI::Format::Info(TextureResource->ImageDescription.Format).BlockSize, 1u);

        // Steps past whatever the cursor points at, layers of a mip in order, then the next mip.
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

            // The IMAGE's mip height, since they disagree on NPOT chains and the backend clamps extent only.
            const uint32 ImageMipHeight = Math::Max(TextureResource->MipExtent(PendingFill.NextMip).y, 1u);

            // A pitch that does not divide the payload is not a tightly packed mip, so move it whole.
            const bool   bBanded    = Mip.RowPitch != 0 && (SliceBytes % Mip.RowPitch) == 0;
            const uint64 RowBytes   = bBanded ? Mip.RowPitch : SliceBytes;
            const uint32 StoredRows = bBanded ? (uint32)(SliceBytes / RowBytes) : 1u;
            const uint32 NeededRows = bBanded ? (ImageMipHeight + BlockH - 1u) / BlockH : 1u;

            // More rows is block padding past the bottom edge, while fewer is a hole this dance prevents.
            if (bBanded && StoredRows < NeededRows)
            {
                LOG_ERROR("CTexture::TickResidencyFill: {} mip {} layer {} holds {} block rows but its image "
                          "needs {} ({} texels tall). Abandoning the staged image rather than publishing it "
                          "with a hole; the texture stays at its previous residency.",
                    GetName(), PendingFill.NextMip, PendingFill.NextLayer, StoredRows, NeededRows, ImageMipHeight);

                // Clearing bActive alone would leave the swap staged and unarmed, refusing this texture forever.
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

            // An image mid-swap is not a deferral candidate, so one forced block row per texture is the price.
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

            // Offsets are texels while the cursor counts blocks, and both ends have to be bounded.
            const uint32 OffsetY    = bBanded ? FirstRow * BlockH : 0u;
            const uint32 BandHeight = bBanded ? Math::Min(Rows * BlockH, ImageMipHeight - OffsetY)
                                              : Math::Min(Mip.Height, ImageMipHeight);

            // RowPitchTexels describes the SOURCE row length, not the destination.
            if (!RHI::Textures::UploadLayer(TextureResource->NewTexture, PendingFill.NextLayer,
                    PendingFill.NextMip - PendingFill.FirstMip,
                    Mip.Pixels.data() + (uint64)FirstRow * RowBytes, BandBytes,
                    Mip.Width, Mip.Width, BandHeight, OffsetY))
            {
                // Committing with a band missing bakes the hole in permanently, so retry bounded rather than advance.
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

        // The slot keeps naming the PREVIOUS image, so a shader reads the old resolution during the changeover.
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

        // Release unbinds immediately, and the retained scene still draws a material naming this slot.
        if (FRenderManager* RenderManager = TryRender())
        {
            RHI::FRenderRelease Release;
            Release.Texture = TextureResource->NewTexture;
            RenderManager->GetReleaseQueue().Post(Release);

            // Handed over, so this object must not release it a second time.
            TextureResource->NewTexture = RHI::FManagedTexture{};
            return;
        }

        // No renderer to defer through (shutdown, or a headless context that still built textures).
        RHI::Textures::Release(TextureResource->NewTexture);
    }
}
