#pragma once

#include "RenderResource.h"   // EFormat
#include "Containers/HashTable.h"
#include "Containers/Pair.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Serialization/BulkDataRef.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CTexture;

    // Decides how much of each texture's mip chain is resident, relying on Textures::Recreate keeping its bindless slot.
    class FTextureStreamingManager
    {
    public:

        // Null when headless, during early init and after shutdown, so a texture skips registration instead of asserting.
        RUNTIME_API static FTextureStreamingManager* TryGet();

        static void Initialize();
        static void Shutdown();

        // Called from CTexture::PostLoad, and ignores textures whose whole chain is inline.
        void RegisterTexture(CTexture* Texture);
        void UnregisterTexture(CTexture* Texture);

        // Reference counted, and a pinned texture is exempt from budget eviction.
        RUNTIME_API void Pin(CTexture* Texture);
        RUNTIME_API void Unpin(CTexture* Texture);

        // Game thread only, since applying residency touches the RHI.
        void Update();

        // Masks is indexed by material slot and holds the resolutions its pixels needed, read back kFramesInFlight late.
        RUNTIME_API void SubmitMaterialFeedback(const uint32* Masks, uint32 Count);

        struct FStats
        {
            uint64 ResidentBytes    = 0;
            uint64 FullyResidentBytes = 0;   // what the same set would cost with no streaming
            uint64 BudgetBytes      = 0;
            uint32 NumTextures      = 0;
            uint32 NumPinned        = 0;
            uint32 NumLoadsInFlight = 0;
            uint32 NumPromotedLastFrame = 0;
            uint32 NumDemotedLastFrame  = 0;

            // Cumulative since startup. The counters are what distinguish "the budget happens to fit" from
            // "streaming is actually running": a session with zero promotions is not streaming, it just
            // never needed to.
            uint64 TotalPromotions  = 0;
            uint64 TotalDemotions   = 0;
            uint64 TotalBytesRead   = 0;
            uint64 TotalFailedLoads = 0;
            uint64 FrameCounter     = 0;
        };

        RUNTIME_API FStats GetStats() const;

        // Copied rather than live, because the debug tool draws while loads complete on workers.
        struct FTextureSnapshot
        {
            FName    Name;
            FString  PackagePath;

            uint32   Width  = 0;
            uint32   Height = 0;
            EFormat  Format = EFormat::UNKNOWN;
            int32    ResourceID = -1;

            uint8    NumMips          = 0;
            uint8    ResidentFirstMip = 0;
            uint8    WantedFirstMip   = 0;
            uint8    BudgetedFirstMip = 0;
            uint8    TailFirstMip     = 0;

            uint64   ResidentBytes = 0;
            uint64   FullBytes     = 0;
            uint64   CpuBytes      = 0;   // mip pixels still held in RAM

            uint32   PinCount = 0;

            // Bit N means mip N of the resident chain was sampled.
            uint32   FeedbackMask  = 0;
            bool     bFeedbackValid = false;
            uint64   FramesSinceDemand = 0;
            bool     bLoadInFlight = false;
        };

        struct FPendingSnapshot
        {
            FName  Name;
            uint8  TargetFirstMip = 0;
            uint8  SourceFirstMip = 0;
            uint64 Bytes = 0;
            bool   bComplete = false;
        };

        RUNTIME_API void GetSnapshot(TVector<FTextureSnapshot>& OutTextures, TVector<FPendingSnapshot>& OutPending) const;

    private:

        struct FStreamingTexture
        {
            TWeakObjectPtr<CTexture> Texture;

            // Cached so the budget sweep does not re-walk every mip.
            uint64 ResidentBytes = 0;
            uint64 FullBytes     = 0;

            uint32 PinCount = 0;

            // Quality target from coverage alone, where 0 is fully resident.
            uint8  WantedFirstMip = 0;

            // WantedFirstMip pushed coarser only when the budget cannot hold everything.
            uint8  BudgetedFirstMip = 0;

            // The inline tail, the lowest residency the texture can drop to.
            uint8  TailFirstMip = 0;

            // Frame of the last non-trivial demand, for retention priority.
            uint64 LastDemandFrame = 0;

            // Dead band against demote and re-promote thrash at a mip boundary.
            uint16 FramesWantingCoarser = 0;

            // Mip mask relative to residency, where bit 0 means the finest resident mip is not fine enough.
            uint32 FeedbackMask = 0u;
            bool   bFeedbackValid = false;

            // Frame the last non-empty feedback arrived, for the not-sampled decay.
            uint64 LastFeedbackFrame = 0;

            bool   bLoadInFlight = false;
        };

        // The worker only reads bytes, and the residency change is applied on the game thread.
        struct FPendingLoad
        {
            TWeakObjectPtr<CTexture> Texture;
            uint8                    TargetFirstMip = 0;

            // Levels [TargetFirstMip, SourceFirstMip) of every layer in SliceIndex order, since an array promotes only whole.
            TVector<TVector<uint8>>  MipBytes;

            // Taken on the game thread at issue because a save or re-cook would free them mid-read; invalid means already in MipBytes.
            TVector<FBulkDataRef>    MipRefs;

            // The package's bulk generation when those refs were taken; a save since then invalidates them.
            uint32                   BulkGeneration = 0;

            uint8                    SourceFirstMip = 0;
            uint32                   LayerCount     = 1;

            // Charged at issue and stored, because applying the load moves MipBytes out and empties them.
            uint64                   StagingBytes   = 0;

            // Pool bytes the promotion adds, held against the budget until the load retires.
            uint64                   GrowthBytes    = 0;

            uint32 MipSpan() const { return (uint32)(SourceFirstMip - TargetFirstMip); }
            uint32 SliceIndex(uint32 Layer, uint32 Mip) const { return Layer * MipSpan() + (Mip - TargetFirstMip); }

            TAtomic<bool>            bComplete{false};
            bool                     bFailed = false;
        };

        FStreamingTexture* Find(CTexture* Texture);


        // Pure quality from coverage, ignoring the budget.
        void ComputeWantedMips();

        // Sheds one mip at a time across the set in retention order until the pool holds it.
        void ComputeBudgetedMips();

        // Never needs IO, since every mip between the tail and the current level is in memory.
        void ApplyDemotions();

        // Kicks IO for textures below their budget, most starved first.
        void IssuePromotions();

        void ProcessCompletedLoads();

        // Pushes staged host uploads within this frame's shared budget.
        void TickResidencyFills();

        // Higher keeps, with pinned always winning and then recent high-coverage textures.
        static float RetentionPriority(const FStreamingTexture& Entry, uint64 FrameCounter);

        uint64 GetBudgetBytes() const;

        // Sort keys are precomputed into these so the comparator never reaches back into Textures.
        TVector<TPair<float, uint32>>           BudgetOrderScratch;
        TVector<TPair<float, uint32>>           BudgetUnresidentScratch;
        TVector<TPair<float, uint32>>           PromotionScratch;

        TVector<FStreamingTexture>              Textures;
        THashMap<CTexture*, uint32>             TextureToIndex;

        // Bindless slot to Textures index, rebuilt every submit because a slot moves when its texture is recreated.
        THashMap<uint32, uint32>                SlotToEntry;

        TVector<TUniquePtr<FPendingLoad>>       PendingLoads;

        // Sum of StagingBytes over PendingLoads; charged at issue, refunded when the load is retired.
        uint64                                  PendingLoadBytes = 0;

        // Shared by staged fills and newly applied loads, so one frame never uploads more than MaxUploadMBPerFrame.
        uint64                                  FrameUploadBudget = 0;

        // Residency changes left this frame, promotions and demotions together. Meters image create/retire
        // churn, which a demotion pays in full while spending no host bytes.
        uint32                                  FrameResidencyChanges = 0;

        // Set when a promotion was refused because the pool was full, which lets demotions skip their dead band.
        bool                                    bPromotionBlockedByPool = false;

        // Rotating start for TickResidencyFills, so early entries cannot take the whole budget every frame.
        int32                                   FillCursor = 0;

        // Guards Textures and TextureToIndex against the async-load path and CTexture::OnDestroy.
        mutable FMutex                          Mutex;

        uint64  FrameCounter = 0;
        uint64  ResidentBytesTotal = 0;
        uint32  PromotedLastFrame = 0;
        uint32  DemotedLastFrame = 0;

        uint64  TotalPromotions  = 0;
        uint64  TotalDemotions   = 0;
        uint64  TotalBytesRead   = 0;
        uint64  TotalFailedLoads = 0;
    };
}
