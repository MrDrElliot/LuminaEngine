#pragma once
#include "SceneRenderTypes.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "Renderer/RHIFwd.h"
#include "World/Scene/SceneInterface.h"


namespace Lumina
{
    class FViewVolume;
    class CMaterialInterface;
    struct SPostProcessSettings;

    class IRenderScene : public ISceneInterface, public IPrimitiveDrawInterface
    {
    public:
        virtual ~IRenderScene() = default;

        virtual void Init() = 0;
        virtual void Shutdown() = 0;

        // Frame boundary, scene prepares/clears per-frame state
        virtual void BeginFrame() = 0;
        virtual void EndFrame() = 0;

        // Post-process material chain for this frame, resolved by the world from the active camera +
        // volumes. Not retained across frames -- the world rebuilds the list each tick.
        virtual void SetActivePostProcessMaterials(const TVector<CMaterialInterface*>& Materials) {}

        // Game thread, populate the frame slot's snapshot. N-buffered so Extract and RenderView run
        // concurrently; Extract back-pressures on the slot's consumed fence.
        virtual void Extract(const FViewVolume&, const SPostProcessSettings* PostProcess) = 0;

        // Render thread, serial: device-wide reconciliation that can't run while other scenes record
        // (e.g. WaitDeviceIdle-guarded resource recreation). Runs for every scene before the parallel
        // RenderView pass, so RenderView can record off-thread.
        virtual void PrepareRender(uint8 FrameIndex) {}

        // Render thread, record + submit this frame's rendering. Safe to run concurrently across scenes
        // (each opens its own command list; shared RHI creation is internally locked).
        virtual void RenderView(uint8 FrameIndex) {}

        // New-heap ResourceID of the scene's final display image (for new-RHI ImGui viewport
        // sampling). ~0u = none (old scenes that render to an old-RHI target).
        virtual uint32 GetDisplayResourceID() const { return ~0u; }

        // New-RHI handle of the final display image, for in-CL composite passes
        // (RmlUi world overlay). Invalid handle = none.
        virtual RHI::FTextureH GetDisplayTexture() const { return {}; }

        // Render thread: release the slot after the last CPU read for this frame.
        virtual void SignalFrameConsumed(uint8 FrameIndex) {}
        
        virtual entt::entity GetEntityAtPixel(uint32 X, uint32 Y) const = 0;

        // Editor only: pick-cursor position (picker-RT texels) so readback copies just a region around it.
        // bOverViewport=false skips the readback that frame.
        virtual void SetPickerCursor(uint32 X, uint32 Y, bool bOverViewport) {}

        // Pixel extent of the scene's render target. Use this for sizing.
        virtual FUIntVector2 GetRenderExtent() const = 0;

        // Scene-capture views: render the world from an extra camera into its own RT (gather once, shade each).
        // Register returns an opaque handle (-1 on fail); display via the capture's heap ResourceID.
        // SetCaptureView returns false for a handle this scene doesn't know (e.g. it predates a scene
        // rebuild); the caller must re-register. Handles are only meaningful to the scene that issued them.
        virtual int32 RegisterCaptureView(const FUIntVector2& Size) { return -1; }
        virtual bool  SetCaptureView(int32 Handle, const FViewVolume& View, bool bEnabled) { return false; }
        virtual int32 GetCaptureDisplayResourceID(int32 Handle) const { return -1; }

        // Re-create the scene's render target at a new size. Used by transient render paths
        // (e.g. thumbnail capture) needing a fixed RT independent of the swapchain.
        virtual void Resize(const FUIntVector2& NewSize) = 0;

        virtual const FSceneRenderStats&  GetRenderStats() const = 0;
        virtual FSceneRenderSettings&     GetSceneRenderSettings() = 0;

        // Returns the scene's shadow atlas, or null if the scene has none
        // (forward has one, deferred does not yet).
        virtual const FShadowAtlas* GetShadowAtlas() const { return nullptr; }
    };
}
