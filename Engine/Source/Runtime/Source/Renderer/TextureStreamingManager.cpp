#include "RuntimePCH.h"
#include "TextureStreamingManager.h"

#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Config/EngineSettings.h"
#include "Core/Math/Math.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/Profile.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "TaskSystem/TaskSystem.h"

#include "EASTL/sort.h"

namespace Lumina
{
    namespace
    {
        // Not exported: every module reaches the streamer through TryGet(), so there is exactly one place
        // that can be wrong about whether one exists.
        FTextureStreamingManager* GTextureStreamingManager = nullptr;

        // Read fresh each frame rather than cached: the Settings panel edits the CDO in place, so a change
        // takes effect on the next Update with no apply step.
        const CTextureStreamingSettings& Settings()
        {
            return *GetDefault<CTextureStreamingSettings>();
        }

        // A texture whose coverage collapsed is not demoted immediately -- a camera cut or a one-frame
        // occlusion would otherwise cost a full reload on the very next frame.
        constexpr uint64 kDemoteHysteresisFrames = 120;

        // How long a coarser budget must hold before residency actually follows it, when the pool is not
        // under pressure. Short enough to reclaim promptly, long enough that stepping back and forth over
        // a mip boundary does not realloc the image every few frames.
        constexpr uint16 kDemoteDeadBandFrames = 30;

        // Headroom a texture must lose before dropping a mip is believed, as a multiplier on coverage.
        // 1.25 == "a quarter more on screen would still not need this mip".
        constexpr float kHysteresisMargin = 1.25f;

        /** Desired first mip so the resident image's long edge is at least DesiredPixels. */
        uint8 MipForScreenSize(uint32 LongEdge, float DesiredPixels, uint8 TailFirstMip)
        {
            if (DesiredPixels <= 0.0f || LongEdge == 0)
            {
                return TailFirstMip;
            }

            uint8  Mip     = 0;
            uint32 Current = LongEdge;
            while (Mip < TailFirstMip && (float)(Current >> 1) >= DesiredPixels && (Current >> 1) > 0)
            {
                Current >>= 1;
                ++Mip;
            }
            return Mip;
        }
    }

    void FTextureStreamingManager::Initialize()
    {
        if (GTextureStreamingManager == nullptr)
        {
            GTextureStreamingManager = Memory::New<FTextureStreamingManager>();
        }
    }

    void FTextureStreamingManager::Shutdown()
    {
        FTextureStreamingManager* Manager = GTextureStreamingManager;

        // Unpublish before deleting: CTexture::OnDestroy calls TryGet(), and textures are still being torn
        // down after this point.
        GTextureStreamingManager = nullptr;

        if (Manager)
        {
            Memory::Delete(Manager);
        }
    }

    FTextureStreamingManager* FTextureStreamingManager::TryGet()
    {
        return GTextureStreamingManager;
    }

    void FTextureStreamingManager::RegisterTexture(CTexture* Texture)
    {
        if (Texture == nullptr || !Texture->IsStreamable())
        {
            return;
        }

        const FTextureResource& Resource = Texture->GetTextureResource();

        FScopeLock Lock(Mutex);

        auto It = TextureToIndex.find(Texture);
        if (It != TextureToIndex.end())
        {
            // Re-registration (a texture editor flip, an array recook) -- refresh the cached sizes rather
            // than adding a second entry that would double-count against the budget.
            FStreamingTexture& Existing = Textures[It->second];
            ResidentBytesTotal -= Existing.ResidentBytes;
            Existing.ResidentBytes = Resource.CalcResidentSizeBytes();
            Existing.FullBytes     = Resource.CalcFullSizeBytes();
            Existing.TailFirstMip  = Resource.ImageDescription.FirstInlineMip;
            ResidentBytesTotal += Existing.ResidentBytes;
            return;
        }

        FStreamingTexture Entry;
        Entry.Texture         = Texture;
        Entry.ResidentBytes   = Resource.CalcResidentSizeBytes();
        Entry.FullBytes       = Resource.CalcFullSizeBytes();
        Entry.TailFirstMip      = Resource.ImageDescription.FirstInlineMip;
        Entry.WantedFirstMip    = Entry.TailFirstMip;
        Entry.BudgetedFirstMip  = Entry.TailFirstMip;
        Entry.LastDemandFrame   = FrameCounter;

        ResidentBytesTotal += Entry.ResidentBytes;

        TextureToIndex.insert_or_assign(Texture, (uint32)Textures.size());
        Textures.push_back(Move(Entry));
    }

