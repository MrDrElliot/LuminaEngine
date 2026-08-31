#include "RuntimePCH.h"
#include "TextureStreamingManager.h"

#include <bit>   // countr_zero, for the finest mip in a feedback mask

#include "Assets/AssetTypes/Textures/Texture.h"
#include "Config/EngineSettings.h"
#include "Core/Math/Math.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/Profile.h"
#include "Memory/MemoryTracking.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "MaterialManager.h"
#include "RenderManager.h"
#include "TaskSystem/TaskSystem.h"


namespace Lumina
{
    namespace
    {
        // Not exported, so exactly one place can be wrong about whether a streamer exists.
        FTextureStreamingManager* GTextureStreamingManager = nullptr;

        // Read fresh each frame, since the Settings panel edits the CDO in place with no apply step.
        const CTextureStreamingSettings& Settings()
        {
            return *GetDefault<CTextureStreamingSettings>();
        }

        // A camera cut or a one-frame occlusion would otherwise cost a full reload on the next frame.
        constexpr uint64 kDemoteHysteresisFrames = 120;

        // Short enough to reclaim promptly, long enough that a mip boundary is not crossed repeatedly.
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

        // Unpublish before deleting, since CTexture::OnDestroy calls TryGet and textures still tear down.
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
            // Refreshes the cached sizes rather than adding a second entry that double-counts.
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

        // The worker's weak pointer will have gone null, and canceling mid-read means synchronizing.
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

        // Valid but empty is what drives the decay-to-tail branch in ComputeWantedMips.
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

        // Costs materials that drew times their distinct textures, a few thousand lookups in a heavy scene.
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

                // Two materials sharing a texture at different scales must both be satisfied.
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

        // Splitting it per stage would let two stages each spend the budget and double the spike.
        FrameUploadBudget = (uint64)Math::Max(Settings().MaxUploadMBPerFrame, 1) * 1024ull * 1024ull;

        // A demotion recreates the image while copying zero host bytes, which the upload budget cannot see.
        FrameResidencyChanges = (uint32)Math::Max(Settings().MaxResidencyChangesPerFrame, 1);

        // A half-filled staged image holds a second allocation and is closest to paying off.
        TickResidencyFills();

        ProcessCompletedLoads();
        ComputeWantedMips();
        ComputeBudgetedMips();
        ApplyDemotions();
        IssuePromotions();

        // A hitch is a shape over time, and these say whether the cost was host uploads or something else.
        LUMINA_PROFILE_VALUE("Streaming/UploadBudgetLeftKiB", (int64)(FrameUploadBudget / 1024));
        LUMINA_PROFILE_VALUE("Streaming/ResidentMiB", (int64)(ResidentBytesTotal / (1024 * 1024)));
        LUMINA_PROFILE_VALUE("Streaming/LoadsInFlight", (int64)PendingLoads.size());
        LUMINA_PROFILE_VALUE("Streaming/LoadStagingKiB", (int64)(PendingLoadBytes / 1024));
        LUMINA_PROFILE_VALUE("Streaming/ResidencyChanges", (int64)(PromotedLastFrame + DemotedLastFrame));
    }

