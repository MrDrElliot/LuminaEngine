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
        // Game thread only, since renderers and script reloads both run there.
        TVector<FManagedRenderScene*>   GLiveScenes;
        TVector<CWorld*>                GPendingRecreate;

        // The C# type the factory override instantiates; set by PostScriptLoad from the loaded generation.
        FString                         GActiveTypeName;

        TUniquePtr<IRenderScene> CreateForWorld(CWorld* World)
        {
            // Returning null makes the factory fall back to the engine default for editor worlds.
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
        // The caller has already waited for the GPU, so nothing still references the handle.
        DestroyManagedRenderScene(Handle);
        Handle = nullptr;

        auto It = Algo::Find(GLiveScenes, this);
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

            // Managed if the new generation still ships one, otherwise the engine default.
            for (CWorld* World : GPendingRecreate)
            {
                World->CreateRenderer();
            }
            GPendingRecreate.clear();
        }
    }

    namespace
    {
        void LmRenderSceneNameSink(void* Ctx, const char* Name, int Len)
        {
            auto* Out = static_cast<TVector<FString>*>(Ctx);
            if (Out != nullptr && Name != nullptr && Len > 0)
            {
                Out->emplace_back(FString(Name, static_cast<size_t>(Len)));
            }
        }

        // Declared here rather than in the host: a render-scene export is this file's business, and
        // adding one costs a line here instead of a field, a typedef and a resolve in DotNetHost.
        const TManagedExport<void (*)(void*, void*)> EnumerateRenderScenes{ "EnumerateRenderScenes" };
        const TManagedExport<void* (*)(const char*, int32, uint64)> CreateRenderScene{ "CreateRenderScene" };
        const TManagedExport<void (*)(void*)> DestroyRenderScene{ "DestroyRenderScene" };
        const TManagedExport<void (*)(void*, const void*)> RenderSceneExtract{ "RenderSceneExtract" };
        const TManagedExport<void (*)(void*, int32)> RenderSceneRender{ "RenderSceneRender" };
        const TManagedExport<void (*)(void*, uint32, uint32)> RenderSceneResize{ "RenderSceneResize" };
        const TManagedExport<uint64 (*)(void*)> RenderSceneGetDisplayTexture{ "RenderSceneGetDisplayTexture" };
        const TManagedExport<uint32 (*)(void*)> RenderSceneGetDisplayResourceID{ "RenderSceneGetDisplayResourceID" };
        const TManagedExport<void (*)(void*, uint32*, uint32*)> RenderSceneGetExtent{ "RenderSceneGetExtent" };
    }
    void GatherManagedRenderSceneTypes(TVector<FString>& Out)
    {
        Out.clear();
        if (IsInitialized() && EnumerateRenderScenes)
        {
            EnumerateRenderScenes.Get()(reinterpret_cast<void*>(&LmRenderSceneNameSink), &Out);
        }
    }

    void* CreateManagedRenderScene(FStringView TypeName, uint64 World)
    {
        if (!IsInitialized() || !CreateRenderScene)
        {
            return nullptr;
        }
        return CreateRenderScene.Get()(TypeName.data(), (int32)TypeName.size(), World);
    }

    void DestroyManagedRenderScene(void* Handle)
    {
        if (IsInitialized() && DestroyRenderScene && Handle)
        {
            DestroyRenderScene.Get()(Handle);
        }
    }

    void ManagedRenderSceneExtract(void* Handle, const void* View)
    {
        if (IsInitialized() && RenderSceneExtract && Handle)
        {
            RenderSceneExtract.Get()(Handle, View);
        }
    }

    void ManagedRenderSceneRender(void* Handle, int32 FrameIndex)
    {
        if (IsInitialized() && RenderSceneRender && Handle)
        {
            RenderSceneRender.Get()(Handle, FrameIndex);
        }
    }

    void ManagedRenderSceneResize(void* Handle, uint32 Width, uint32 Height)
    {
        if (IsInitialized() && RenderSceneResize && Handle)
        {
            RenderSceneResize.Get()(Handle, Width, Height);
        }
    }

    uint64 ManagedRenderSceneGetDisplayTexture(void* Handle)
    {
        if (!IsInitialized() || !RenderSceneGetDisplayTexture || Handle == nullptr)
        {
            return 0;
        }
        return RenderSceneGetDisplayTexture.Get()(Handle);
    }

    uint32 ManagedRenderSceneGetDisplayResourceID(void* Handle)
    {
        if (!IsInitialized() || !RenderSceneGetDisplayResourceID || Handle == nullptr)
        {
            return ~0u;
        }
        return RenderSceneGetDisplayResourceID.Get()(Handle);
    }

    void ManagedRenderSceneGetExtent(void* Handle, uint32* OutWidth, uint32* OutHeight)
    {
        if (IsInitialized() && RenderSceneGetExtent && Handle)
        {
            RenderSceneGetExtent.Get()(Handle, OutWidth, OutHeight);
        }
    }
}
