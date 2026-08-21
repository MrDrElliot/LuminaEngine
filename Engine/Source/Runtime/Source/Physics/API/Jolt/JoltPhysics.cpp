#include "RuntimePCH.h"
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
#include "Renderer/ImmediateLineRenderer.h"
#include "World/World.h"
#include "World/Entity/Systems/DebugDrawSystem.h"
#include "Log/Log.h"
#include <Jolt/Physics/Body/BodyFilter.h>

static_assert(sizeof(JPH::ObjectLayer) == 4);

#if defined(LE_PLATFORM_WINDOWS)
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent();
#else
[[maybe_unused]] static int IsDebuggerPresent() { return 0; }
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
            DebugDrawSettings.mDrawShape = Containers::Get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugShapeWireframe("Jolt.Debug.ShapeWireframe", DebugDrawSettings.mDrawShapeWireframe, "Toggles wireframe rendering for shapes", [](const auto& Var)
        {
            DebugDrawSettings.mDrawShapeWireframe = Containers::Get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugAABB("Jolt.Debug.AABB", DebugDrawSettings.mDrawBoundingBox, "Toggles debugging AABB for Jolt Physics", [](const auto& Var)
        {
            DebugDrawSettings.mDrawBoundingBox = Containers::Get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugVelocity("Jolt.Debug.Velocity", DebugDrawSettings.mDrawVelocity, "Toggles debugging velocity vectors for Jolt Physics", [](const auto& Var)
        {
            DebugDrawSettings.mDrawVelocity = Containers::Get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugCenterOfMass("Jolt.Debug.CenterOfMass", DebugDrawSettings.mDrawCenterOfMassTransform, "Toggles center of mass visualization", [](const auto& Var)
        {
            DebugDrawSettings.mDrawCenterOfMassTransform = Containers::Get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugWorldTransform("Jolt.Debug.WorldTransform", DebugDrawSettings.mDrawWorldTransform, "Toggles world transform axes visualization", [](const auto& Var)
        {
            DebugDrawSettings.mDrawWorldTransform = Containers::Get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugSleepStats("Jolt.Debug.SleepStats", DebugDrawSettings.mDrawSleepStats, "Toggles sleep statistics visualization", [](const auto& Var)
        {
            DebugDrawSettings.mDrawSleepStats = Containers::Get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugGetSupport("Jolt.Debug.GetSupport", DebugDrawSettings.mDrawGetSupportFunction, "Toggles GetSupport function visualization for collision detection", [](const auto& Var)
        {
            DebugDrawSettings.mDrawGetSupportFunction = Containers::Get<bool>(Var);
        });

    static TConsoleVar CVarJoltDebugGetSupportDirection("Jolt.Debug.GetSupportDir", DebugDrawSettings.mDrawGetSupportingFace, "Toggles GetSupportingFace visualization", [](const auto& Var)
        {
            DebugDrawSettings.mDrawGetSupportingFace = Containers::Get<bool>(Var);
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

    // Tagged at the hook, since Jolt's job threads would miss a call-site scope.
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
    
    #ifdef JPH_ENABLE_ASSERTS
    static bool JoltAssertionFailed(const char* expr, const char* msg, const char* file, uint32 line)
    {
        LOG_CRITICAL("JOLT ASSERT FAILED: Message {}, File: {} - {}", expr, msg, file, line);

        // EPhysicsUpdateError (cache overflow) is recoverable and content-driven; never break on it.
        if (expr != nullptr && std::strstr(expr, "EPhysicsUpdateError") != nullptr)
        {
            return false;
        }

        // Break only under a debugger, so a standalone run logs and continues rather than hard-crashing.
        return ::IsDebuggerPresent() != 0;
    }
    #endif

    void FJoltPhysicsContext::Initialize()
    {
        // JPH_ASSERT is function-like, so testing it is 0 and the handler would never install.
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
        NumJoltThreads = Math::Max(NumJoltThreads, 1);

        if (CVarJoltUseEngineJobSystem.GetValue())
        {
            // The plus one mirrors Jolt's own pool, where the thread calling WaitForJobs also executes jobs.
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
        // Building the uint directly skips a float round-trip that fed 0-255 into a 0-1 clamp.
        const uint32 Packed = ((uint32)inColor.a << 24) | ((uint32)inColor.b << 16)
                            | ((uint32)inColor.g <<  8) |  (uint32)inColor.r;

        if (Lines != nullptr)
        {
            Lines->Line(JoltUtils::FromJPHVec3(inFrom), JoltUtils::FromJPHVec3(inTo), Packed);
            return;
        }

        // A one-off query draw that wants a lifetime goes through the timed batcher instead.
        const float DrawDuration = (float)Math::Max(World->GetWorldDeltaTime(), Duration);
        World->DrawLine(JoltUtils::FromJPHVec3(inFrom), JoltUtils::FromJPHVec3(inTo),
                        FVector4(inColor.r / 255.0f, inColor.g / 255.0f, inColor.b / 255.0f, inColor.a / 255.0f),
                        1.0f, true, DrawDuration);
    }

    namespace
    {
        // The immediate path's culling lives here, at source granularity rather than per line.
        class FDebugDrawBodyFilter final : public JPH::BodyDrawFilter
        {
        public:

            explicit FDebugDrawBodyFilter(const FDebugDrawState& InState)
                : State(InState)
            {}

            bool ShouldDraw(const JPH::Body& Body) const override
            {
                const JPH::AABox& Bounds = Body.GetWorldSpaceBounds();
                return DebugDraw::ShouldDraw(State,
                    FAABB(JoltUtils::FromJPHVec3(Bounds.mMin), JoltUtils::FromJPHVec3(Bounds.mMax)));
            }

        private:

            const FDebugDrawState& State;
        };
    }

    void FJoltDebugRenderer::DrawBodies(JPH::PhysicsSystem* System, CWorld* InWorld)
    {
        World = InWorld;

        const FDebugDrawState* State = DebugDraw::GetState(World);
        if (State == nullptr || !State->bEnabled || !State->bHasView)
        {
            return;
        }

        // Jolt uses the camera position for its own LOD, so it gets the same view the cull runs against.
        SetCameraPos(JoltUtils::ToJPHRVec3(State->ViewOrigin));

        SetImmediateSink(DebugDraw::GetLines(World));

        const FDebugDrawBodyFilter Filter(*State);
        System->DrawBodies(DebugDrawSettings, this, &Filter);

        // Sink is per-frame; leaving it set would let a later query draw write into a closed window.
        SetImmediateSink(nullptr);
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
