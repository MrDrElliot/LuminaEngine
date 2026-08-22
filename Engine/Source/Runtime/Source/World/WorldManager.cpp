#include "RuntimePCH.h"
#include "WorldManager.h"
#include "Core/Object/Package/Package.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Engine/Engine.h"
#include "Core/Engine/GameInstance.h"
#include "Core/Profiler/Profile.h"
#include "Renderer/ImmediateLineRenderer.h"
#include "Renderer/RenderManager.h"
#include "TaskSystem/TaskSystem.h"


namespace Lumina
{
    RUNTIME_API FWorldManager* GWorldManager = nullptr;

    static TConsoleVar<float> CVarIdleReclaimSeconds("Editor.RenderScene.IdleReclaimSeconds", 3.0f,
        "Seconds a hidden world's render scene is kept resident before being freed.");

    static TConsoleVar<bool> CVarParallelWorldRender("r.ParallelWorldRender", true,
        "Record each world's render commands on a separate task thread (only engages with >1 live world).");

    FWorldManager::~FWorldManager()
    {
        // Tear down in reverse so PIE/derived contexts go before their source.
        for (auto It = Contexts.rbegin(); It != Contexts.rend(); ++It)
        {
            if ((*It)->World.IsValid())
            {
                (*It)->World->TeardownWorld();
            }
        }
        Contexts.clear();
    }

    void FWorldManager::BeginFrame(double NowSeconds)
    {
        LUMINA_PROFILE_SCOPE();

        for (const TUniquePtr<FWorldContext>& Context : Contexts)
        {
            if (CWorld* World = Context->World.Get())
            {
                World->AdvanceThrottle(NowSeconds);
            }
        }
    }

    void FWorldManager::UpdateWorlds(const FUpdateContext& UpdateContext)
    {
        LUMINA_PROFILE_SCOPE();

        for (const TUniquePtr<FWorldContext>& Context : Contexts)
        {
            CWorld* World = Context->World.Get();
            if (World == nullptr || !World->IsTickingThisFrame())
            {
                continue;
            }

            World->Update(UpdateContext);
        }
    }

    void FWorldManager::BeginImmediateLines()
    {
        LUMINA_PROFILE_SCOPE();

        // Mirrors ExtractWorlds's filter exactly, or a producer fills a buffer nobody snapshots.
        for (const TUniquePtr<FWorldContext>& Context : Contexts)
        {
            CWorld* World = Context->World.Get();
            if (World == nullptr || World->GetRenderer() == nullptr)
            {
                continue;
            }

            if (!World->IsTickingThisFrame())
            {
                if (FImmediateLineRenderer* Lines = World->GetRenderer()->GetImmediateLines())
                {
                    Lines->CloseFrame();
                }
                continue;
            }

            World->GetRenderer()->BeginImmediateLines();
        }
    }

    void FWorldManager::ReclaimIdleRenderers(double NowSeconds)
    {
        LUMINA_PROFILE_SCOPE();

        const double Grace = (double)CVarIdleReclaimSeconds.GetValue();

        // One reclaim per frame, since DestroyRenderer calls WaitIdle and batching stalls hard.
        for (const TUniquePtr<FWorldContext>& Context : Contexts)
        {
            CWorld* World = Context->World.Get();
            if (World != nullptr && World->ReclaimIdleRenderer(NowSeconds, Grace))
            {
                break;
            }
        }
    }

    void FWorldManager::TickPhysics()
    {
        LUMINA_PROFILE_SCOPE();

        for (const TUniquePtr<FWorldContext>& Context : Contexts)
        {
            CWorld* World = Context->World.Get();
            if (World == nullptr || !World->IsTickingThisFrame())
            {
                continue;
            }

            World->TickPhysics();

            // Dispatched on the thread that stepped it, so the queue never outlives its frame.
            World->DispatchPhysicsEvents();
        }
    }

    void FWorldManager::ExtractWorlds()
    {
        LUMINA_PROFILE_SCOPE();

        // Opens a new extract generation, which is what keeps retained slots alive while drawn.
        if (FRenderManager* RenderManager = TryRender())
        {
            RenderManager->GetReleaseQueue().BeginExtract();
        }

        for (const TUniquePtr<FWorldContext>& Context : Contexts)
        {
            CWorld* World = Context->World.Get();
            // Skip renderer-less worlds (dedicated server) so the editor never extracts an invisible world.
            if (World == nullptr || !World->IsTickingThisFrame() || World->GetRenderer() == nullptr)
            {
                continue;
            }

            World->Extract();
        }
    }

