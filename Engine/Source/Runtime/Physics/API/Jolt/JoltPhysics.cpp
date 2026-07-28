#include "pch.h"
#include "JoltPhysics.h"
#include <Core/Console/ConsoleVariable.h>

#include <algorithm>
#include <cstring>
#include "JoltPhysicsScene.h"
#include "JoltJobSystemBridge.h"
#include "Core/Threading/Thread.h"
#include "Jolt/RegisterTypes.h"
#include "Jolt/Core/Factory.h"
#include "Memory/MemoryTracking.h"
#include "Physics/API/Jolt/JoltUtils.h"
#include "World/World.h"
#include "Log/Log.h"

static_assert(sizeof(JPH::ObjectLayer) == 4);

#if defined(LE_PLATFORM_WINDOWS)
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent();
#else
static int IsDebuggerPresent() { return 0; }
#endif

namespace Lumina::Physics
{
    static TUniquePtr<FJoltData> JoltData;

    static TConsoleVar CVarJoltUseEngineJobSystem("Physics.Jolt.UseEngineJobSystem", true,
        "Route Jolt physics jobs through the engine fiber scheduler instead of Jolt's own thread pool. Read once at physics init.");

    #if JPH_DEBUG_RENDERER
    static JPH::BodyManager::DrawSettings DebugDrawSettings;

    static TConsoleVar CVarJoltDebug("Jolt.Debug.Draw", false, "Toggles debug drawing for Jolt Physics, has severe performance impact.");