    void FTextureStreamingManager::UnregisterTexture(CTexture* Texture)
    {
        if (Texture == nullptr)
        {
            return;
        }

        FScopeLock Lock(Mutex);

        auto It = TextureToIndex.find(Texture);
        if (It == TextureToIndex.end())
        {
            return;
        }

        const uint32 Index = It->second;
        ResidentBytesTotal -= Textures[Index].ResidentBytes;

        // Swap-and-pop, fixing up the index of whatever moved into the hole.
        const uint32 LastIndex = (uint32)Textures.size() - 1;
        if (Index != LastIndex)
        {
            Textures[Index] = Move(Textures[LastIndex]);
            if (CTexture* Moved = Textures[Index].Texture.Get())
            {
                TextureToIndex.insert_or_assign(Moved, Index);
            }
        }
        Textures.pop_back();
        TextureToIndex.erase(Texture);

        // Any in-flight load for this texture is left to complete and be discarded: its weak pointer will
        // have gone null, and cancelling mid-read would mean synchronising with the worker.
    }

    FTextureStreamingManager::FStreamingTexture* FTextureStreamingManager::Find(CTexture* Texture)
    {
        auto It = TextureToIndex.find(Texture);
        return It == TextureToIndex.end() ? nullptr : &Textures[It->second];
    }

    void FTextureStreamingManager::Pin(CTexture* Texture)
    {
        if (Texture == nullptr)
        {
            return;
        }

        FScopeLock Lock(Mutex);

        if (FStreamingTexture* Entry = Find(Texture))
        {
            ++Entry->PinCount;
            Entry->LastDemandFrame = FrameCounter;
        }
    }

    void FTextureStreamingManager::Unpin(CTexture* Texture)
    {
        if (Texture == nullptr)
        {
            return;
        }

        FScopeLock Lock(Mutex);

        if (FStreamingTexture* Entry = Find(Texture))
        {
            if (Entry->PinCount > 0)
            {
                --Entry->PinCount;
            }
            else
            {
                LOG_WARN("FTextureStreamingManager::Unpin: unbalanced unpin for texture {}", Texture->GetName());
            }
        }
    }

    void FTextureStreamingManager::UpdateMaterialTextures(uint32 MaterialIndex, const TVector<CTexture*>& InTextures)
    {
        FScopeLock Lock(Mutex);

        if (InTextures.empty())
        {
            MaterialTextures.erase(MaterialIndex);
            return;
        }

        MaterialTextures.insert_or_assign(MaterialIndex, InTextures);
    }

    void FTextureStreamingManager::ForgetMaterial(uint32 MaterialIndex)
    {
        FScopeLock Lock(Mutex);
        MaterialTextures.erase(MaterialIndex);
    }

    void FTextureStreamingManager::SubmitMaterialCoverage(uint32 MaterialIndex, float ScreenCoveragePixels, bool bDensityMeasured)
    {
        if (ScreenCoveragePixels <= 0.0f)
        {
            return;
        }

        FScopeLock Lock(Mutex);

        auto It = MaterialTextures.find(MaterialIndex);
        if (It == MaterialTextures.end())
        {
            return;
        }

        for (CTexture* Texture : It->second)
        {
            if (FStreamingTexture* Entry = Find(Texture))
            {
                Entry->FrameCoverage     = Math::Max(Entry->FrameCoverage, ScreenCoveragePixels);
                Entry->LastCoverage      = Entry->FrameCoverage;
                Entry->LastDemandFrame   = FrameCounter;
                Entry->bDensityMeasured  = bDensityMeasured;
            }
        }
    }

    uint64 FTextureStreamingManager::GetBudgetBytes() const
    {
        const int64 PoolMB = Math::Max<int64>(Settings().PoolSizeMB, 16);
        return (uint64)PoolMB * 1024ull * 1024ull;
    }

