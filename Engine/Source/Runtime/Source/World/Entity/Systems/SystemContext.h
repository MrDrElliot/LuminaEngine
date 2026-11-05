#pragma once

#include "World/ECS/Registry.h"
#include "World/ECS/Query.h"
#include "World/ECS/EventQueue.h"
#include "World/ECS/CommandBus.h"

#include "Core/UpdateStage.h"
#include "Physics/PhysicsTypes.h"
#include "Physics/Ray/RayCast.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "Scripting/ScriptCallback.h"
#include "SystemAccess.h"
#include "SystemResources.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/TransformComponent.h"
#include "SystemContext.generated.h"


namespace Lumina
{
    enum class EWorldType : uint8;
    enum class EMoveMode : uint8;

    class CWorld;
    struct FSimpleElementVertex;

    namespace Physics
    {
        class IPhysicsScene;
        class IPhysicsQueries;
    }

    struct SExclusiveContext;

    // What a system's OnUpdate and HasWork receive: only what is safe beside every other system, the same value C# reads through a handle.
    REFLECT()
    struct SSystemContext
    {
        GENERATED_BODY()

    protected:

        friend class CWorld;

        // First, because the C# SSystemContext reads the world from offset zero.
        CWorld*             World = nullptr;
        ECS::FRegistry*     Registry = nullptr;
        ECS::FEventQueues*  Events = nullptr;
        ECS::FCommandBus*   CommandBus = nullptr;

    public:

        SSystemContext() = default;
        RUNTIME_API explicit SSystemContext(CWorld* InWorld);

        //~ Time.

        NODISCARD FORCEINLINE float GetDeltaTime() const { return static_cast<float>(DeltaTime); }

        // Unscaled and running while paused, for effects that ignore time dilation.
        NODISCARD FORCEINLINE float GetRealDeltaTime() const { return static_cast<float>(RealDeltaTime); }
        NODISCARD FORCEINLINE double GetTime() const { return Time; }
        NODISCARD FORCEINLINE EUpdateStage GetUpdateStage() const { return UpdateStage; }
        NODISCARD RUNTIME_API EWorldType GetWorldType() const;

        //~ Structure. A parallel system's changes land at its segment's flush, an exclusive system's at once.

        // The handle is valid at once either way; from a parallel system it gets its components at the flush.
        RUNTIME_API ECS::FEntity Create(const FTransform& Transform, FName EntityName = "Entity") const;
        RUNTIME_API ECS::FEntity Create(FVector3 Location, FName EntityName = "Entity") const;
        RUNTIME_API ECS::FEntity Create(FName EntityName = "Entity") const;

        FUNCTION()
        void Destroy(ECS::FEntity Entity) const
        {
            if (ECS::FCommandBus::ShouldDefer())
            {
                CommandBus->Destroy(Entity);
                return;
            }
            Registry->Destroy(Entity);
        }

        // Adds the component, or replaces the one the entity has.
        template<ECS::CComponent T, typename... TArgs>
        void Emplace(ECS::FEntity Entity, TArgs&&... Args) const
        {
            if (ECS::FCommandBus::ShouldDefer())
            {
                CommandBus->Emplace<T>(Entity, std::forward<TArgs>(Args)...);
                return;
            }
            Registry->EmplaceOrReplace<T>(Entity, std::forward<TArgs>(Args)...);
        }

        // Edits the component in place once it exists, so a spawn can be filled in after its Emplace either way.
        template<ECS::CDataComponent T, typename TFunc>
        void Patch(ECS::FEntity Entity, TFunc&& Mutator) const
        {
            if (ECS::FCommandBus::ShouldDefer())
            {
                CommandBus->Patch<T>(Entity, std::forward<TFunc>(Mutator));
                return;
            }
            if (Registry->TryGet<T>(Entity) != nullptr)
            {
                Registry->Patch<T>(Entity, Mutator);
            }
        }

        template<ECS::CComponent T>
        void Remove(ECS::FEntity Entity) const
        {
            if (ECS::FCommandBus::ShouldDefer())
            {
                CommandBus->Remove<T>(Entity);
                return;
            }
            Registry->Remove<T>(Entity);
        }

        FUNCTION()
        RUNTIME_API void SetEntityLifetime(ECS::FEntity Entity, float Lifetime) const;

        FUNCTION()
        NODISCARD RUNTIME_API bool IsValidEntity(ECS::FEntity Entity) const;

        // The parent links, settled at every flush, so any system may walk them.
        NODISCARD RUNTIME_API const ECS::FHierarchy& GetHierarchy() const;

        //~ Physics.