    void FWorldManager::RenderWorlds(uint8 FrameIndex)
    {
        LUMINA_PROFILE_SCOPE();
        
        // bExtractedThisFrame clears at the start of Extract, so a idle world keeps it set.
        uint32 LiveWorlds = 0;
        for (const TUniquePtr<FWorldContext>& Context : Contexts)
        {
            CWorld* World = Context->World.Get();
            IRenderScene* Renderer = World ? World->GetRenderer() : nullptr;
            if (Renderer == nullptr || !World->IsTickingThisFrame())
            {
                continue;
            }
            Renderer->PrepareRender(FrameIndex);
            ++LiveWorlds;
        }
        
        auto RecordContext = [&](uint32 i)
        {
            CWorld* World = Contexts[i]->World.Get();
            IRenderScene* Renderer = World ? World->GetRenderer() : nullptr;
            if (Renderer == nullptr)
            {
                return;
            }
            // Same gate as the prepare loop, so a throttled viewport keeps showing its last frame.
            if (World->IsTickingThisFrame())
            {
                Renderer->RenderView(FrameIndex);
            }
        };

        // Parallel only earns its keep with multiple live worlds; one world takes the serial path.
        if (CVarParallelWorldRender.GetValue() && LiveWorlds > 1)
        {
            Task::ParallelFor((uint32)Contexts.size(), [&](const Task::FParallelRange& Range)
            {
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    RecordContext(i);
                }
            }, 1);
        }
        else
        {
            for (uint32 i = 0; i < (uint32)Contexts.size(); ++i)
            {
                RecordContext(i);
            }
        }

        // Must stay after the record loop, including the parallel branch.
        if (FRenderManager* RenderManager = TryRender())
        {
            RenderManager->GetReleaseQueue().EndRender();
        }
    }

    FWorldContext* FWorldManager::CreateWorldContext(CWorld* World, EWorldType Type, ENetMode NetMode)
    {
        if (World == nullptr)
        {
            return nullptr;
        }

        if (FWorldContext* Existing = FindContext(World))
        {
            return Existing;
        }

        TUniquePtr<FWorldContext> Context = MakeUnique<FWorldContext>();
        Context->World   = World;
        Context->Type    = Type;
        Context->NetMode = NetMode;

        FWorldContext* Raw = Context.get();
        
        Contexts.push_back(Move(Context));

        World->OwningContext = Raw;
        
        CGameInstance* GameInstance = ((Type == EWorldType::Game || Type == EWorldType::Simulation) && GEngine != nullptr) ? GEngine->GetGameInstance() : nullptr;
        if (GameInstance != nullptr)
        {
            GameInstance->PreWorldLoad(World);
        }

        World->InitializeWorld(Type);

        if (GameInstance != nullptr)
        {
            GameInstance->PostWorldLoad(World);
        }

        return Raw;
    }

    void FWorldManager::DestroyWorldContext(CWorld* World)
    {
        if (World == nullptr)
        {
            return;
        }

        for (size_t i = 0; i < Contexts.size(); ++i)
        {
            if (Contexts[i]->World.Get() == World)
            {
                World->TeardownWorld();
                World->OwningContext = nullptr;

                size_t Last = Contexts.size() - 1;
                if (i != Last)
                {
                    std::swap(Contexts[i], Contexts[Last]);
                }
                Contexts.pop_back();
                return;
            }
        }

        // Not registered, tear down anyway so the caller's expectations hold.
        World->TeardownWorld();
    }

    FWorldContext* FWorldManager::FindContext(CWorld* World)
    {
        for (const TUniquePtr<FWorldContext>& Context : Contexts)
        {
            if (Context->World.Get() == World)
            {
                return Context.get();
            }
        }
        return nullptr;
    }

    FWorldContext* FWorldManager::GetPrimaryGameContext()
    {
        for (const TUniquePtr<FWorldContext>& Context : Contexts)
        {
            if (Context->Type == EWorldType::Game)
            {
                return Context.get();
            }
        }
        return nullptr;
    }

    CWorld* FWorldManager::StartPIE(CWorld* SourceWorld, EWorldType SessionType, ENetMode NetMode)
    {
        if (SourceWorld == nullptr)
        {
            return nullptr;
        }

        CWorld* PIEWorld = CWorld::DuplicateWorld(SourceWorld);
        if (PIEWorld == nullptr)
        {
            return nullptr;
        }

        FWorldContext* Ctx = CreateWorldContext(PIEWorld, SessionType, NetMode);
        if (Ctx != nullptr)
        {
            Ctx->bPIE = true;
            Ctx->SourceWorld = SourceWorld;

            // Map identity is the editor source map's path, which lets a PIE client skip travel.
            if (CPackage* Pkg = SourceWorld->GetPackage())
            {
                Ctx->MapPath = FString(Pkg->GetName().c_str());
            }
        }

        return PIEWorld;
    }

    void FWorldManager::StopPIE(CWorld* PIEWorld)
    {
        DestroyWorldContext(PIEWorld);
    }
}