    void FTextureStreamingManager::QueueMaterialPublish(CMaterialInterface* Material)
    {
        if (Material == nullptr)
        {
            return;
        }

        FScopeLock Lock(Mutex);
        PendingPublishes.push_back(TWeakObjectPtr<CMaterialInterface>(Material));
    }

    void FTextureStreamingManager::DrainPendingPublishes()
    {
        // Swapped out under the lock and published outside it: PublishStreamingTextures calls back into
        // UpdateMaterialTextures, which takes the same (non-recursive) mutex.
        TVector<TWeakObjectPtr<CMaterialInterface>> Pending;
        {
            FScopeLock Lock(Mutex);
            if (PendingPublishes.empty())
            {
                return;
            }
            Pending.swap(PendingPublishes);
        }

        for (const TWeakObjectPtr<CMaterialInterface>& Weak : Pending)
        {
            if (CMaterialInterface* Material = Weak.Get())
            {
                // Still conditional on the dirty flag: a material queued several times in one frame, or one
                // that has since published by another route, does no work here.
                Material->PublishStreamingTexturesIfDirty();
            }
        }
    }

    void FTextureStreamingManager::Update()
    {
        LUMINA_PROFILE_SCOPE();

        DrainPendingPublishes();

        FScopeLock Lock(Mutex);

        ++FrameCounter;
        PromotedLastFrame = 0;
        DemotedLastFrame  = 0;

        ProcessCompletedLoads();
        ComputeWantedMips();
        ComputeBudgetedMips();
        ApplyDemotions();
        IssuePromotions();

        // Coverage is a per-frame report; clearing here (rather than on submit) means a texture that stopped
        // being drawn decays to its tail via the hysteresis window instead of sticking at last frame's value.
        for (FStreamingTexture& Entry : Textures)
        {
            Entry.FrameCoverage = 0.0f;
        }
    }

    void FTextureStreamingManager::ComputeWantedMips()
    {
        const bool  bEnabled = Settings().bEnabled;
        const float Bias     = Math::Max(Settings().ResolutionBias, 0.01f);

        for (FStreamingTexture& Entry : Textures)
        {
            CTexture* Texture = Entry.Texture.Get();
            if (Texture == nullptr)
            {
                continue;
            }

            if (!bEnabled || Entry.PinCount > 0)
            {
                Entry.WantedFirstMip = 0;
                continue;
            }

            const FTextureResource& Resource = Texture->GetTextureResource();
            const uint32 LongEdge = Math::Max(Resource.ImageDescription.Extent.x, Resource.ImageDescription.Extent.y);

            if (Entry.FrameCoverage > 0.0f)
            {
                uint8 NewWanted = MipForScreenSize(LongEdge, Entry.FrameCoverage * Bias, Entry.TailFirstMip);

                // Coverage jitters by a few percent every frame as the camera moves. Sitting exactly on a
                // mip boundary, that made Wanted alternate between K and K+1, so the state flickered
                // trimming/settled and the demote dead-band counter reset every other frame -- it could
                // never accumulate, so the dead band did nothing at all.
                //
                // Going COARSER therefore has to clear the boundary by a margin: ask whether a coverage
                // kHysteresisMargin larger would STILL want coarser. Going finer is unguarded, because
                // being late to sharpen is the visible failure.
                const uint8 Current = (uint8)Texture->GetResidentFirstMip();
                if (NewWanted > Current)
                {
                    const uint8 Confirm = MipForScreenSize(LongEdge, Entry.FrameCoverage * Bias * kHysteresisMargin, Entry.TailFirstMip);
                    if (Confirm <= Current)
                    {
                        NewWanted = Current;
                    }
                }

                Entry.WantedFirstMip = NewWanted;
                continue;
            }

            // Nothing reported this texture this frame. Hold what it has until the hysteresis window
            // expires, then let it fall back to the tail.
            const uint8 CurrentFirstMip = (uint8)Texture->GetResidentFirstMip();
            const bool  bRecentlyUsed   = (FrameCounter - Entry.LastDemandFrame) < kDemoteHysteresisFrames;

            Entry.WantedFirstMip = bRecentlyUsed ? CurrentFirstMip : Entry.TailFirstMip;
        }
    }

