#pragma once

#include "RenderResource.h"   // EFormat
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CTexture;

    /**
     * Decides how much of each texture's mip chain is resident on the GPU.
     *
     * Textures load holding only their inline tail (see kInlineMipMaxDimension) and are promoted from there
     * on demand -- by an explicit Pin (an editor tab wants the real thing) or by the renderer reporting how
     * much of the screen the texture actually covers.
     *
     * Each frame runs Wanted -> Budgeted -> residency. WantedFirstMip is pure quality from coverage;
     * BudgetedFirstMip reconciles it against the pool, shedding a single mip at a time across the whole
     * set in retention order so the scene degrades uniformly rather than a few textures collapsing.
     * Residency then follows Budgeted in both directions.
     *
     * The whole scheme rests on RHI::Textures::Recreate repointing the existing bindless heap slot instead
     * of allocating a new one. A texture's ResourceID is baked into every material uniform block that
     * samples it and is never revisited, so a residency change that moved the slot would silently repoint
     * those materials at whatever landed there next. See project bindless-slot-lifetime notes.
     */
    class FTextureStreamingManager
    {
    public:

        /** Optional-subsystem accessor, same contract as TryRender(): null when headless, during early
         *  init, and after shutdown. Never asserts -- a texture built before the renderer exists has to be
         *  able to skip registration rather than take the process down. */
        RUNTIME_API static FTextureStreamingManager* TryGet();

        static void Initialize();
        static void Shutdown();

        /** Called from CTexture::PostLoad once the inline tail is on the GPU. Ignores non-streamable
         *  textures (fully inline chains) -- there is nothing to decide for them. */
        void RegisterTexture(CTexture* Texture);
        void UnregisterTexture(CTexture* Texture);

        /** Hold Texture fully resident until a matching Unpin. Reference counted: two editor tabs on one
         *  texture each hold their own claim, and the texture stays pinned until both are closed. Pinned
         *  textures are exempt from budget eviction, so a pin is a promise, not a hint. */
        RUNTIME_API void Pin(CTexture* Texture);
        RUNTIME_API void Unpin(CTexture* Texture);

        /** Per-frame: retire completed IO, apply residency changes, evict when over budget.
         *  Game thread only -- it calls CTexture::ApplyMipResidency, which touches the RHI. */
        void Update();

        /** GPU feedback, indexed by MATERIAL slot: Masks[Slot] is the OR of the absolute resolutions every
         *  pixel shading that material needed (see RequestMaterialResolution in SceneGlobals.slang). One
         *  atomic per pixel rather than one per texture sample, which is the whole reason it is keyed this
         *  way; the per-texture demand is recovered here by expanding each material slot through the
         *  uniform mirror's texture IDs, and a required RESOLUTION lands on the right mip for each texture
         *  regardless of their differing sizes.
         *
         *  Called once per frame on the game thread with a readback that is kFramesInFlight old. */
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

        /** One row per registered texture, for the streaming debug tool. Copies rather than exposing the
         *  live array, because the tool draws from the game thread while loads complete on workers. */
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

            /** GPU feedback, relative to current residency: bit N = "mip N of what is resident was
             *  sampled". 0 with bFeedbackValid means nothing sampled it this readback. */
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

            /** Bytes the GPU image currently holds, cached so the budget sweep doesn't re-walk every mip. */
            uint64 ResidentBytes = 0;
            uint64 FullBytes     = 0;

            uint32 PinCount = 0;

            /** Quality target, from coverage alone, ignoring the pool entirely. 0 == fully resident. */
            uint8  WantedFirstMip = 0;

            /** What the pool actually allows: starts at WantedFirstMip and is pushed coarser only when the
             *  budget cannot hold everything. Keeping these separate is what stops the budgeter and the
             *  quality policy overwriting each other -- with one field, "what we want" was indistinguishable
             *  from "what we settled for", and the eviction pass had to clobber it every frame. */
            uint8  BudgetedFirstMip = 0;

            /** Highest residency the texture can drop to -- its inline tail. */
            uint8  TailFirstMip = 0;

            /** Frame of the last non-trivial demand, for retention priority. */
            uint64 LastDemandFrame = 0;

            /** Consecutive frames the budget has wanted this texture coarser than it currently is. A
             *  dead-band against thrash: crossing a mip boundary while walking past a wall would otherwise
             *  demote and re-promote every few frames, and each swap is a full image realloc + re-upload. */
            uint16 FramesWantingCoarser = 0;

            /** GPU feedback: the mip mask the shaders OR'd for this texture's heap slot, RELATIVE to what
             *  is resident (bit 0 = "the finest mip I have is still not fine enough"). ~0u == the readback
             *  reported nothing, which is different from "reported 0": not sampled at all. */
            uint32 FeedbackMask = 0u;
            bool   bFeedbackValid = false;

            /** Frame the last non-empty feedback arrived, for the not-sampled-in-a-while decay. */
            uint64 LastFeedbackFrame = 0;

            bool   bLoadInFlight = false;
        };

        /** One in-flight promotion. The worker only reads bytes off disk; the residency change itself is
         *  applied on the game thread, because it touches the RHI and the CTexture. */
        struct FPendingLoad
        {
            TWeakObjectPtr<CTexture> Texture;
            uint8                    TargetFirstMip = 0;

            /** Bytes for levels [TargetFirstMip, SourceFirstMip) of EVERY layer, laid out layer-major --
             *  see SliceIndex. An array texture needs all of its layers before ApplyMipResidency will
             *  accept the promotion (it rejects on any layer having empty pixels), so loading only
             *  layer 0 leaves it retrying the same read forever, pinned at its inline tail. */
            TVector<TVector<uint8>>  MipBytes;

            /** Where each of those slices lives on disk, snapshotted on the GAME THREAD when the load is
             *  issued. The worker must not reach back into the texture for them: a save refills
             *  FMip::Pixels and a re-cook resizes Mips outright, and either one under a running read is a
             *  use-after-free. Invalid here means the slice was handed over already resident in MipBytes
             *  (or that there was nothing to read, which fails the load). */
            TVector<FBulkDataRef>    MipRefs;

            uint8                    SourceFirstMip = 0;
            uint32                   LayerCount     = 1;

            uint32 MipSpan() const { return (uint32)(SourceFirstMip - TargetFirstMip); }
            uint32 SliceIndex(uint32 Layer, uint32 Mip) const { return Layer * MipSpan() + (Mip - TargetFirstMip); }

            TAtomic<bool>            bComplete{false};
            bool                     bFailed = false;
        };

        FStreamingTexture* Find(CTexture* Texture);


        /** Turn this frame's coverage reports into WantedFirstMip. Pure quality, no budget. */
        void ComputeWantedMips();

        /** Reconcile Wanted against the pool into BudgetedFirstMip. Under budget the two are identical;
         *  over it, mips are shed ONE AT A TIME across the whole set in retention order, so the scene
         *  degrades uniformly instead of a few textures collapsing to their tail. */
        void ComputeBudgetedMips();

        /** Bring residency down to BudgetedFirstMip. Never needs IO -- everything between the tail and the
         *  current level is already in memory. */
        void ApplyDemotions();

        /** Kick IO for textures below their budget, most-starved first. */
        void IssuePromotions();

        /** Apply completed loads and drop finished records. */
        void ProcessCompletedLoads();

        /** Push the staged images' remaining host uploads, within this frame's shared budget. */
        void TickResidencyFills();

        /** Higher = keep. Pinned is unbeatable; otherwise recent, high-coverage textures hold their mips. */
        static float RetentionPriority(const FStreamingTexture& Entry, uint64 FrameCounter);

        uint64 GetBudgetBytes() const;

        TVector<FStreamingTexture>              Textures;
        THashMap<CTexture*, uint32>             TextureToIndex;

        /** Bindless sampled slot -> index into Textures. Rebuilt inside SubmitMaterialFeedback rather than
         *  maintained, because a slot moves whenever a texture is recreated and a mapping that has to be
         *  kept in sync is exactly what made the previous material-keyed attempt fail silently. Held as a
         *  member only to keep its storage across frames. */
        THashMap<uint32, uint32>                SlotToEntry;

        TVector<TUniquePtr<FPendingLoad>>       PendingLoads;

        /** Host-upload bytes left this frame, reset at the top of Update and drawn down by BOTH the staged
         *  fills and the newly applied loads. Shared deliberately: two stages each spending "the budget"
         *  would land twice the spike MaxUploadMBPerFrame names. */
        uint64                                  FrameUploadBudget = 0;

        // Residency changes left this frame, promotions and demotions together. Meters image create/retire
        // churn, which a demotion pays in full while spending no host bytes.
        uint32                                  FrameResidencyChanges = 0;

        /** Where TickResidencyFills starts spending the frame's budget, advanced every frame. Registry
         *  order alone let whatever sits early take all of it, every frame, forever -- see the comment
         *  there. Purely a fairness hint: it indexes a list that registration reorders, and nothing about
         *  correctness depends on where it lands. */
        SIZE_T                                  FillCursor = 0;

        /** Guards Textures/TextureToIndex against registration from the async-load path and from
         *  CTexture::OnDestroy, both of which can run while Update is walking the list. */
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