    static TConsoleVar CVarJoltDebugShapes("Jolt.Debug.Shapes", DebugDrawSettings.mDrawShape, "Toggles debugging shapes for Jolt Physics", [](const auto& Var)
        {
            DebugDrawSettings.mDrawShape = eastl::get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugShapeWireframe("Jolt.Debug.ShapeWireframe", DebugDrawSettings.mDrawShapeWireframe, "Toggles wireframe rendering for shapes", [](const auto& Var)
        {
            DebugDrawSettings.mDrawShapeWireframe = eastl::get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugAABB("Jolt.Debug.AABB", DebugDrawSettings.mDrawBoundingBox, "Toggles debugging AABB for Jolt Physics", [](const auto& Var)
        {
            DebugDrawSettings.mDrawBoundingBox = eastl::get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugVelocity("Jolt.Debug.Velocity", DebugDrawSettings.mDrawVelocity, "Toggles debugging velocity vectors for Jolt Physics", [](const auto& Var)
        {
            DebugDrawSettings.mDrawVelocity = eastl::get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugCenterOfMass("Jolt.Debug.CenterOfMass", DebugDrawSettings.mDrawCenterOfMassTransform, "Toggles center of mass visualization", [](const auto& Var)
        {
            DebugDrawSettings.mDrawCenterOfMassTransform = eastl::get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugWorldTransform("Jolt.Debug.WorldTransform", DebugDrawSettings.mDrawWorldTransform, "Toggles world transform axes visualization", [](const auto& Var)
        {
            DebugDrawSettings.mDrawWorldTransform = eastl::get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugSleepStats("Jolt.Debug.SleepStats", DebugDrawSettings.mDrawSleepStats, "Toggles sleep statistics visualization", [](const auto& Var)
        {
            DebugDrawSettings.mDrawSleepStats = eastl::get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugGetSupport("Jolt.Debug.GetSupport", DebugDrawSettings.mDrawGetSupportFunction, "Toggles GetSupport function visualization for collision detection", [](const auto& Var)
        {
            DebugDrawSettings.mDrawGetSupportFunction = eastl::get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugGetSupportDirection("Jolt.Debug.GetSupportDir", DebugDrawSettings.mDrawGetSupportingFace, "Toggles GetSupportingFace visualization", [](const auto& Var)
        {
            DebugDrawSettings.mDrawGetSupportingFace = eastl::get<bool>(Var);
        });
    #endif
    
    #ifdef JPH_ENABLE_ASSERTS
    static void JoltTraceCallback(const char* format, ...)
    {
        va_list list;
        va_start(list, format);
        char buffer[1024];
        (void)vsnprintf(buffer, sizeof(buffer), format, list);

        if (JoltData)
        {
            JoltData->LastErrorMessage = buffer;
        }
        LOG_TRACE("Jolt Physics - {}", buffer);
    }
    #endif

    // Tag at the hook, not call-site: Jolt's job threads would miss a call-site scope.
    static void* JPHCustomAllocate(size_t size)
    {
        LUMINA_MEMORY_SCOPE("Physics");
        return Memory::Malloc(size);
    }

    static void* JPHCustomReallocate(void* block, size_t oldSize, size_t newSize)
    {
        LUMINA_MEMORY_SCOPE("Physics");
        return Memory::Realloc(block, newSize);
    }

    static void JPHCustomFree(void* block)
    {
        Memory::Free(block);
    }

    static void* JPHCustomAlignedAllocate(size_t size, size_t alignment)
    {
        LUMINA_MEMORY_SCOPE("Physics");
        return Memory::Malloc(size, alignment);
    }

    static void JPHCustomAlignedFree(void* block)
    {
        Memory::Free(block);
    }
    
    static bool JoltAssertionFailed(const char* expr, const char* msg, const char* file, uint32 line)
    {
        LOG_CRITICAL("JOLT ASSERT FAILED: Message {}, File: {} - {}", expr, msg, file, line);

        // EPhysicsUpdateError (cache overflow) is recoverable and content-driven; never break on it.
        if (expr != nullptr && std::strstr(expr, "EPhysicsUpdateError") != nullptr)
        {
            return false;
        }

        // Any other assert is a genuine bug: break only when a debugger is attached, otherwise
        // log and continue so a standalone run does not hard-crash on STATUS_BREAKPOINT.
        return ::IsDebuggerPresent() != 0;
    }

    void FJoltPhysicsContext::Initialize()
    {
        // Must be JPH_ENABLE_ASSERTS, not JPH_ASSERT: the latter is function-like, so `#if JPH_ASSERT`
        // is 0 and the handler never installs (Jolt's always-break default runs instead).
        #ifdef JPH_ENABLE_ASSERTS
        JPH::Trace              = JoltTraceCallback;
        JPH::AssertFailed       = JoltAssertionFailed;
        #endif
        
        JPH::Reallocate         = JPHCustomReallocate;
        JPH::Allocate           = JPHCustomAllocate;
        JPH::Free               = JPHCustomFree;
        JPH::AlignedAllocate    = JPHCustomAlignedAllocate;
        JPH::AlignedFree        = JPHCustomAlignedFree;

        JoltData = MakeUnique<FJoltData>();
        #if JPH_DEBUG_RENDERER
		JoltData->DebugRenderer = MakeUnique<FJoltDebugRenderer>();
        #endif
        JPH::Factory::sInstance = Memory::New<JPH::Factory>();
        
        #if JPH_DEBUG_RENDERER
        JPH::DebugRenderer::sInstance = JoltData->DebugRenderer.get();
        #endif
        JPH::RegisterTypes();
        
        int NumJoltThreads = (int)Threading::GetNumThreads() - 3;
        NumJoltThreads = std::max(NumJoltThreads, 1);

        if (CVarJoltUseEngineJobSystem.GetValue())
        {
            // +1 mirrors Jolt's own pool: the thread calling WaitForJobs also executes jobs.
            JoltData->JobSystem = MakeUnique<FJoltJobSystemBridge>(2048, 8, NumJoltThreads + 1);
            LOG_DISPLAY("[Jolt] Physics jobs routed through the engine fiber scheduler (max concurrency {}).", NumJoltThreads + 1);
        }
        else
        {
            JoltData->JobSystem = MakeUnique<JPH::JobSystemThreadPool>(2048, 8, NumJoltThreads);
        }
    }

    void FJoltPhysicsContext::Shutdown()
    {
        JPH::UnregisterTypes();
        JoltData.reset();
        
        #if JPH_DEBUG_RENDERER
		JPH::DebugRenderer::sInstance = nullptr;
        #endif
        
        Memory::Delete(JPH::Factory::sInstance);
    }

    TUniquePtr<IPhysicsScene> FJoltPhysicsContext::CreatePhysicsScene(CWorld* World)
    {
        return MakeUnique<FJoltPhysicsScene>(World);
    }

    JPH::JobSystem* FJoltPhysicsContext::GetThreadPool()
    {
        return JoltData->JobSystem.get();
    }

    #if JPH_DEBUG_RENDERER
    FJoltDebugRenderer* FJoltPhysicsContext::GetDebugRenderer()
    {
        return JoltData->DebugRenderer.get();
    }

    void FJoltDebugRenderer::DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor)
    {
        float DrawDuration = (float)std::max(World->GetWorldDeltaTime(), Duration);
        World->DrawLine(JoltUtils::FromJPHVec3(inFrom), JoltUtils::FromJPHVec3(inTo), FVector4(inColor.r, inColor.g, inColor.b, inColor.a), 1.0f, DrawDuration);
    }

    void FJoltDebugRenderer::DrawBodies(JPH::PhysicsSystem* System, CWorld* InWorld)
    {
        World = InWorld;
        
        if (SCameraComponent* Camera = World->GetActiveCamera())
        {
            SetCameraPos(JoltUtils::ToJPHRVec3(Camera->GetPosition()));
            #if JPH_DEBUG_RENDERER
            System->DrawBodies(DebugDrawSettings, this);
            #endif
        }
    }
    #endif
}

namespace JPH
{
    #if JPH_EXTERNAL_PROFILE
    ExternalProfileMeasurement::ExternalProfileMeasurement(const char* inName, uint32 inColor /* = 0 */)
        : mUserData{}
    {
        
    }
    
    ExternalProfileMeasurement::~ExternalProfileMeasurement()
    {
        
    }
    #endif
}
