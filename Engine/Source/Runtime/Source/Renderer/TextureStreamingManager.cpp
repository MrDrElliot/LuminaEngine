#include "RuntimePCH.h"
#include "TextureStreamingManager.h"

#include <bit>   // countr_zero, for the finest mip in a feedback mask

#include "Assets/AssetTypes/Textures/Texture.h"
#include "Config/EngineSettings.h"
#include "Core/Math/Math.h"
#include "Core/Object/Package/Package.h"
#include "Core/Templates/NumericLimits.h"
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

        // Only mips with a bulk ref, since anything else has no way back once its bytes are gone.
        void ReleaseMipsBelow(FTextureResource& Resource, uint32 FirstMip)
        {
            const uint32 NumLayers = Math::Max(Resource.GetNumLayers(), 1u);
            for (uint32 Layer = 0; Layer < NumLayers; ++Layer)
            {
                for (uint32 Mip = 0; Mip < FirstMip; ++Mip)
                {
                    const uint32 Index = Resource.MipIndex(Layer, Mip);
                    if (Index >= static_cast<uint32>(Resource.Mips.Num()))
                    {
                        continue;
                    }

                    FTextureResource::FMip& MipData = Resource.Mips[Index];
                    if (MipData.BulkRef.IsValid())
                    {
                        MipData.Pixels.Reset();
                        MipData.Pixels.Shrink();
                    }
                }
            }
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

        if (FStreamingTexture* Found = Find(Texture))
        {
            // Refreshes the cached sizes rather than adding a second entry that double-counts.
            FStreamingTexture& Existing = *Found;
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

        TextureToIndex.Add(Entry.Texture.GetHandle(), (uint32)Textures.Num());
        Textures.Add(Move(Entry));
    }

    void FTextureStreamingManager::UnregisterTexture(CTexture* Texture)
    {
        if (Texture == nullptr)
        {
            return;
        }

        FScopeLock Lock(Mutex);

        const FObjectHandle Handle = Object::Internal::GetHandleByObject(Texture);
        auto It = TextureToIndex.Find(Handle);
        if (It == nullptr)
        {
            return;
        }

        const uint32 Index = (*It);
        ResidentBytesTotal -= Textures[Index].ResidentBytes;

        // Swap-and-pop, fixing up the index of whatever moved into the hole.
        const uint32 LastIndex = (uint32)Textures.Num() - 1;
        if (Index != LastIndex)
        {
            Textures[Index] = Move(Textures[LastIndex]);
            TextureToIndex.Add(Textures[Index].Texture.GetHandle(), Index);
        }
        Textures.Pop();
        TextureToIndex.Remove(Handle);

        // A load in flight keeps only its package pinned and is dropped on completion, since canceling mid-read means synchronizing.
    }

    FTextureStreamingManager::FStreamingTexture* FTextureStreamingManager::Find(CTexture* Texture)
    {
        auto It = TextureToIndex.Find(Object::Internal::GetHandleByObject(Texture));
        return It == nullptr ? nullptr : &Textures[(*It)];
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
        SlotToEntry.Reset();
        for (int32 i = 0; i < Textures.Num(); ++i)
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
            SlotToEntry.Add(Slot, i);
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
                auto It = SlotToEntry.Find(TextureIDs[i]);
                if (It == nullptr)
                {
                    continue;
                }

                // Two materials sharing a texture at different scales must both be satisfied.
                FStreamingTexture& Entry = Textures[(*It)];
                Entry.FeedbackMask     |= Mask;
                Entry.LastFeedbackFrame = FrameCounter;
            }
        }
    }

    uint64 FTextureStreamingManager::GetBudgetBytes() const
    {
        const int64 PoolMB = Math::Max<int64>(Settings().PoolSizeMB, 16);
        return (uint64)PoolMB * Constants::kMiB;
    }

    void FTextureStreamingManager::Update()
    {
        LUMINA_PROFILE_SCOPE();

        FScopeLock Lock(Mutex);

        ++FrameCounter;
        PromotedLastFrame = 0;
        DemotedLastFrame  = 0;

        // Splitting it per stage would let two stages each spend the budget and double the spike.
        FrameUploadBudget = (uint64)Math::Max(Settings().MaxUploadMBPerFrame, 1) * Constants::kMiB;

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
        LUMINA_PROFILE_VALUE("Streaming/UploadBudgetLeftKiB", FrameUploadBudget / Constants::kKiB);
        LUMINA_PROFILE_VALUE("Streaming/ResidentMiB", ResidentBytesTotal / Constants::kMiB);
        LUMINA_PROFILE_VALUE("Streaming/LoadsInFlight", PendingLoads.Num());
        LUMINA_PROFILE_VALUE("Streaming/LoadStagingKiB", PendingLoadBytes / Constants::kKiB);
        LUMINA_PROFILE_VALUE("Streaming/ResidencyChanges", PromotedLastFrame + DemotedLastFrame);
    }

    void FTextureStreamingManager::TickResidencyFills()
    {
        LUMINA_PROFILE_SECTION("Streaming::TickResidencyFills");

        const int32 Count = Textures.Num();
        if (Count == 0)
        {
            return;
        }

        // Fairness only, since the guarantee that a fill cannot stall is the floor below, not this.
        for (int32 i = 0; i < Count; ++i)
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
            return TNumericLimits<float>::Max();
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
        Order.Reset();
        Order.Reserve(Textures.Num());
        for (int32 i = 0; i < Textures.Num(); ++i)
        {
            if (Textures[i].PinCount == 0 && Textures[i].Texture.Get() != nullptr)
            {
                Order.Emplace(RetentionPriority(Textures[i], FrameNow), i);
            }
        }

        // Every visible texture ties on priority, and an unstable order shed a different set each frame.
        Algo::Sort(Order,
            [](const TPair<float, uint32>& A, const TPair<float, uint32>& B)
            {
                return A.Key != B.Key ? A.Key < B.Key : A.Value < B.Value;
            });

        // Compacted as textures reach their floor, so a later pass never revisits one that cannot shed.
        const auto ShedPass = [this, &Total, Budget](TVector<TPair<float, uint32>>& Candidates, bool bOnlyUnresident)
        {
            int32 Live = Candidates.Num();
            while (Total > Budget && Live > 0)
            {
                int32 Write = 0;
                bool bDroppedAny = false;

                for (int32 i = 0; i < Live; ++i)
                {
                    FStreamingTexture& Entry   = Textures[Candidates[i].Value];
                    const CTexture*    Texture = Entry.Texture.Get();
                    if (Texture == nullptr)
                    {
                        continue;
                    }

                    // The inline tail is never given up, and the first pass stops where shedding would start demoting.
                    const uint32 Floor = bOnlyUnresident
                        ? Math::Min<uint32>(Entry.TailFirstMip, Texture->GetResidentFirstMip())
                        : Entry.TailFirstMip;
                    if (Entry.BudgetedFirstMip >= Floor)
                    {
                        continue;
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

                    Candidates[Write++] = Candidates[i];
                }

                Live = Write;
                if (!bDroppedAny)
                {
                    break;
                }
            }
        };

        // Declining promotions fits the pool without touching anything resident, so it is tried before any demotion.
        TVector<TPair<float, uint32>>& Unresident = BudgetUnresidentScratch;
        Unresident.Reset();
        Unresident.Append(Order.begin(), Order.end());
        ShedPass(Unresident, /*bOnlyUnresident*/ true);
        ShedPass(Order, /*bOnlyUnresident*/ false);

        if (Total > Budget)
        {
            LOG_WARN("Texture streaming: {:.1f} MiB still needed against a {:.1f} MiB pool with every "
                     "unpinned texture at its inline tail -- raise Streaming.Texture.PoolSizeMB.",
                     (double)Total / double(Constants::kMiB), (double)Budget / double(Constants::kMiB));
        }
    }

    void FTextureStreamingManager::ApplyDemotions()
    {
        LUMINA_PROFILE_SECTION("Streaming::ApplyDemotions");

        const uint64 Budget      = GetBudgetBytes();
        // A promotion refused for room last frame is waiting on exactly these demotions, so they skip the dead band.
        const bool   bUnderPress = ResidentBytesTotal > Budget || bPromotionBlockedByPool;

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
            ReleaseMipsBelow(Texture->GetTextureResource(), Entry.BudgetedFirstMip);
        }
    }

    void FTextureStreamingManager::IssuePromotions()
    {
        LUMINA_PROFILE_SECTION("Streaming::IssuePromotions");
        LUMINA_MEMORY_SCOPE("Texture Streaming");

        const int32  MaxInFlight     = Math::Max(Settings().MaxLoadsInFlight, 1);
        const uint64 MaxStagingBytes = (uint64)Math::Max(Settings().MaxLoadStagingMB, 1) * Constants::kMiB;

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
                    if (MipIndex >= static_cast<uint32>(Resource.Mips.Num()))
                    {
                        continue;
                    }

                    const FTextureResource::FMip& MipData = Resource.Mips[MipIndex];
                    Bytes += !MipData.Pixels.IsEmpty() ? (uint64)MipData.Pixels.Num()
                                                     : (uint64)Math::Max<int64>(MipData.BulkRef.Size, 0);
                }
            }
            return Bytes;
        };

        // Priority is the mip deficit weighted by how big the thing is on screen.
        TVector<TPair<float, uint32>>& Candidates = PromotionScratch;
        Candidates.Reset();
        Candidates.Reserve(Textures.Num());

        for (int32 i = 0; i < Textures.Num(); ++i)
        {
            FStreamingTexture& Entry   = Textures[i];
            CTexture*          Texture = Entry.Texture.Get();

            if (Texture == nullptr || Entry.bLoadInFlight || Texture->GetPackage() == nullptr || Texture->IsResidencyBlocked())
            {
                continue;
            }

            if (Entry.BudgetedFirstMip < Texture->GetResidentFirstMip())
            {
                const uint32 Deficit = Texture->GetResidentFirstMip() - Entry.BudgetedFirstMip;

                // A pin is an explicit request such as an open editor tab, not a hint.
                const float Score = (Entry.PinCount > 0 ? 1.0e9f : 0.0f) + (float)Deficit;
                Candidates.Emplace(Score, i);
            }
        }

        // Scored once here rather than inside the comparator, which resolved the weak pointer per compare.
        Algo::Sort(Candidates,
            [](const TPair<float, uint32>& A, const TPair<float, uint32>& B) { return A.Key > B.Key; });

        const uint64 Budget = GetBudgetBytes();
        bPromotionBlockedByPool = false;
        uint64 PendingGrowth = 0;
        for (const TUniquePtr<FPendingLoad>& Pending : PendingLoads)
        {
            PendingGrowth += Pending->GrowthBytes;
        }

        for (const TPair<float, uint32>& Candidate : Candidates)
        {
            const uint32 Index = Candidate.Value;
            if (PendingLoads.Num() >= MaxInFlight)
            {
                break;
            }

            FStreamingTexture& Entry   = Textures[Index];
            CTexture*          Texture = Entry.Texture.Get();

            const uint32 CurrentFirstMip = Texture->GetResidentFirstMip();
            const uint32 LayerCount      = Math::Max(Texture->GetTextureResource().GetNumLayers(), 1u);

            // A whole array chain in one read held hundreds of megabytes and a worker for 100 ms, so a load stops at the cap.
            uint32 TargetFirstMip = Entry.BudgetedFirstMip;
            uint64 StagingBytes   = PredictStagingBytes(Texture, TargetFirstMip, CurrentFirstMip, LayerCount);
            while (TargetFirstMip + 1u < CurrentFirstMip && StagingBytes > MaxStagingBytes)
            {
                ++TargetFirstMip;
                StagingBytes = PredictStagingBytes(Texture, TargetFirstMip, CurrentFirstMip, LayerCount);
            }

            // Skip rather than break, since priority order means an oversized texture must not stall smaller ones.
            if (!PendingLoads.IsEmpty() && PendingLoadBytes + StagingBytes > MaxStagingBytes)
            {
                continue;
            }

            // A promotion the pool cannot hold forces a demotion elsewhere, and that pair repeating is the churn.
            const uint64 TargetBytes = Texture->GetTextureResource().CalcSizeBytesFromMip(TargetFirstMip);
            const uint64 GrowthBytes = TargetBytes > Entry.ResidentBytes ? TargetBytes - Entry.ResidentBytes : 0;
            if (Entry.PinCount == 0 && ResidentBytesTotal + PendingGrowth + GrowthBytes > Budget)
            {
                bPromotionBlockedByPool = true;
                continue;
            }

            TUniquePtr<FPendingLoad> Load = MakeUnique<FPendingLoad>(Texture, TargetFirstMip, CurrentFirstMip, LayerCount);
            Load->GrowthBytes    = GrowthBytes;
            PendingGrowth       += GrowthBytes;
            Load->StagingBytes   = StagingBytes;

            FPendingLoad* Raw = Load.get();
            PendingLoads.Add(Move(Load));
            PendingLoadBytes += StagingBytes;
            Entry.bLoadInFlight = true;

            // Only the disk read is off-thread, and the residency change it feeds runs on the game thread.
            Jobs::Spawn(Jobs::EJobPriority::Background, [](FPendingLoad* InFlight) -> Jobs::TTask<void>
            {
                InFlight->Read();
                co_return;
            }, Raw);
        }
    }

    FTextureStreamingManager::FPendingLoad::FPendingLoad(CTexture* InTexture, uint32 InTargetFirstMip, uint32 InSourceFirstMip, uint32 InLayerCount)
        : Texture(InTexture)
        , Package(InTexture->GetPackage())
        , TargetFirstMip((uint8)InTargetFirstMip)
        , SourceFirstMip((uint8)InSourceFirstMip)
        , LayerCount(InLayerCount)
    {
        MipBytes.SetNum(IntCastChecked<int32>(static_cast<int64>(LayerCount) * MipSpan()));
        MipRefs.SetNum(IntCastChecked<int32>(static_cast<int64>(LayerCount) * MipSpan()));

        // Snapshotted with the refs, so a save landing before the worker reads is caught rather than read through.
        if (Package)
        {
            BulkGeneration = Package->GetBulkGeneration();
        }

        // Every layer, since Mips is layer-major and the promotion is refused unless all are populated.
        const FTextureResource& Resource = InTexture->GetTextureResource();
        for (uint32 Layer = 0; Layer < LayerCount; ++Layer)
        {
            for (uint32 Mip = TargetFirstMip; Mip < SourceFirstMip; ++Mip)
            {
                const uint32 MipIndex = Resource.MipIndex(Layer, Mip);
                if (MipIndex >= static_cast<uint32>(Resource.Mips.Num()))
                {
                    continue;   // no ref and no bytes, so the worker fails the load on this slice
                }

                const FTextureResource::FMip& MipData = Resource.Mips[MipIndex];
                const uint32 Slice = SliceIndex(Layer, Mip);

                // Also the correct fallback when a failed save left the ref pointing into an unwritten region.
                if (!MipData.Pixels.IsEmpty())
                {
                    MipBytes[Slice] = MipData.Pixels;
                }
                else
                {
                    MipRefs[Slice] = MipData.BulkRef;
                }
            }
        }
    }

    void FTextureStreamingManager::FPendingLoad::Read()
    {
        LUMINA_PROFILE_SECTION("Texture Stream Read");
        LUMINA_MEMORY_SCOPE("Texture Streaming");

        // A destroyed package still reads safely here and fails on its removed file, which the game thread then drops.
        bFailed = !Package;

        for (int32 Slice = 0; Slice < MipBytes.Num() && !bFailed; ++Slice)
        {
            if (!MipBytes[Slice].IsEmpty())
            {
                continue;   // handed over already resident
            }

            bFailed = !MipRefs[Slice].IsValid() || !Package->ReadBulkData(MipRefs[Slice], MipBytes[Slice], BulkGeneration);
        }

        bComplete.store(true, std::memory_order_release);
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

        for (int32 i = 0; i < PendingLoads.Num(); )
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
                            [](const TVector<uint8>& Bytes) { return Bytes.Num(); });

                        for (uint32 Layer = 0; Layer < Load.LayerCount; ++Layer)
                        {
                            for (uint32 Mip = Load.TargetFirstMip; Mip < Load.SourceFirstMip; ++Mip)
                            {
                                const uint32 Index = Resource.MipIndex(Layer, Mip);
                                if (Index < static_cast<uint32>(Resource.Mips.Num()))
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
                        else
                        {
                            ReleaseMipsBelow(Resource, NowFirstMip);
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
            PendingLoads[i] = Move(PendingLoads.Last());
            PendingLoads.Pop();
        }
    }

    FTextureStreamingManager::FStats FTextureStreamingManager::GetStats() const
    {
        FScopeLock Lock(Mutex);

        FStats Stats;
        Stats.BudgetBytes         = GetBudgetBytes();
        Stats.ResidentBytes       = ResidentBytesTotal;
        Stats.NumTextures         = (uint32)Textures.Num();
        Stats.NumLoadsInFlight    = (uint32)PendingLoads.Num();
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

        OutTextures.Reset();
        OutTextures.Reserve(Textures.Num());

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
                Row.CpuBytes += Mip.Pixels.Num();
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

            OutTextures.Add(Move(Row));
        }

        OutPending.Reset();
        OutPending.Reserve(PendingLoads.Num());

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
                Row.Bytes += Bytes.Num();
            }

            OutPending.Add(Move(Row));
        }
    }
}
