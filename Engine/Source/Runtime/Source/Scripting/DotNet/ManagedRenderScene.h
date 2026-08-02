#pragma once
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"
#include "World/Scene/RenderScene/RenderScene.h"


namespace Lumina::DotNet
{
    // Blittable camera snapshot handed to the managed OnExtract. Mirrored 1:1 by LuminaSharp's
    // SceneView (Renderer/RenderScene.cs); any change here must update the C# struct and GAbiVersion.
    struct FManagedSceneView
    {
        FMatrix4    View;
        FMatrix4    Projection;
        FMatrix4    ViewProjection;
        FVector3    Position;      float FOV;
        FVector3    Forward;       float NearZ;
        FVector3    Up;            float FarZ;
        FVector3    Right;         float AspectRatio;
    };

    static_assert(sizeof(FManagedSceneView) == 256, "FManagedSceneView layout drifted; update the C# SceneView mirror + GAbiVersion");

    /**
     * IRenderScene proxy over a C# RenderScene subclass. The engine treats it like any renderer; every
     * virtual forwards across the interop boundary to the managed instance (a strong GCHandle).
     *
     * Lifetime vs hot reload: managed handles die with their script generation, so ManagedRenderScenes::
     * PreScriptUnload destroys every live proxy (through CWorld::DestroyRenderer, which flushes the render
     * thread first) BEFORE the ALC unloads, and PostScriptLoad recreates the affected worlds' renderers
     * against the new generation. A proxy therefore never outlives its generation.
     */
    class FManagedRenderScene : public IRenderScene
    {
    public:

        FManagedRenderScene(CWorld* InWorld, const FString& InTypeName);
        ~FManagedRenderScene() override;
        LE_NO_COPYMOVE(FManagedRenderScene);

        void Init() override;

        void Extract(const FViewVolume& ViewVolume, const SPostProcessSettings* PostProcess) override;
        void RenderView(uint8 FrameIndex) override;
        void Resize(const FUIntVector2& NewSize) override;

        FUIntVector2 GetRenderExtent() const override;
        uint32 GetDisplayResourceID() const override;
        RHI::FTextureH GetDisplayTexture() const override;

    private:

        FString     TypeName;
        void*       Handle = nullptr;
    };

    namespace ManagedRenderScenes
    {
        // Script (re)load bracket, called by DotNetHost::LoadScriptUnitsCore on the game thread.
        // PreScriptUnload tears down every world renderer backed by a managed scene (the old generation's
        // GCHandles are about to die); PostScriptLoad re-syncs the RenderSceneFactory override against the
        // freshly loaded types and recreates the renderers PreScriptUnload tore down.
        void PreScriptUnload();
        void PostScriptLoad();
    }
}