    float FTextureStreamingManager::RetentionPriority(const FStreamingTexture& Entry, uint64 FrameCounter)
    {
        if (Entry.PinCount > 0)
        {
            return FLT_MAX;
        }

        // Coverage over staleness: a texture filling the screen outranks one glimpsed a second ago, and
        // both outrank something nothing has drawn in a while. The +1s keep a never-drawn texture (0
        // coverage, huge age) at a small positive priority rather than a divide-by-zero.
        const uint64 Age = FrameCounter - Entry.LastDemandFrame;
        return (Entry.LastCoverage + 1.0f) / (1.0f + (float)Age);
    }

    void FTextureStreamingManager::ComputeBudgetedMips()
    {
        // Start from pure quality. Under budget this is the answer, and it is also what makes an
        // over-resident texture fall back to what it actually needs -- holding mips nothing samples is not
        // free, it is pool that another texture could have used.
        uint64 Total = 0;
        for (FStreamingTexture& Entry : Textures)
        {
            Entry.BudgetedFirstMip = Entry.WantedFirstMip;

            if (const CTexture* Texture = Entry.Texture.Get())
            {
                Total += Texture->GetTextureResource().CalcSizeBytesFromMip(Entry.BudgetedFirstMip);
            }
        }

        const uint64 Budget = GetBudgetBytes();
        if (Total <= Budget || !Settings().bEnabled)
        {
            return;
        }

        // Over budget: shed. Sorted worst-retention-first, then walked in ROUNDS taking a single mip per
        // texture per pass. That spread is the whole point -- dropping one texture all the way to its tail
        // frees the same bytes but concentrates every bit of the damage on one surface, and then that
        // surface immediately streams back up. One mip off many textures is both less visible and stable.
        TVector<uint32> Order;
        Order.reserve(Textures.size());
        for (uint32 i = 0; i < (uint32)Textures.size(); ++i)
        {
            if (Textures[i].PinCount == 0 && Textures[i].Texture.Get() != nullptr)
            {
                Order.push_back(i);
            }
        }

        const uint64 FrameNow = FrameCounter;
        eastl::sort(Order.begin(), Order.end(), [this, FrameNow](uint32 A, uint32 B)
        {
            return RetentionPriority(Textures[A], FrameNow) < RetentionPriority(Textures[B], FrameNow);
        });

        bool bDroppedAny = true;
        while (Total > Budget && bDroppedAny)
        {
            bDroppedAny = false;

            for (uint32 Index : Order)
            {
                if (Total <= Budget)
                {
                    break;
                }

                FStreamingTexture& Entry   = Textures[Index];
                const CTexture*    Texture = Entry.Texture.Get();
                if (Texture == nullptr || Entry.BudgetedFirstMip >= Entry.TailFirstMip)
                {
                    continue;   // already at its floor; the inline tail is never given up
                }

                const FTextureResource& Resource = Texture->GetTextureResource();
                const uint64 Before = Resource.CalcSizeBytesFromMip(Entry.BudgetedFirstMip);
                ++Entry.BudgetedFirstMip;
                const uint64 After  = Resource.CalcSizeBytesFromMip(Entry.BudgetedFirstMip);

                Total -= (Before - After);
                bDroppedAny = true;
            }
        }

        if (Total > Budget)
        {
            LOG_WARN("Texture streaming: {:.1f} MiB still needed against a {:.1f} MiB pool with every "
                     "unpinned texture at its inline tail -- raise Streaming.Texture.PoolSizeMB.",
                     (double)Total / (1024.0 * 1024.0), (double)Budget / (1024.0 * 1024.0));
        }
    }

