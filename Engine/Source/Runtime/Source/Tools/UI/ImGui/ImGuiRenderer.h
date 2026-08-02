#pragma once

#include "imgui.h"
#include "ImDrawDataSnapshot.h"
#include "ImGuiX.h"
#include "Containers/Array.h"
#include "Core/Threading/Atomic.h"
#include "Renderer/RHI.h"

#include <condition_variable>
#include <mutex>

struct ImPlotContext;

namespace Lumina
{
    class FRenderManager;
    class FUpdateContext;
}

namespace Lumina
{
    class IImGuiRenderer
    {
    public:

        virtual ~IImGuiRenderer() = default;

        virtual void Initialize();
        virtual void Deinitialize();

        void StartFrame(const FUpdateContext& UpdateContext);

        // Game thread: ImGui::Render() then swap DrawData into FrameIndex's slot. Returns a pointer
        // into the renderer-owned ring (forward to render thread), or nullptr if no valid draw data.
        FImDrawDataSnapshot* BuildFrame_GameThread(uint8 FrameIndex);

        // Render thread: call after recording to release the slot; the game thread
        // may be blocked in BuildFrame_GameThread waiting for it to wrap back around.
        void SignalSnapshotSlotConsumed(uint8 FrameIndex);

        // Releases persistent snapshot storage. Must be called BEFORE
        // ImGui::DestroyContext() since pooled ImDrawLists use its allocator.
        void ClearSnapshots();

        virtual void OnStartFrame(const FUpdateContext& UpdateContext) = 0;

        // New-RHI record: draw the snapshot directly into the acquired swapchain image
        // (Target) via the new RHI. The frame loop has set the heap + acquire barrier.
        virtual void OnEndFrame_NewRHI(RHI::FCmdListH CL, RHI::FTextureH Target, const FUIntVector2& Extent, FImDrawDataSnapshot& Snapshot) {}

        // Multi-viewport (windows dragged out of the main window into their own OS windows). Capture
        // runs on the game thread inside BuildFrame (deep-copies each secondary viewport's draw data
        // into the frame slot); render runs on the render thread and presents each secondary swapchain.
        virtual void CaptureSecondaryViewports_GameThread(uint8 FrameIndex) {}
        virtual void RenderSecondaryViewports_RenderThread(uint8 FrameIndex) {}

        // Game thread: process pending ImGui textures before handing off the snapshot, else 1.92's
        // backend does it lazily in RenderDrawData against shared state and races. Hold the gfx-queue lock.
        virtual void ProcessTextureUpdates_GameThread() {}

        virtual ImTextureID GetOrCreateImTexture(FStringView Path) = 0;

        RUNTIME_API ImGuiContext* GetImGuiContext() const { return Context; }
        RUNTIME_API ImPlotContext* GetImPlotContext() const { return ImPlotContext; }
        
    protected:

        ImGuiContext* Context = nullptr;
        ImPlotContext* ImPlotContext = nullptr;

        // Persistent ring keyed by render-thread frame index; each slot pools reused ImDrawList
        // copies, so after warm-up SnapUsingSwap is allocation-free unless buffers grow.
        FImDrawDataSnapshot Snapshots[RHI::kFramesInFlight];

        // Per-slot producer/consumer counters (independent of the world FrameRing) so the game thread
        // blocks rather than stomp a snapshot the render thread is still recording from.
        TAtomic<uint64> SnapshotProduced[RHI::kFramesInFlight] = {};
        TAtomic<uint64> SnapshotConsumed[RHI::kFramesInFlight] = {};
        std::mutex SnapshotSlotMutex;
        std::condition_variable SnapshotSlotCV;

        void WaitForSnapshotSlot(uint8 Slot, uint64 Target);
    };
}