        // Raycasts, sweeps, overlaps, body reads and body forces, all safe beside other systems; null without a physics scene.
        NODISCARD RUNTIME_API Physics::IPhysicsQueries* GetPhysics() const;

        //~ Deferred work.

        // Runs Func at the next flush on the main thread with the whole world in reach, for work a system cannot do beside others.
        template<typename TFunc>
        void Defer(TFunc&& Func) const;

        //~ Debug drawing.

        FUNCTION()
        RUNTIME_API void DrawDebugLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness = 1.0f, float Duration = 1.0f) const;

        FUNCTION()
        RUNTIME_API void DrawDebugBox(const FVector3& Center, const FVector3& Extents, const FQuat& Rotation, const FVector4& Color, float Thickness = 1.0f, float Duration = 1.0f) const;

        FUNCTION()
        RUNTIME_API void DrawDebugSphere(const FVector3& Center, float Radius, const FVector4& Color, uint8 Segments = 16, float Thickness = 1.0f, float Duration = 1.0f) const;

        FUNCTION()
        RUNTIME_API void DrawDebugCone(const FVector3& Apex, const FVector3& Direction, float AngleRadians, float Length, const FVector4& Color, uint8 Segments = 16, uint8 Stacks = 4, float Thickness = 1.0f, float Duration = 1.0f) const;

        FUNCTION()
        RUNTIME_API void DrawFrustum(const FMatrix4& Matrix, float zNear, float zFar, const FVector4& Color, float Thickness = 1.0f, float Duration = 1.0f) const;

        FUNCTION()
        RUNTIME_API void DrawDebugArrow(const FVector3& Start, const FVector3& Direction, float Length, const FVector4& Color, float Thickness = 1.0f, float Duration = 1.0f, float HeadSize = 0.2f) const;

        RUNTIME_API void DrawDebugSolidTriangles(TVector<FSimpleElementVertex>&& Vertices, ESolidDrawMode Mode = ESolidDrawMode::Translucent, float Duration = 1.0f) const;

        //~ C# entry points, which name a component by its FComponentOps token and defer exactly as the templates do.

        FUNCTION(ScriptInternal)
        RUNTIME_API ECS::FEntity ScriptCreate(const FTransform& Transform, FName EntityName) const;

        // The value to fill in place: the live component from an exclusive system, or a staged copy that lands at the flush. Null for a tag.
        FUNCTION(ScriptInternal)
        RUNTIME_API void* ScriptEmplace(ECS::FEntity Entity, void* Ops) const;

        // Copies Source at the call, so it may be a component in the pool the emplace grows.
        FUNCTION(ScriptInternal)
        RUNTIME_API void ScriptEmplaceCopy(ECS::FEntity Entity, void* Ops, void* Source) const;

        FUNCTION(ScriptInternal)
        RUNTIME_API void ScriptRemove(ECS::FEntity Entity, void* Ops) const;

        // Runs the callback once at the next flush with the SExclusiveContext's address, and releases it unrun if the commands are discarded.
        FUNCTION(ScriptInternal)
        RUNTIME_API void ScriptDefer(SScriptCallback Callback) const;

        // False inside a parallel system, so a C# exclusive context forged from a shared one cannot hand out the world.
        FUNCTION(ScriptInternal)
        NODISCARD static RUNTIME_API bool CanReachWorld();

        PROPERTY(Hidden)
        double DeltaTime = 0.0;

        PROPERTY(Hidden)
        double Time = 0.0;

        PROPERTY(Hidden)
        double RealDeltaTime = 0.0;

        PROPERTY(Hidden)
        EUpdateStage UpdateStage = EUpdateStage::FrameStart;
    };

    // What a system running alone on the main thread receives, with the world and its registry in reach.
    REFLECT()
    struct SExclusiveContext : public SSystemContext
    {
        GENERATED_BODY()

        SExclusiveContext() = default;
        explicit SExclusiveContext(const SSystemContext& Shared) : SSystemContext(Shared) {}

        NODISCARD CWorld* GetWorld() const { return World; }
        NODISCARD ECS::FRegistry& GetRegistry() const { return *Registry; }
        NODISCARD ECS::FEventQueues& GetEvents() const { return *Events; }
        NODISCARD ECS::FCommandBus& GetCommandBus() const { return *CommandBus; }
        NODISCARD RUNTIME_API Physics::IPhysicsScene* GetPhysicsScene() const;
    };

    template<typename TFunc>
    void SSystemContext::Defer(TFunc&& Func) const
    {
        CommandBus->Enqueue([Shared = *this, Func = std::forward<TFunc>(Func)]() mutable
        {
            const SExclusiveContext Exclusive(Shared);
            Func(Exclusive);
        });
    }

}