    void FTextureStreamingManager::ApplyDemotions()
    {
        const uint64 Budget      = GetBudgetBytes();
        const bool   bUnderPress = ResidentBytesTotal > Budget;

        for (FStreamingTexture& Entry : Textures)
        {
            CTexture* Texture = Entry.Texture.Get();
            if (Texture == nullptr || Entry.bLoadInFlight)
            {
                continue;
            }

            const uint32 CurrentFirstMip = Texture->GetResidentFirstMip();

            // Budgeted <= Current means we are at or below what is allowed; nothing to give back.
            if (Entry.BudgetedFirstMip <= CurrentFirstMip)
            {
                Entry.FramesWantingCoarser = 0;
                continue;
            }

            // Dead-band, so walking past a wall and crossing a mip boundary does not demote and re-promote
            // every few frames -- each swap is a full image realloc plus a re-upload of every resident mip.
            // Budget pressure overrides it: when the pool is full, freeing memory beats staying smooth.
            if (Entry.FramesWantingCoarser < 0xFFFFu)
            {
                ++Entry.FramesWantingCoarser;
            }
            if (!bUnderPress && Entry.FramesWantingCoarser < kDemoteDeadBandFrames)
            {
                continue;
            }

            // Demotion never needs IO: every mip from the budgeted level down to the tail is either inline
            // or one we already loaded and still hold.
            if (!Texture->ApplyMipResidency(Entry.BudgetedFirstMip))
            {
                continue;
            }

            ResidentBytesTotal -= Entry.ResidentBytes;
            Entry.ResidentBytes = Texture->GetTextureResource().CalcResidentSizeBytes();
            ResidentBytesTotal += Entry.ResidentBytes;

            Entry.FramesWantingCoarser = 0;
            ++DemotedLastFrame;
            ++TotalDemotions;

            // CPU copies of the mips we just dropped are dead weight; they are re-readable from the
            // package's bulk region, which is the whole reason they can be thrown away. Only below the new
            // resident level -- anything at or above it is what the GPU image now holds.
            FTextureResource& Resource = Texture->GetTextureResource();
            for (uint32 Mip = 0; Mip < Entry.BudgetedFirstMip && Mip < Resource.Mips.size(); ++Mip)
            {
                FTextureResource::FMip& MipData = Resource.Mips[Mip];
                if (MipData.BulkRef.IsValid())
                {
                    MipData.Pixels.clear();
                    MipData.Pixels.shrink_to_fit();
                }
            }
        }
    }

    void FTextureStreamingManager::IssuePromotions()
    {
        const int32 MaxInFlight = Math::Max(Settings().MaxLoadsInFlight, 1);

        // Ordered by how starved each texture is, NOT by registry order. With only a handful of IO slots,
        // walking the registry lets whatever happens to be registered first take them -- so a distant
        // texture short one mip could hold up one filling the screen and short four. Priority is the mip
        // deficit weighted by how big the thing is on screen.
        TVector<uint32> Candidates;
        Candidates.reserve(Textures.size());

        for (uint32 i = 0; i < (uint32)Textures.size(); ++i)
        {
            FStreamingTexture& Entry   = Textures[i];
            CTexture*          Texture = Entry.Texture.Get();

            if (Texture == nullptr || Entry.bLoadInFlight || Texture->GetPackage() == nullptr)
            {
                continue;
            }

            if (Entry.BudgetedFirstMip < Texture->GetResidentFirstMip())
            {
                Candidates.push_back(i);
            }
        }

        eastl::sort(Candidates.begin(), Candidates.end(), [this](uint32 A, uint32 B)
        {
            auto Score = [this](uint32 Index)
            {
                const FStreamingTexture& E = Textures[Index];
                const CTexture* T = E.Texture.Get();
                const uint32 Deficit = T ? (T->GetResidentFirstMip() - E.BudgetedFirstMip) : 0u;

                // Pinned first regardless: a pin is an explicit request (an open editor tab), not a hint.
                return (E.PinCount > 0 ? 1.0e9f : 0.0f) + (float)Deficit * (E.LastCoverage + 1.0f);
            };
            return Score(A) > Score(B);
        });

        for (uint32 Index : Candidates)
        {
            if ((int32)PendingLoads.size() >= MaxInFlight)
            {
                break;
            }

            FStreamingTexture& Entry   = Textures[Index];
            CTexture*          Texture = Entry.Texture.Get();

            const uint32 CurrentFirstMip = Texture->GetResidentFirstMip();

            TUniquePtr<FPendingLoad> Load = MakeUnique<FPendingLoad>();
            Load->Texture        = Texture;
            Load->TargetFirstMip = Entry.BudgetedFirstMip;
            Load->SourceFirstMip = (uint8)CurrentFirstMip;
            Load->MipBytes.resize(CurrentFirstMip - Entry.BudgetedFirstMip);

            FPendingLoad* Raw = Load.get();
            PendingLoads.push_back(Move(Load));
            Entry.bLoadInFlight = true;

            // Only the disk read happens off-thread. ReadBulkData is a stateless ranged VFS read against a
            // region whose location is fixed for the life of the loaded package, so it needs no lock; the
            // residency change it feeds is applied back on the game thread by ProcessCompletedLoads.
            Task::AsyncTask(1, 1, [Raw](uint32, uint32, uint32)
            {
                LUMINA_PROFILE_SECTION("Texture Stream Read");

                // Resolved here rather than captured: a captured CPackage* would have to outlive the read
                // on its own, whereas the texture's weak pointer already tells us whether any of this is
                // still worth doing.
                CTexture* Target  = Raw->Texture.Get();
                CPackage* Package = Target ? Target->GetPackage() : nullptr;
                if (Target == nullptr || Package == nullptr)
                {
                    Raw->bFailed = true;
                    Raw->bComplete.store(true, std::memory_order_release);
                    return;
                }

                const FTextureResource& Resource = Target->GetTextureResource();

                for (uint32 Mip = Raw->TargetFirstMip; Mip < Raw->SourceFirstMip; ++Mip)
                {
                    if (Mip >= Resource.Mips.size())
                    {
                        Raw->bFailed = true;
                        break;
                    }

                    const FTextureResource::FMip& MipData = Resource.Mips[Mip];

                    // Already in memory (a save just pulled it back, or a demotion has not reclaimed it yet):
                    // copy rather than re-read. Also the correct fallback when a failed save left BulkRef
                    // pointing into a region that was never written.
                    if (!MipData.Pixels.empty())
                    {
                        Raw->MipBytes[Mip - Raw->TargetFirstMip] = MipData.Pixels;
                        continue;
                    }

                    if (!Package->ReadBulkData(MipData.BulkRef, Raw->MipBytes[Mip - Raw->TargetFirstMip]))
                    {
                        Raw->bFailed = true;
                        break;
                    }
                }

                Raw->bComplete.store(true, std::memory_order_release);
            }, ETaskPriority::Background);
        }
    }

