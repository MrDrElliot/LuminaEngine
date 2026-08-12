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
    class CMaterialInterface;

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

        /** Renderer feedback. Coverage is the required texture resolution in pixels (texel factor scaled by
         *  distance, NOT the object's screen size); the largest value reported for a material in a frame
         *  wins. Game thread -- called after the parallel gather has merged, because it walks
         *  CMaterial::ResolvedTextures.
         *
         *  bDensityMeasured says the coverage came from a mesh's baked TexelFactor rather than the
         *  bounding-sphere fallback. Diagnostic only -- it does not change the decision, it just makes
         *  "has this mesh been resaved with texel density yet" visible in the streaming tool. */
        RUNTIME_API void SubmitMaterialCoverage(uint32 MaterialIndex, float ScreenCoveragePixels, bool bDensityMeasured);

        /** Publishes which textures a material slot samples, so coverage reported against that slot can be
         *  turned into per-texture demand without the renderer knowing about textures at all. Called when a
         *  material resolves or rebinds its textures. */
        RUNTIME_API void UpdateMaterialTextures(uint32 MaterialIndex, const TVector<CTexture*>& Textures);
        RUNTIME_API void ForgetMaterial(uint32 MaterialIndex);

        /** Queue a material to publish its texture list on the next Update.
         *
         *  Publishing used to be driven off the per-frame resolve gate, which only runs for primitives that
         *  go through FMeshResolveCache. Dynamic meshes resolve their materials ONCE at commit, so a
         *  dynamic-mesh-only material never published and every coverage report against its slot was
         *  silently dropped. Queueing on the dirty-mark instead makes publication independent of which
         *  render path a material happens to be used by. Safe from any thread. */
        RUNTIME_API void QueueMaterialPublish(CMaterialInterface* Material);

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
            float    LastCoverage = 0.0f;
            uint64   FramesSinceDemand = 0;
            bool     bLoadInFlight = false;
            bool     bDensityMeasured = false;
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

            /** Largest coverage reported this frame, reset every Update. */
            float  FrameCoverage = 0.0f;

            /** Last non-zero coverage, kept across the per-frame reset purely so the debug tool can show
             *  why a texture is at the mip it is at. */
            float  LastCoverage = 0.0f;

            /** Whether the last coverage report came from a mesh's baked texel density rather than the
             *  bounding-sphere fallback. Diagnostic only; does not affect the decision. */
            bool   bDensityMeasured = false;

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
            uint8                    SourceFirstMip = 0;
            uint32                   LayerCount     = 1;

            uint32 MipSpan() const { return (uint32)(SourceFirstMip - TargetFirstMip); }
            uint32 SliceIndex(uint32 Layer, uint32 Mip) const { return Layer * MipSpan() + (Mip - TargetFirstMip); }

            TAtomic<bool>            bComplete{false};
            bool                     bFailed = false;
        };

        FStreamingTexture* Find(CTexture* Texture);

        /** Publish any material queued since the last frame. Runs BEFORE the lock is taken, because
         *  publishing re-enters the manager. */
        void DrainPendingPublishes();

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

        /** Higher = keep. Pinned is unbeatable; otherwise recent, high-coverage textures hold their mips. */
        static float RetentionPriority(const FStreamingTexture& Entry, uint64 FrameCounter);

        uint64 GetBudgetBytes() const;

        TVector<FStreamingTexture>              Textures;
        THashMap<CTexture*, uint32>             TextureToIndex;

        /** MaterialIndex -> the textures that slot samples. Rebuilt whenever a material rebinds. */
        THashMap<uint32, TVector<CTexture*>>    MaterialTextures;

        TVector<TUniquePtr<FPendingLoad>>       PendingLoads;

        /** Materials whose texture set changed and that owe a publish. Weak, because a material can be
         *  destroyed between marking and the next Update. */
        TVector<TWeakObjectPtr<CMaterialInterface>> PendingPublishes;

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
