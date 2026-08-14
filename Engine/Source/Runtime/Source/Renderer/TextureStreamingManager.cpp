#include "RuntimePCH.h"
#include "TextureStreamingManager.h"

#include <bit>   // countr_zero, for the finest mip in a feedback mask

#include "Assets/AssetTypes/Textures/Texture.h"
#include "Config/EngineSettings.h"
#include "Core/Math/Math.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/Profile.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "MaterialManager.h"
#include "RenderManager.h"
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

    void FTextureStreamingManager::SubmitMaterialFeedback(const uint32* Masks, uint32 Count)
    {
        if (Masks == nullptr || Count == 0)
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Streaming::SubmitMaterialFeedback");

        RHI::FMaterialManager& Materials = Render().GetMaterialManager();

        FScopeLock Lock(Mutex);

        // The feedback is complete by construction -- every lane that shades a material reports -- so a
        // texture nobody reported for genuinely was not sampled. Start from zero and let the expansion
        // below fill in; "valid but empty" is what drives the decay-to-tail branch in ComputeWantedMips.
        SlotToEntry.clear();
        for (uint32 i = 0; i < (uint32)Textures.size(); ++i)
        {
            FStreamingTexture& Entry = Textures[i];
            Entry.FeedbackMask   = 0u;
            Entry.bFeedbackValid = true;

            const CTexture* Texture = Entry.Texture.Get();
            if (Texture == nullptr)
            {
                Entry.bFeedbackValid = false;
                continue;
            }

            const uint32 Slot = Texture->GetTextureResource().NewTexture.SampledSlot;
            if (Slot == RHI::kInvalidHeapSlot)
            {
                Entry.bFeedbackValid = false;
                continue;
            }
            SlotToEntry.insert_or_assign(Slot, i);
        }

        // Walk only the material slots that actually reported. Cost is (materials that drew) x (their
        // distinct textures), which is a few thousand hash lookups in a heavy scene.
        const uint32 NumMaterialSlots = Math::Min(Count, Materials.GetCapacity());
        uint32 TextureIDs[MAX_TEXTURES];

        for (uint32 MaterialSlot = 0; MaterialSlot < NumMaterialSlots; ++MaterialSlot)
        {
            const uint32 Mask = Masks[MaterialSlot];
            if (Mask == 0u)
            {
                continue;
            }

            const uint32 NumIDs = Materials.CopySlotTextureIDs(MaterialSlot, TextureIDs, MAX_TEXTURES);
            for (uint32 i = 0; i < NumIDs; ++i)
            {
                auto It = SlotToEntry.find(TextureIDs[i]);
                if (It == SlotToEntry.end())
                {
                    continue;
                }

                // OR, not assign: two materials sharing a texture at different scales must both be
                // satisfied, and the highest set bit is the one that wins downstream.
                FStreamingTexture& Entry = Textures[It->second];
                Entry.FeedbackMask     |= Mask;
                Entry.LastFeedbackFrame = FrameCounter;
            }
        }
    }

    uint64 FTextureStreamingManager::GetBudgetBytes() const
    {
        const int64 PoolMB = Math::Max<int64>(Settings().PoolSizeMB, 16);
        return (uint64)PoolMB * 1024ull * 1024ull;
    }

    void FTextureStreamingManager::Update()
    {
        LUMINA_PROFILE_SCOPE();

        FScopeLock Lock(Mutex);

        ++FrameCounter;
        PromotedLastFrame = 0;
        DemotedLastFrame  = 0;

        // ONE host-upload budget for the whole frame, spent by the staged fills first and by newly applied
        // loads with whatever is left. Splitting it per stage would let two stages each spend the "budget"
        // and land twice the spike the setting names.
        FrameUploadBudget = (uint64)Math::Max(Settings().MaxUploadMBPerFrame, 1) * 1024ull * 1024ull;

        // And ONE image-churn budget: a demotion recreates the image while copying zero host bytes, so the
        // upload budget above cannot see it.
        FrameResidencyChanges = (uint32)Math::Max(Settings().MaxResidencyChangesPerFrame, 1);

        // Before anything new is started: an image that is already staged and half-filled is holding a
        // whole second allocation and is closer to paying off than anything not begun.
        TickResidencyFills();

        ProcessCompletedLoads();
        ComputeWantedMips();
        ComputeBudgetedMips();
        ApplyDemotions();
        IssuePromotions();

        // Plotted rather than logged: a hitch is a shape over time, and these are the two numbers that say
        // whether the frame's cost was host uploads or something else entirely.
        LUMINA_PROFILE_VALUE("Streaming/UploadBudgetLeftKiB", (int64)(FrameUploadBudget / 1024));
        LUMINA_PROFILE_VALUE("Streaming/ResidentMiB", (int64)(ResidentBytesTotal / (1024 * 1024)));
        LUMINA_PROFILE_VALUE("Streaming/LoadsInFlight", (int64)PendingLoads.size());
        LUMINA_PROFILE_VALUE("Streaming/ResidencyChanges", (int64)(PromotedLastFrame + DemotedLastFrame));
    }

    void FTextureStreamingManager::TickResidencyFills()
    {
        LUMINA_PROFILE_SECTION("Streaming::TickResidencyFills");

        bool bMayExceedBudgetThisFrame = true;

        const SIZE_T Count = Textures.size();
        if (Count == 0)
        {
            return;
        }

        // ROTATED, not registry order. The frame's budget is spent by whoever comes first, and in a fixed
        // order that is always the same textures -- so anything further down could afford zero rows frame
        // after frame, forever. That is not a slow fill, it is a stuck one: the staged image never
        // completes, CommitRecreate is never reached, and the swap sits unarmed until the "never
        // committed" error fires. It bites the biggest textures hardest, which is why terrain arrays
        // (LayerCount x a full chain, the hungriest things in the registry) were the ones that hung.
        for (SIZE_T i = 0; i < Count; ++i)
        {
            FStreamingTexture& Entry = Textures[(FillCursor + i) % Count];

            CTexture* Texture = Entry.Texture.Get();
            if (Texture == nullptr)
            {
                continue;
            }

            // The over-budget allowance is granted at most ONCE per frame, to whichever texture spends
            // first, so a mip bigger than the whole budget still converges without every texture in the
            // scene claiming the same exemption in the same frame. Combined with the rotation above, every
            // texture gets its turn at the front, which is what makes progress guaranteed rather than
            // dependent on where registration happened to put it.
            const uint64 Before      = FrameUploadBudget;
            const uint32 FirstMipWas = Texture->GetResidentFirstMip();

            Texture->TickResidencyFill(FrameUploadBudget, bMayExceedBudgetThisFrame);
            bMayExceedBudgetThisFrame = bMayExceedBudgetThisFrame && (FrameUploadBudget == Before);

            // A fill that had to abandon its staged image rolls residency back, and the entry's cached
            // size was charged against the pool when the change was APPLIED. Left alone, the pool would
            // keep counting mips that were given back -- permanently, since nothing else recomputes it
            // until the next successful change.
            if (Texture->GetResidentFirstMip() != FirstMipWas)
            {
                ResidentBytesTotal -= Entry.ResidentBytes;
                Entry.ResidentBytes = Texture->GetTextureResource().CalcResidentSizeBytes();
                ResidentBytesTotal += Entry.ResidentBytes;
            }
        }

        FillCursor = (FillCursor + 1) % Count;
    }

    void FTextureStreamingManager::ComputeWantedMips()
    {
        LUMINA_PROFILE_SECTION("Streaming::ComputeWantedMips");

        const bool bEnabled = Settings().bEnabled;

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
            const uint8  Current  = (uint8)Texture->GetResidentFirstMip();

            // GPU feedback wins wherever it exists. It is a measurement of the mip the shaders actually
            // sampled, so it needs no bounds, no distance and no texel density -- the three things the CPU
            // estimate below got wrong. The mask is an ABSOLUTE required resolution, independent of what
            // is currently resident, so it names a target directly rather than nudging toward one.
            if (Entry.bFeedbackValid)
            {
                if (Entry.FeedbackMask == 0u)
                {
                    // Nothing sampled it. Hold, then decay to the tail.
                    const bool bRecentlyUsed = (FrameCounter - Entry.LastFeedbackFrame) < kDemoteHysteresisFrames;
                    Entry.WantedFirstMip = bRecentlyUsed ? Current : Entry.TailFirstMip;
                    continue;
                }

                // HIGHEST set bit: the request is an absolute resolution ("at least 2^N texels across"),
                // so the largest anything asked for is the one to satisfy. Independent of what is currently
                // resident, which is what stopped this from being a convergence loop.
                const uint32 RequiredLongEdge = 1u << (31u - (uint32)std::countl_zero(Entry.FeedbackMask));

                // First mip whose long edge still meets the request. Walks down from the full chain, so it
                // lands on an absolute target in one step rather than creeping toward it.
                uint32 Target = 0;
                while (Target < Entry.TailFirstMip && (LongEdge >> (Target + 1u)) >= RequiredLongEdge)
                {
                    ++Target;
                }

                // Straight to the target, no per-frame stepping. That cap existed to bound the cost of a
                // step back when every step was a full image realloc plus a re-upload of the entire
                // resident chain -- converging 4 -> 0 meant four of them, re-staging ~28 MiB to deliver 21.
                // Retained mips now move GPU-side and the host half is metered by TickResidencyFill, which
                // bounds the cost where it actually is; stepping would only multiply the reallocations.
                Entry.WantedFirstMip = (uint8)Target;
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

        // Recency alone now that coverage is gone: the GPU mask says whether a texture was sampled, not
        // how big it was, so "sampled most recently" is the honest ranking. The +1 keeps a never-drawn
        // texture at a small positive priority rather than a divide-by-zero.
        const uint64 Age = FrameCounter - Entry.LastFeedbackFrame;
        return 1.0f / (1.0f + (float)Age);
    }

    void FTextureStreamingManager::ComputeBudgetedMips()
    {
        LUMINA_PROFILE_SECTION("Streaming::ComputeBudgetedMips");

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
        LUMINA_PROFILE_SECTION("Streaming::ApplyDemotions");

        const uint64 Budget      = GetBudgetBytes();
        const bool   bUnderPress = ResidentBytesTotal > Budget;

        for (FStreamingTexture& Entry : Textures)
        {
            // Out of image churn. The rest keep their FramesWantingCoarser and demote over later frames.
            if (FrameResidencyChanges == 0)
            {
                break;
            }

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
            --FrameResidencyChanges;
            ++DemotedLastFrame;
            ++TotalDemotions;

            // CPU copies of the mips we just dropped are dead weight; they are re-readable from the
            // package's bulk region, which is the whole reason they can be thrown away. Only below the new
            // resident level -- anything at or above it is what the GPU image now holds.
            FTextureResource& Resource = Texture->GetTextureResource();
            const uint32 NumLayers = Math::Max(Resource.GetNumLayers(), 1u);
            for (uint32 Layer = 0; Layer < NumLayers; ++Layer)
            {
                for (uint32 Mip = 0; Mip < Entry.BudgetedFirstMip; ++Mip)
                {
                    const uint32 Index = Resource.MipIndex(Layer, Mip);
                    if (Index >= Resource.Mips.size())
                    {
                        continue;
                    }

                    FTextureResource::FMip& MipData = Resource.Mips[Index];
                    if (MipData.BulkRef.IsValid())
                    {
                        MipData.Pixels.clear();
                        MipData.Pixels.shrink_to_fit();
                    }
                }
            }
        }
    }

    void FTextureStreamingManager::IssuePromotions()
    {
        LUMINA_PROFILE_SECTION("Streaming::IssuePromotions");

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
                return (E.PinCount > 0 ? 1.0e9f : 0.0f) + (float)Deficit;
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
            Load->LayerCount     = Math::Max(Texture->GetTextureResource().GetNumLayers(), 1u);
            Load->MipBytes.resize((SIZE_T)Load->LayerCount * Load->MipSpan());
            Load->MipRefs.resize((SIZE_T)Load->LayerCount * Load->MipSpan());

            // Snapshot what the read needs BEFORE it is dispatched, so the worker never dereferences the
            // texture's mips. It used to read FMip::Pixels and FMip::BulkRef live, which raced anything on
            // the game thread that touched them -- most sharply a save: CTexture::PreSave refills Pixels
            // from disk, and reallocating that vector while the worker is copying it is a use-after-free.
            // Renaming a texture that is streaming in is exactly that sequence.
            //
            // Every layer, not just layer 0: Mips is a flat Layer-major array and ApplyMipResidency refuses
            // the promotion unless all of them are populated.
            {
                const FTextureResource& Resource = Texture->GetTextureResource();

                for (uint32 Layer = 0; Layer < Load->LayerCount; ++Layer)
                {
                    for (uint32 Mip = Load->TargetFirstMip; Mip < Load->SourceFirstMip; ++Mip)
                    {
                        const uint32 MipIndex = Resource.MipIndex(Layer, Mip);
                        if (MipIndex >= Resource.Mips.size())
                        {
                            continue;   // no ref and no bytes: the worker fails the load on this slice
                        }

                        const FTextureResource::FMip& MipData = Resource.Mips[MipIndex];
                        const uint32 Slice = Load->SliceIndex(Layer, Mip);

                        // Already in memory (a save just pulled it back, or a demotion has not reclaimed it
                        // yet): hand the bytes over now rather than re-reading them. Also the correct
                        // fallback when a failed save left BulkRef pointing into a region never written.
                        if (!MipData.Pixels.empty())
                        {
                            Load->MipBytes[Slice] = MipData.Pixels;
                        }
                        else
                        {
                            Load->MipRefs[Slice] = MipData.BulkRef;
                        }
                    }
                }
            }

            FPendingLoad* Raw = Load.get();
            PendingLoads.push_back(Move(Load));
            Entry.bLoadInFlight = true;

            // Only the disk read happens off-thread, against refs and a package that were resolved above;
            // the residency change it feeds is applied back on the game thread by ProcessCompletedLoads.
            Task::AsyncTask(1, 1, [Raw](uint32, uint32, uint32)
            {
                LUMINA_PROFILE_SECTION("Texture Stream Read");

                // Resolved here rather than captured: a captured CPackage* would have to outlive the read
                // on its own, whereas the texture's weak pointer already tells us whether any of this is
                // still worth doing. A rename does NOT invalidate it -- CPackage::Rename keeps its exported
                // objects alive precisely so live references survive -- and ReadBulkData takes the region
                // and the file it lives in as one consistent snapshot, so a save committing underneath this
                // read moves it to the new file wholesale rather than half-way.
                CTexture* Target  = Raw->Texture.Get();
                CPackage* Package = Target ? Target->GetPackage() : nullptr;
                if (Target == nullptr || Package == nullptr)
                {
                    Raw->bFailed = true;
                    Raw->bComplete.store(true, std::memory_order_release);
                    return;
                }

                for (SIZE_T Slice = 0; Slice < Raw->MipBytes.size() && !Raw->bFailed; ++Slice)
                {
                    if (!Raw->MipBytes[Slice].empty())
                    {
                        continue;   // handed over already resident
                    }

                    if (!Raw->MipRefs[Slice].IsValid()
                     || !Package->ReadBulkData(Raw->MipRefs[Slice], Raw->MipBytes[Slice]))
                    {
                        Raw->bFailed = true;
                    }
                }

                Raw->bComplete.store(true, std::memory_order_release);
            }, ETaskPriority::Background);
        }
    }

    void FTextureStreamingManager::ProcessCompletedLoads()
    {
        LUMINA_PROFILE_SECTION("Streaming::ProcessCompletedLoads");

        // Applying a promotion is a full re-upload of the texture's resident chain (Recreate hands back an
        // empty image), which is a game-thread memcpy into the staging ring. Several large textures
        // finishing their reads on the same frame is what produces the stream-in hitch, so only so many
        // bytes are allowed to land per frame. A load that does not fit stays complete and pending -- its
        // bytes are already in memory, so waiting a frame costs nothing but the wait.

        // Promotions run first, so under pressure they leave half the changes for the half that frees
        // memory. Floored at one: over-budget can be permanent, and zero would strand a PINNED texture.
        const uint32 PromotionLimit = (ResidentBytesTotal > GetBudgetBytes())
            ? Math::Max(FrameResidencyChanges / 2u, 1u)
            : FrameResidencyChanges;
        uint32 PromotionsApplied = 0;

        for (size_t i = 0; i < PendingLoads.size(); )
        {
            FPendingLoad& Load = *PendingLoads[i];

            if (!Load.bComplete.load(std::memory_order_acquire))
            {
                ++i;
                continue;
            }

            // Budget is checked per load, not per byte: a single texture larger than the whole budget must
            // still make progress, so the first load of a frame is always allowed through.
            if (FrameUploadBudget == 0 || PromotionsApplied >= PromotionLimit)
            {
                ++i;
                continue;
            }

            CTexture* Texture = Load.Texture.Get();

            // The last residency change for this texture is staged and not visible yet. Applying another
            // one now would abandon it, so hold the load instead of spending it -- ApplyMipResidency would
            // refuse anyway, and the bytes it moved into the resource would be dropped on the floor.
            if (Texture != nullptr && Texture->HasPendingGPUResidency())
            {
                ++i;
                continue;
            }

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

                        for (uint32 Layer = 0; Layer < Load.LayerCount; ++Layer)
                        {
                            for (uint32 Mip = Load.TargetFirstMip; Mip < Load.SourceFirstMip; ++Mip)
                            {
                                const uint32 Index = Resource.MipIndex(Layer, Mip);
                                if (Index < Resource.Mips.size())
                                {
                                    Resource.Mips[Index].Pixels = Move(Load.MipBytes[Load.SliceIndex(Layer, Mip)]);
                                }
                            }
                        }

                        if (Texture->ApplyMipResidency(Load.TargetFirstMip))
                        {
                            ResidentBytesTotal -= Entry->ResidentBytes;
                            Entry->ResidentBytes = Resource.CalcResidentSizeBytes();
                            ResidentBytesTotal += Entry->ResidentBytes;
                            --FrameResidencyChanges;
                            ++PromotionsApplied;
                            ++PromotedLastFrame;
                            ++TotalPromotions;
                            TotalBytesRead += BytesRead;

                            // NOT charged here. Applying a promotion only STAGES an image; the host
                            // uploads that actually cost bandwidth are metered by TickResidencyFill as
                            // they happen. Charging here as well would bill the same bytes twice.
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
            Row.FeedbackMask     = Entry.FeedbackMask;
            Row.bFeedbackValid   = Entry.bFeedbackValid;
            Row.bLoadInFlight    = Entry.bLoadInFlight;
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