    void FTextureStreamingManager::ProcessCompletedLoads()
    {
        for (size_t i = 0; i < PendingLoads.size(); )
        {
            FPendingLoad& Load = *PendingLoads[i];

            if (!Load.bComplete.load(std::memory_order_acquire))
            {
                ++i;
                continue;
            }

            CTexture* Texture = Load.Texture.Get();
            if (Texture != nullptr)
            {
                if (FStreamingTexture* Entry = Find(Texture))
                {
                    Entry->bLoadInFlight = false;

                    // Re-check against the CURRENT residency, not the one the load was issued against: a
                    // budget sweep may have demoted this texture while the read was in flight, which would
                    // leave a hole between what we loaded and what is on the GPU.
                    const uint32 NowFirstMip = Texture->GetResidentFirstMip();

                    if (!Load.bFailed && NowFirstMip == Load.SourceFirstMip && Load.TargetFirstMip < NowFirstMip)
                    {
                        FTextureResource& Resource = Texture->GetTextureResource();

                        // Tallied before the move below empties them.
                        uint64 BytesRead = 0;
                        for (const TVector<uint8>& Bytes : Load.MipBytes)
                        {
                            BytesRead += Bytes.size();
                        }

                        for (uint32 Mip = Load.TargetFirstMip; Mip < Load.SourceFirstMip; ++Mip)
                        {
                            if (Mip < Resource.Mips.size())
                            {
                                Resource.Mips[Mip].Pixels = Move(Load.MipBytes[Mip - Load.TargetFirstMip]);
                            }
                        }

                        if (Texture->ApplyMipResidency(Load.TargetFirstMip))
                        {
                            ResidentBytesTotal -= Entry->ResidentBytes;
                            Entry->ResidentBytes = Resource.CalcResidentSizeBytes();
                            ResidentBytesTotal += Entry->ResidentBytes;
                            ++PromotedLastFrame;
                            ++TotalPromotions;
                            TotalBytesRead += BytesRead;
                        }
                    }
                    else if (Load.bFailed)
                    {
                        ++TotalFailedLoads;
                        LOG_ERROR("Texture streaming: failed to read mips for {}; it stays at mip {}",
                            Texture->GetName(), NowFirstMip);
                    }
                }
            }

            PendingLoads[i] = Move(PendingLoads.back());
            PendingLoads.pop_back();
        }
    }