    void FTextureStreamingManager::TickResidencyFills()
    {
        LUMINA_PROFILE_SECTION("Streaming::TickResidencyFills");

        const SIZE_T Count = Textures.size();
        if (Count == 0)
        {
            return;
        }

        // Fairness only, since the guarantee that a fill cannot stall is the floor below, not this.
        for (SIZE_T i = 0; i < Count; ++i)
        {
            FStreamingTexture& Entry = Textures[(FillCursor + i) % Count];

            CTexture* Texture = Entry.Texture.Get();
            if (Texture == nullptr)
            {
                continue;
            }

            // The budget decides how MUCH each fill moves, never whether it moves at all.
            const uint32 FirstMipWas = Texture->GetResidentFirstMip();

            Texture->TickResidencyFill(FrameUploadBudget, /*bGuaranteeProgress*/ true);

            // Left alone the pool would keep counting mips that were given back, permanently.
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

        const bool bStreaming = Settings().bTextureStreaming;

        for (FStreamingTexture& Entry : Textures)
        {
            CTexture* Texture = Entry.Texture.Get();
            if (Texture == nullptr)
            {
                continue;
            }

            if (!bStreaming || Entry.PinCount > 0)
            {
                Entry.WantedFirstMip = 0;
                continue;
            }

            const FTextureResource& Resource = Texture->GetTextureResource();
            const uint32 LongEdge = Math::Max(Resource.ImageDescription.Extent.x, Resource.ImageDescription.Extent.y);
            const uint8  Current  = (uint8)Texture->GetResidentFirstMip();

            // The mask is an ABSOLUTE required resolution, so it names a target rather than nudging toward one.
            if (Entry.bFeedbackValid)
            {
                if (Entry.FeedbackMask == 0u)
                {
                    // Nothing sampled it. Hold, then decay to the tail.
                    const bool bRecentlyUsed = (FrameCounter - Entry.LastFeedbackFrame) < kDemoteHysteresisFrames;
                    Entry.WantedFirstMip = bRecentlyUsed ? Current : Entry.TailFirstMip;
                    continue;
                }

                // The highest set bit is the largest anything asked for, so it is the one to satisfy.
                const uint32 RequiredLongEdge = 1u << (31u - (uint32)std::countl_zero(Entry.FeedbackMask));

                // Walks down from the full chain, landing on an absolute target in one step.
                uint32 Target = 0;
                while (Target < Entry.TailFirstMip && (LongEdge >> (Target + 1u)) >= RequiredLongEdge)
                {
                    ++Target;
                }

                // Retained mips move GPU-side now, so stepping would only multiply the reallocations.
                Entry.WantedFirstMip = (uint8)Target;
                continue;
            }

            // Holds what it has until the hysteresis window expires, then falls back to the tail.
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

        // The plus one keeps a never-drawn texture at a small positive priority rather than dividing by zero.
        const uint64 Age = FrameCounter - Entry.LastFeedbackFrame;
        return 1.0f / (1.0f + (float)Age);
    }

    void FTextureStreamingManager::ComputeBudgetedMips()
    {
        LUMINA_PROFILE_SECTION("Streaming::ComputeBudgetedMips");

        // Holding mips nothing samples is not free, it is pool another texture could have used.
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
        if (Total <= Budget || !Settings().bTextureStreaming)
        {
            return;
        }

        // One mip off many textures is both less visible and more stable than gutting one surface.
        const uint64 FrameNow = FrameCounter;

        TVector<TPair<float, uint32>>& Order = BudgetOrderScratch;
        Order.clear();
        Order.reserve(Textures.size());
        for (uint32 i = 0; i < (uint32)Textures.size(); ++i)
        {
            if (Textures[i].PinCount == 0 && Textures[i].Texture.Get() != nullptr)
            {
                Order.emplace_back(RetentionPriority(Textures[i], FrameNow), i);
            }
        }

        // Keyed pairs, so the comparator never reaches back into Textures for a scattered load.
        Algo::Sort(Order,
            [](const TPair<float, uint32>& A, const TPair<float, uint32>& B) { return A.first < B.first; });

        // Compacted as textures reach their floor, so a later pass never revisits one that cannot shed.
        SIZE_T Live = Order.size();
        while (Total > Budget && Live > 0)
        {
            SIZE_T Write = 0;
            bool bDroppedAny = false;

            for (SIZE_T i = 0; i < Live; ++i)
            {
                const uint32 Index = Order[i].second;

                FStreamingTexture& Entry   = Textures[Index];
                const CTexture*    Texture = Entry.Texture.Get();
                if (Texture == nullptr || Entry.BudgetedFirstMip >= Entry.TailFirstMip)
                {
                    continue;   // already at its floor; the inline tail is never given up
                }

                if (Total > Budget)
                {
                    const FTextureResource& Resource = Texture->GetTextureResource();
                    const uint64 Before = Resource.CalcSizeBytesFromMip(Entry.BudgetedFirstMip);
                    ++Entry.BudgetedFirstMip;
                    const uint64 After  = Resource.CalcSizeBytesFromMip(Entry.BudgetedFirstMip);

                    Total -= (Before - After);
                    bDroppedAny = true;
                }

                Order[Write++] = Order[i];
            }

            Live = Write;
            if (!bDroppedAny)
            {
                break;
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

            // Budget pressure overrides it, since when the pool is full freeing memory beats staying smooth.
            if (Entry.FramesWantingCoarser < 0xFFFFu)
            {
                ++Entry.FramesWantingCoarser;
            }
            if (!bUnderPress && Entry.FramesWantingCoarser < kDemoteDeadBandFrames)
            {
                continue;
            }

            // Every mip from the budgeted level down is either inline or already loaded and still held.
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

            // Only below the new resident level, since anything at or above it is what the image now holds.
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
        LUMINA_MEMORY_SCOPE("Texture Streaming");

        const int32  MaxInFlight     = Math::Max(Settings().MaxLoadsInFlight, 1);
        const uint64 MaxStagingBytes = (uint64)Math::Max(Settings().MaxLoadStagingMB, 1) * 1024ull * 1024ull;

        // Sized before the load is built, since building one copies every already-resident mip.
        auto PredictStagingBytes = [](const CTexture* Texture, uint32 TargetFirstMip, uint32 SourceFirstMip, uint32 LayerCount)
        {
            const FTextureResource& Resource = Texture->GetTextureResource();
            uint64 Bytes = 0;

            for (uint32 Layer = 0; Layer < LayerCount; ++Layer)
            {
                for (uint32 Mip = TargetFirstMip; Mip < SourceFirstMip; ++Mip)
                {
                    const uint32 MipIndex = Resource.MipIndex(Layer, Mip);
                    if (MipIndex >= Resource.Mips.size())
                    {
                        continue;
                    }

                    const FTextureResource::FMip& MipData = Resource.Mips[MipIndex];
                    Bytes += !MipData.Pixels.empty() ? (uint64)MipData.Pixels.size()
                                                     : (uint64)Math::Max<int64>(MipData.BulkRef.Size, 0);
                }
            }
            return Bytes;
        };

        // Priority is the mip deficit weighted by how big the thing is on screen.
        TVector<TPair<float, uint32>>& Candidates = PromotionScratch;
        Candidates.clear();
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
                const uint32 Deficit = Texture->GetResidentFirstMip() - Entry.BudgetedFirstMip;

                // A pin is an explicit request such as an open editor tab, not a hint.
                const float Score = (Entry.PinCount > 0 ? 1.0e9f : 0.0f) + (float)Deficit;
                Candidates.emplace_back(Score, i);
            }
        }

        // Scored once here rather than inside the comparator, which resolved the weak pointer per compare.
        Algo::Sort(Candidates,
            [](const TPair<float, uint32>& A, const TPair<float, uint32>& B) { return A.first > B.first; });

        for (const TPair<float, uint32>& Candidate : Candidates)
        {
            const uint32 Index = Candidate.second;
            if ((int32)PendingLoads.size() >= MaxInFlight)
            {
                break;
            }

            FStreamingTexture& Entry   = Textures[Index];
            CTexture*          Texture = Entry.Texture.Get();

            const uint32 CurrentFirstMip = Texture->GetResidentFirstMip();
            const uint32 LayerCount      = Math::Max(Texture->GetTextureResource().GetNumLayers(), 1u);
            const uint64 StagingBytes    = PredictStagingBytes(Texture, Entry.BudgetedFirstMip, CurrentFirstMip, LayerCount);

            // Skip rather than break, since priority order means an oversized texture must not stall smaller ones.
            if (!PendingLoads.empty() && PendingLoadBytes + StagingBytes > MaxStagingBytes)
            {
                continue;
            }

            TUniquePtr<FPendingLoad> Load = MakeUnique<FPendingLoad>();
            Load->Texture        = Texture;
            Load->TargetFirstMip = Entry.BudgetedFirstMip;
            Load->SourceFirstMip = (uint8)CurrentFirstMip;
            Load->LayerCount     = LayerCount;
            Load->StagingBytes   = StagingBytes;
            Load->MipBytes.resize((SIZE_T)Load->LayerCount * Load->MipSpan());
            Load->MipRefs.resize((SIZE_T)Load->LayerCount * Load->MipSpan());

            // Every layer, since Mips is layer-major and the promotion is refused unless all are populated.
            {
                const FTextureResource& Resource = Texture->GetTextureResource();

                // Snapshotted with the refs, so a save landing before the worker reads is caught rather than read through.
                if (const CPackage* RefPackage = Texture->GetPackage())
                {
                    Load->BulkGeneration = RefPackage->GetBulkGeneration();
                }

                for (uint32 Layer = 0; Layer < Load->LayerCount; ++Layer)
                {
                    for (uint32 Mip = Load->TargetFirstMip; Mip < Load->SourceFirstMip; ++Mip)
                    {
                        const uint32 MipIndex = Resource.MipIndex(Layer, Mip);
                        if (MipIndex >= Resource.Mips.size())
                        {
                            continue;   // no ref and no bytes, so the worker fails the load on this slice
                        }

                        const FTextureResource::FMip& MipData = Resource.Mips[MipIndex];
                        const uint32 Slice = Load->SliceIndex(Layer, Mip);

                        // Also the correct fallback when a failed save left the ref pointing into an unwritten region.
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
            PendingLoadBytes += StagingBytes;
            Entry.bLoadInFlight = true;

            // Only the disk read is off-thread, and the residency change it feeds runs on the game thread.
            Task::AsyncTask(1, 1, [Raw](uint32, uint32, uint32)
            {
                LUMINA_PROFILE_SECTION("Texture Stream Read");
                LUMINA_MEMORY_SCOPE("Texture Streaming");

                // The texture's weak pointer already says whether any of this is still worth doing.
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
                     || !Package->ReadBulkData(Raw->MipRefs[Slice], Raw->MipBytes[Slice], Raw->BulkGeneration))
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
        LUMINA_MEMORY_SCOPE("Texture Streaming");

        // A load that does not fit stays complete and pending, so waiting a frame costs nothing.

        // Floored at one, since over-budget can be permanent and zero would strand a pinned texture.
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

            // The first load of a frame is always allowed through, so an oversized texture still progresses.
            if (FrameUploadBudget == 0 || PromotionsApplied >= PromotionLimit)
            {
                ++i;
                continue;
            }

            CTexture* Texture = Load.Texture.Get();

            // Applying another now would abandon the staged one, and the bytes moved would be dropped.
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

                    // A budget sweep may have demoted this while the read was in flight, leaving a hole.
                    const uint32 NowFirstMip = Texture->GetResidentFirstMip();

                    if (!Load.bFailed && NowFirstMip == Load.SourceFirstMip && Load.TargetFirstMip < NowFirstMip)
                    {
                        FTextureResource& Resource = Texture->GetTextureResource();

                        // Tallied before the move below empties them.
                        const uint64 BytesRead = Algo::Accumulate(Load.MipBytes, uint64(0),
                            [](const TVector<uint8>& Bytes) { return Bytes.size(); });

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

                            // Applying only STAGES an image, and the host uploads are metered as they happen.
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

            PendingLoadBytes -= Math::Min(PendingLoadBytes, Load.StagingBytes);
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

            // Catches a streamer freeing GPU images while leaking their CPU copies.
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
