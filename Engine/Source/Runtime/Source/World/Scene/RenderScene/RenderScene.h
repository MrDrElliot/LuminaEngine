#pragma once
#include "SceneRenderTypes.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "Renderer/RHIFwd.h"
#include "World/Entity/EntityHandle.h"


namespace Lumina
{
    class CWorld;
    class FViewVolume;
    class CMaterialInterface;
    struct SPostProcessSettings;

    /**
     * A world's renderer. The engine creates one per rendered world through RenderSceneFactory,
     * so a project or plugin can replace the default (FForwardRenderScene) with its own
     * implementation -- see RenderSceneFactory.h.
     *
     * Threading contract: Extract runs on the game thread; PrepareRender / RenderView /
     * SignalFrameConsumed run on the render thread. Everything else is game thread unless noted.
     *
     * Only the pure-virtual core is required. Every optional feature (picking, capture views,
     * post-process, debug draw, shadow atlas) defaults to "not supported" so a minimal renderer
     * stays minimal.
     */
    class IRenderScene : public IPrimitiveDrawInterface
    {
    public:

        explicit IRenderScene(CWorld* InWorld)
            : World(InWorld)
        {}
        virtual ~IRenderScene() = default;

        //~ Required core -------------------------------------------------------------------

        // Two-phase construction is required here (and only here): Init runs virtual calls and hands
        // `this` to systems, neither of which works from a constructor. Teardown has no such excuse --
        // each class's destructor releases what it owns, so there is no Shutdown() to pair with it.
        virtual void Init() = 0;

        // Game thread, populate the frame slot's snapshot. N-buffered so Extract and RenderView run
        // concurrently; Extract back-pressures on the slot's consumed fence.
        virtual void Extract(const FViewVolume& ViewVolume, const SPostProcessSettings* PostProcess) = 0;

        // Render thread, record + submit this frame's rendering. Safe to run concurrently across scenes
        // (each opens its own command list; shared RHI creation is internally locked).
        virtual void RenderView(uint8 FrameIndex) = 0;

        // Re-create the scene's render target at a new size. Used by transient render paths
        // (e.g. thumbnail capture) needing a fixed RT independent of the swapchain.
        virtual void Resize(const FUIntVector2& NewSize) = 0;

        // Pixel extent of the scene's render target. Use this for sizing.
        virtual FUIntVector2 GetRenderExtent() const = 0;

        //~ Frame hooks ---------------------------------------------------------------------

        // Render thread, serial: device-wide reconciliation that can't run while other scenes record
        // (e.g. WaitDeviceIdle-guarded resource recreation). Runs for every scene before the parallel
        // RenderView pass, so RenderView can record off-thread.
        virtual void PrepareRender(uint8 FrameIndex) {}

        // Render thread: release the slot after the last CPU read for this frame.
        virtual void SignalFrameConsumed(uint8 FrameIndex) {}

        // Post-process material chain for this frame, resolved by the world from the active camera +
        // volumes. Not retained across frames -- the world rebuilds the list each tick.
        virtual void SetActivePostProcessMaterials(const TVector<CMaterialInterface*>& Materials) {}

        //~ Display output ------------------------------------------------------------------

        // Global-heap ResourceID of the scene's final display image (for ImGui viewport sampling).
        // ~0u = none.
        virtual uint32 GetDisplayResourceID() const { return ~0u; }

        // RHI handle of the final display image, for in-CL composite passes (game present blit,
        // RmlUi world overlay). Invalid handle = none.
        virtual RHI::FTextureH GetDisplayTexture() const { return {}; }

        //~ Entity picking (editor) ---------------------------------------------------------

        virtual entt::entity GetEntityAtPixel(uint32 X, uint32 Y) const { return entt::null; }

        // Pick-cursor position (picker-RT texels) so readback copies just a region around it.
        // bOverViewport=false skips the readback that frame.
        virtual void SetPickerCursor(uint32 X, uint32 Y, bool bOverViewport) {}

        //~ Scene-capture views -------------------------------------------------------------

        // Render the world from an extra camera into its own RT (gather once, shade each).
        // Register returns an opaque handle (-1 on fail); display via the capture's heap ResourceID.
        // SetCaptureView returns false for a handle this scene doesn't know (e.g. it predates a scene
        // rebuild); the caller must re-register. Handles are only meaningful to the scene that issued them.
        virtual int32 RegisterCaptureView(const FUIntVector2& Size) { return -1; }
        virtual bool  SetCaptureView(int32 Handle, const FViewVolume& View, bool bEnabled) { return false; }
        virtual int32 GetCaptureDisplayResourceID(int32 Handle) const { return -1; }

        //~ Debug draw (IPrimitiveDrawInterface) --------------------------------------------

        void DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale) override {}
        void DrawLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness, bool bDepthTest, float Duration) override {}

        //~ Stats / settings ----------------------------------------------------------------

        const FSceneRenderStats&  GetRenderStats() const        { return RenderStats; }
        FSceneRenderSettings&     GetSceneRenderSettings()      { return RenderSettings; }

        // The scene's shadow atlas, or null if the scene has none.
        virtual const FShadowAtlas* GetShadowAtlas() const { return nullptr; }

        CWorld* GetWorld() const { return World; }

    protected:

        CWorld*                 World = nullptr;
        FSceneRenderStats       RenderStats;
        FSceneRenderSettings    RenderSettings;
    };
}