    FTextureStreamingManager::FStats FTextureStreamingManager::GetStats() const
    {
        FScopeLock Lock(Mutex);

        FStats Stats;
        Stats.BudgetBytes         = GetBudgetBytes();
        Stats.ResidentBytes       = ResidentBytesTotal;
        Stats.NumTextures         = (uint32)Textures.size();
        Stats.NumLoadsInFlight    = (uint32)PendingLoads.size();
        Stats.NumPromotedLastFrame = PromotedLastFrame;
        Stats.NumDemotedLastFrame  = DemotedLastFrame;

        Stats.TotalPromotions  = TotalPromotions;
        Stats.TotalDemotions   = TotalDemotions;
        Stats.TotalBytesRead   = TotalBytesRead;
        Stats.TotalFailedLoads = TotalFailedLoads;
        Stats.FrameCounter     = FrameCounter;

        for (const FStreamingTexture& Entry : Textures)
        {
            Stats.FullyResidentBytes += Entry.FullBytes;
            if (Entry.PinCount > 0)
            {
                ++Stats.NumPinned;
            }
        }

        return Stats;
    }

    void FTextureStreamingManager::GetSnapshot(TVector<FTextureSnapshot>& OutTextures, TVector<FPendingSnapshot>& OutPending) const
    {
        FScopeLock Lock(Mutex);

        OutTextures.clear();
        OutTextures.reserve(Textures.size());

        for (const FStreamingTexture& Entry : Textures)
        {
            CTexture* Texture = Entry.Texture.Get();
            if (Texture == nullptr)
            {
                continue;   // pending unregistration; nothing meaningful to show
            }

            const FTextureResource& Resource = Texture->GetTextureResource();

            FTextureSnapshot Row;
            Row.Name   = Texture->GetName();
            Row.Width  = Resource.ImageDescription.Extent.x;
            Row.Height = Resource.ImageDescription.Extent.y;
            Row.Format = Resource.ImageDescription.Format;
            Row.ResourceID = Texture->GetResourceID();

            Row.NumMips          = (uint8)Resource.GetNumMips();
            Row.ResidentFirstMip = Resource.ResidentFirstMip;
            Row.WantedFirstMip   = Entry.WantedFirstMip;
            Row.BudgetedFirstMip = Entry.BudgetedFirstMip;
            Row.TailFirstMip     = Entry.TailFirstMip;

            Row.ResidentBytes = Entry.ResidentBytes;
            Row.FullBytes     = Entry.FullBytes;

            // What the mips are still costing in RAM, which is a separate question from GPU residency and
            // the one that catches a streamer that is freeing GPU images but leaking their CPU copies.
            for (const FTextureResource::FMip& Mip : Resource.Mips)
            {
                Row.CpuBytes += Mip.Pixels.size();
            }

            Row.PinCount         = Entry.PinCount;
            Row.LastCoverage     = Entry.LastCoverage;
            Row.bLoadInFlight    = Entry.bLoadInFlight;
            Row.bDensityMeasured = Entry.bDensityMeasured;
            Row.FramesSinceDemand = FrameCounter - Entry.LastDemandFrame;

            if (const CPackage* Package = Texture->GetPackage())
            {
                Row.PackagePath = Package->GetName().ToString();
            }

            OutTextures.push_back(Move(Row));
        }

        OutPending.clear();
        OutPending.reserve(PendingLoads.size());

        for (const TUniquePtr<FPendingLoad>& Load : PendingLoads)
        {
            FPendingSnapshot Row;
            Row.TargetFirstMip = Load->TargetFirstMip;
            Row.SourceFirstMip = Load->SourceFirstMip;
            Row.bComplete      = Load->bComplete.load(std::memory_order_acquire);

            if (CTexture* Texture = Load->Texture.Get())
            {
                Row.Name = Texture->GetName();
            }

            for (const TVector<uint8>& Bytes : Load->MipBytes)
            {
                Row.Bytes += Bytes.size();
            }

            OutPending.push_back(Move(Row));
        }
    }
}
