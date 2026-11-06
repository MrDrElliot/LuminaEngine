#pragma once

#include "World/ECS/Registry.h"
#include "SceneRenderTypes.h"
#include "WorldRenderSettings.h"
#include "RenderCallbacks.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "Renderer/RHIFwd.h"
#include "Lumina.h"

namespace Lumina
{
    class CWorld;
    class FViewVolume;
    class CMaterialInterface;
    class FImmediateLineRenderer;
    struct SPostProcessSettings;
    struct FViewLayerSettings;

    class IRenderScene : public IPrimitiveDrawInterface
    {
    public:

        explicit IRenderScene(CWorld* InWorld)
            : World(InWorld)
        {}
        virtual ~IRenderScene() = default;

        //~ Required core

        virtual void Init() = 0;

        // Gather this frame's scene from the ECS.
        virtual void Extract(const FViewVolume& ViewVolume, const SPostProcessSettings* PostProcess, const FViewLayerSettings& Layers) = 0;

        virtual void RenderView(uint8 FrameIndex) = 0;

        virtual void Resize(const FUIntVector2& NewSize) = 0;

        // Sizes the primary view in pixels to the surface displaying it and stops it tracking the swapchain.
        virtual void SetPrimaryViewSize(const FUIntVector2& SizePixels) {}

        // Pixel extent of the displayed image, which input, picking and deprojection are measured in.
        virtual FUIntVector2 GetRenderExtent() const = 0;

        //~ Frame hooks

        virtual void PrepareRender(uint8 FrameIndex) {}

        virtual void SetActivePostProcessMaterials(const TVector<CMaterialInterface*>& Materials) {}

        //~ Display output

        virtual uint32 GetDisplayResourceID() const { return Constants::kIndexNoneU32; }

        // False until a frame has composited into the display target, which is what makes it opaque.
        virtual bool HasCompositedFrame() const { return true; }

        virtual RHI::FTextureH GetDisplayTexture() const { return {}; }

        //~ Entity picking (editor)

        virtual ECS::FEntity GetEntityAtPixel(uint32 X, uint32 Y) const { return ECS::NullEntity; }

        // The world-space surface and its normal under a pixel, read through foliage and Ignored. False when nothing near it counts.
        virtual bool GetSurfaceAtPixel(uint32 X, uint32 Y, TSpan<const ECS::FEntity> Ignored, FVector3& OutPosition, FVector3& OutNormal) const { return false; }

        // Copies the whole picker, depth and foliage images back once, which GetSurfaceAtPixel prefers while the camera holds still.
        virtual void RequestSurfaceSnapshot() {}
        virtual void ReleaseSurfaceSnapshot() {}
        virtual bool IsSurfaceSnapshotReady() const { return false; }

        virtual void SetPickerCursor(uint32 X, uint32 Y, bool bOverViewport) {}

        //~ Scene-capture views

        virtual int32 RegisterCaptureView(const FUIntVector2& Size) { return -1; }
        virtual bool  SetCaptureView(int32 Handle, const FViewVolume& View, bool bEnabled) { return false; }
        virtual int32 GetCaptureDisplayResourceID(int32 Handle) const { return -1; }

        //~ Render callbacks

        // A renderer that runs render callbacks builds their pipelines here, and the default creates none.
        virtual RHI::FPipelineH GetCallbackPipeline(const FRenderPipelineDesc& Desc, TSpan<const RHI::FColorTarget> ColorTargets, EFormat DepthFormat) { return {}; }
        virtual RHI::FPipelineH GetCallbackComputePipeline(FShaderH ComputeShader) { return {}; }

        //~ Debug draw (IPrimitiveDrawInterface)

        void DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale) override {}
        void DrawLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness, bool bDepthTest, float Duration) override {}

        virtual FImmediateLineRenderer* GetImmediateLines() { return nullptr; }

        virtual void BeginImmediateLines() {}

        //~ Stats and settings

        const FSceneRenderStats&  GetRenderStats() const        { return RenderStats; }

        // The scene's shadow atlas, or null if the scene has none.
        virtual const FShadowAtlas* GetShadowAtlas() const { return nullptr; }

        CWorld* GetWorld() const { return World; }

    protected:

        // Copied from the world once per extract, since the render thread must not read live world state.
        RUNTIME_API void RefreshFrameSettings();

        CWorld*                 World = nullptr;
        FSceneRenderStats       RenderStats;
        SWorldRenderSettings    FrameSettings;
        FSceneFrameFlags        FrameFlags;
    };
}
