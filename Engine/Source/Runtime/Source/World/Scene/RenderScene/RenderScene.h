#pragma once

#include "World/ECS/Registry.h"
#include "SceneRenderTypes.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "Renderer/RHIFwd.h"

namespace Lumina
{
    class CWorld;
    class FViewVolume;
    class CMaterialInterface;
    class FImmediateLineRenderer;
    struct SPostProcessSettings;

    class IRenderScene : public IPrimitiveDrawInterface
    {
    public:

        explicit IRenderScene(CWorld* InWorld)
            : World(InWorld)
        {}
        virtual ~IRenderScene() = default;

        //~ Required core -------------------------------------------------------------------

        virtual void Init() = 0;

        // Gather this frame's scene from the ECS.
        virtual void Extract(const FViewVolume& ViewVolume, const SPostProcessSettings* PostProcess) = 0;

        virtual void RenderView(uint8 FrameIndex) = 0;

        virtual void Resize(const FUIntVector2& NewSize) = 0;

        /**
         * Sizes the primary view to the surface actually displaying it, in pixels, and stops it tracking
         * the swapchain.
         */
        virtual void SetPrimaryViewSize(const FUIntVector2& SizePixels) {}

        // Pixel extent of the scene's render target. Use this for sizing.
        virtual FUIntVector2 GetRenderExtent() const = 0;

        //~ Frame hooks ---------------------------------------------------------------------

        virtual void PrepareRender(uint8 FrameIndex) {}

        virtual void SetActivePostProcessMaterials(const TVector<CMaterialInterface*>& Materials) {}

        //~ Display output ------------------------------------------------------------------

        virtual uint32 GetDisplayResourceID() const { return ~0u; }

        virtual RHI::FTextureH GetDisplayTexture() const { return {}; }

        //~ Entity picking (editor) ---------------------------------------------------------

        virtual ECS::FEntity GetEntityAtPixel(uint32 X, uint32 Y) const { return ECS::NullEntity; }

        virtual void SetPickerCursor(uint32 X, uint32 Y, bool bOverViewport) {}

        //~ Scene-capture views -------------------------------------------------------------

        virtual int32 RegisterCaptureView(const FUIntVector2& Size) { return -1; }
        virtual bool  SetCaptureView(int32 Handle, const FViewVolume& View, bool bEnabled) { return false; }
        virtual int32 GetCaptureDisplayResourceID(int32 Handle) const { return -1; }

        //~ Debug draw (IPrimitiveDrawInterface) --------------------------------------------

        void DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale) override {}
        void DrawLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness, bool bDepthTest, float Duration) override {}

        virtual FImmediateLineRenderer* GetImmediateLines() { return nullptr; }

        virtual void BeginImmediateLines() {}

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
