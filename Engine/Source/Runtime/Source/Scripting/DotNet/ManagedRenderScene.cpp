#include "RuntimePCH.h"
#include "ManagedRenderScene.h"

#include "DotNetHost.h"
#include "Log/Log.h"
#include "Renderer/ViewVolume.h"
#include "World/World.h"
#include "World/Scene/RenderScene/RenderSceneFactory.h"

namespace Lumina::DotNet
{
    namespace
    {
        // Live proxies + the worlds mid-reload teardown left renderer-less. Game thread only: renderers are
        // created/destroyed on the game thread and script (re)loads run there too.
        TVector<FManagedRenderScene*>   GLiveScenes;
        TVector<CWorld*>                GPendingRecreate;

        // The C# type the factory override instantiates; set by PostScriptLoad from the loaded generation.
        FString                         GActiveTypeName;

        TUniquePtr<IRenderScene> CreateForWorld(CWorld* World)
        {
            // Editor/utility worlds (thumbnails, asset previews) keep the engine renderer; returning null
            // makes RenderSceneFactory fall back to the default.
            if (World == nullptr || !World->IsGameWorld())
            {
                return nullptr;
            }
            return MakeUnique<FManagedRenderScene>(World, GActiveTypeName);
        }
    }

    FManagedRenderScene::FManagedRenderScene(CWorld* InWorld, const FString& InTypeName)
        : IRenderScene(InWorld)
        , TypeName(InTypeName)
    {
        GLiveScenes.push_back(this);
    }

    FManagedRenderScene::~FManagedRenderScene()
    {
        // Destroying the proxy releases the managed instance; the caller (CWorld::DestroyRenderer)
        // has already flushed the render thread, so nothing is still referencing the handle.
        DestroyManagedRenderScene(Handle);
        Handle = nullptr;

        auto It = eastl::find(GLiveScenes.begin(), GLiveScenes.end(), this);
        if (It != GLiveScenes.end())
        {
            GLiveScenes.erase(It);
        }
    }

    void FManagedRenderScene::Init()
    {
        Handle = CreateManagedRenderScene(TypeName, reinterpret_cast<uint64>(World));
        if (Handle == nullptr)
        {
            LOG_ERROR("C# RenderScene '{}' failed to create; the world will render nothing.", TypeName.c_str());
        }
    }

    void FManagedRenderScene::Extract(const FViewVolume& ViewVolume, const SPostProcessSettings* PostProcess)
    {
        if (Handle == nullptr)
        {
            return;
        }

        FManagedSceneView Snapshot;
        Snapshot.View           = ViewVolume.GetViewMatrix();
        Snapshot.Projection     = ViewVolume.GetProjectionMatrix();
        Snapshot.ViewProjection = ViewVolume.GetViewProjectionMatrix();
        Snapshot.Position       = ViewVolume.GetViewPosition();
        Snapshot.FOV            = ViewVolume.GetFOV();
        Snapshot.Forward        = ViewVolume.GetForwardVector();
        Snapshot.NearZ          = ViewVolume.GetNear();
        Snapshot.Up             = ViewVolume.GetUpVector();
        Snapshot.FarZ           = ViewVolume.GetFar();
        Snapshot.Right          = ViewVolume.GetRightVector();
        Snapshot.AspectRatio    = ViewVolume.GetAspectRatio();

        ManagedRenderSceneExtract(Handle, &Snapshot);
    }

    void FManagedRenderScene::RenderView(uint8 FrameIndex)
    {
        ManagedRenderSceneRender(Handle, (int32)FrameIndex);
    }

    void FManagedRenderScene::Resize(const FUIntVector2& NewSize)
    {
        ManagedRenderSceneResize(Handle, NewSize.x, NewSize.y);
    }

    FUIntVector2 FManagedRenderScene::GetRenderExtent() const
    {
        uint32 Width = 0, Height = 0;
        ManagedRenderSceneGetExtent(Handle, &Width, &Height);
        // Callers size viewports off this; never report a zero extent.
        return FUIntVector2(Width > 0 ? Width : 16u, Height > 0 ? Height : 16u);
    }

    uint32 FManagedRenderScene::GetDisplayResourceID() const
    {
        return ManagedRenderSceneGetDisplayResourceID(Handle);
    }

    RHI::FTextureH FManagedRenderScene::GetDisplayTexture() const
    {
        return RHI::FTextureH{ ManagedRenderSceneGetDisplayTexture(Handle) };
    }

    namespace ManagedRenderScenes
    {
        void PreScriptUnload()
        {
            GPendingRecreate.clear();

            // DestroyRenderer mutates GLiveScenes (proxy dtor), so walk a snapshot of the owning worlds.
            TVector<CWorld*> Affected;
            for (FManagedRenderScene* Scene : GLiveScenes)
            {
                Affected.push_back(Scene->GetWorld());
            }

            for (CWorld* World : Affected)
            {
                if (World != nullptr)
                {
                    World->DestroyRenderer();
                    GPendingRecreate.push_back(World);
                }
            }
        }

        void PostScriptLoad()
        {
            TVector<FString> Types;
            GatherManagedRenderSceneTypes(Types);

            const bool bOursInstalled = RenderSceneFactory::GetOverride() == &CreateForWorld;

            if (Types.empty())
            {
                GActiveTypeName.clear();
                if (bOursInstalled)
                {
                    RenderSceneFactory::SetOverride(nullptr);
                }
            }
            else
            {
                if (Types.size() > 1)
                {
                    LOG_WARN("Multiple C# RenderScene types loaded ({} found); using '{}'.", Types.size(), Types[0].c_str());
                }
                GActiveTypeName = Types[0];

                if (RenderSceneFactory::HasOverride() && !bOursInstalled)
                {
                    LOG_WARN("C# RenderScene '{}' is replacing a natively installed renderer override.", GActiveTypeName.c_str());
                }
                RenderSceneFactory::SetOverride(&CreateForWorld, GActiveTypeName.c_str());
            }

            // Give the worlds PreScriptUnload tore down a renderer again (managed if the new generation
            // still ships one, engine default otherwise).
            for (CWorld* World : GPendingRecreate)
            {
                World->CreateRenderer();
            }
            GPendingRecreate.clear();
        }
    }
}
