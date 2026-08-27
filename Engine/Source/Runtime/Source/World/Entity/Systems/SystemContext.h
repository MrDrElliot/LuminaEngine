#pragma once

#include "World/ECS/Registry.h"
#include "World/ECS/EventDispatcher.h"

#include "Core/UpdateStage.h"
#include "Physics/PhysicsTypes.h"
#include "Physics/Ray/RayCast.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "SystemAccess.h"
#include "SystemResources.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/TransformComponent.h"


namespace Lumina
{
    enum class EWorldType : uint8;
    enum class EMoveMode : uint8;

    class CWorld;
    struct FSimpleElementVertex;

    namespace Physics
    {
        class IPhysicsScene;
    }

    struct FSystemContext : INonCopyable
    {
        friend class CWorld;
        friend struct SScriptSystem;
        
        FSystemContext(CWorld* InWorld);
        ~FSystemContext() = default;
        
        RUNTIME_API FORCEINLINE double GetDeltaTime() const { return DeltaTime; }

        // For systems that drive world-level state rather than just components (the sequencer switching
        // the active camera, spawning a binding's prefab).
        RUNTIME_API FORCEINLINE CWorld* GetWorld() const { return World; }
        RUNTIME_API FORCEINLINE double GetTime() const { return Time; }
        RUNTIME_API FORCEINLINE EUpdateStage GetUpdateStage() const { return UpdateStage; }
        
        RUNTIME_API void SetEntityLifetime(ECS::FEntity Entity, float Lifetime) const;


        template<typename T>
        NODISCARD auto EventSink() const
        {
            return Dispatcher.Sink<T>();
        }

        template<typename T, typename ... TArgs>
        void DispatchEvent(TArgs&&... Args) const
        {
            Dispatcher.Trigger<T>(Forward<TArgs>(Args)...);
        }
        
        template<typename... Ts, typename... TArgs>
        NODISCARD auto CreateView(TArgs&&... Args) const -> decltype(std::declval<ECS::FRegistry>().View<Ts...>(std::forward<TArgs>(Args)...))
        {
            return Registry.View<Ts...>(std::forward<TArgs>(Args)...);
        }
        
        template<typename... Ts, typename TFunc, typename... TArgs>
        void ForEach(TFunc&& Function, TArgs&&... Args)
        {
            auto View = Registry.View<Ts...>(std::forward<TArgs>(Args)...);
            View.ForEach(Forward<TFunc>(Function));
        }

        template<typename... Ts, typename TFunc, typename... TArgs>
        void ParallelForEach(TFunc&& Function, TArgs&&... Args)
        {
            auto View = Registry.View<Ts...>(std::forward<TArgs>(Args)...);

            // Chunked, and the range walk reuses the driver's dense index rather than probing twice.
            Task::ParallelFor(static_cast<uint32>(View.NumDenseSlots()), [&](const Task::FParallelRange& Range)
            {
                View.ForEachInRange(Range.Start, Range.End, Function);
            }, 64);
        }

        NODISCARD auto& GetRegistryContext() const
        {
            return Registry.Ctx();
        }

        NODISCARD ECS::FRegistry& GetRegistry() const
        {
            return Registry;
        }
        
        template<typename... Ts, typename ... TArgs>
        NODISCARD auto CreateGroup(TArgs&&... Args) const
        {
            return Registry.View<Ts...>(std::forward<TArgs>(Args)...);
        }

        template<typename... Ts>
        NODISCARD decltype(auto) Get(ECS::FEntity entity) const
        {
            return Registry.Get<Ts...>(entity);
        }

        template<typename... Ts>
        NODISCARD decltype(auto) TryGet(ECS::FEntity entity) const
        {
            return Registry.TryGet<Ts...>(entity);
        }
        
        template<typename... Ts>
        void Clear() const
        {
            (ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<Ts>()), true, "Write<> of the cleared component"), ...);
            Registry.ClearComponent<Ts...>();
        }

        template<typename... Ts>
        NODISCARD bool HasAnyOf(ECS::FEntity EntityID) const
        {
            return Registry.HasAny<Ts...>(EntityID);
        }

        template<typename ... Ts>
        NODISCARD bool HasAllOf(ECS::FEntity EntityID) const
        {
            return Registry.HasAll<Ts...>(EntityID);
        }

        template<typename T, typename ... TArgs>
        T& Emplace(ECS::FEntity entity, TArgs&& ... Args)
        {
            ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<T>()), true, "Write<> of the emplaced component");
            return Registry.Emplace<T>(entity, std::forward<TArgs>(Args)...);
        }

        template<typename T, typename ... TArgs>
        T& EmplaceOrReplace(ECS::FEntity entity, TArgs&& ... Args) const
        {
            ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<T>()), true, "Write<> of the emplaced component");
            return Registry.EmplaceOrReplace<T>(entity, std::forward<TArgs>(Args)...);
        }
        
        template<typename T>
        decltype(auto) GetStorage() const
        {
            return Registry.GetStorage<T>();
        }
        
        /** The world's physics scene, or null when the world has none. */
        RUNTIME_API Physics::IPhysicsScene* GetPhysicsScene() const;

        RUNTIME_API void ActivateBody(uint32 BodyID);
        RUNTIME_API void DeactivateBody(uint32 BodyID);
        RUNTIME_API void ChangeBodyMotionType(uint32 BodyID, EBodyType NewType);

        /** Physics body id for an entity, or ~0u when it has no body / no physics scene. */
        RUNTIME_API uint32 GetEntityBodyID(ECS::FEntity Entity) const;

        /** Live physics-body pose (NOT the lagged STransformComponent) and velocity, for physics-stage systems. */
        RUNTIME_API FVector3 GetBodyPosition(ECS::FEntity Entity) const;
        RUNTIME_API FQuat    GetBodyRotation(ECS::FEntity Entity) const;
        RUNTIME_API FVector3 GetVelocityAtPoint(ECS::FEntity Entity, const FVector3& Point) const;

        /** Apply a world-space force at a world-space point (adds torque too). Safe from PrePhysics. */
        RUNTIME_API void AddForceAtPosition(ECS::FEntity Entity, const FVector3& Force, const FVector3& Position) const;

        /** Shape-accurate buoyancy for one frame: pass the fluid surface point + normal (e.g. sampled wave
            surface). Buoyancy 1 = neutral density, >1 floats. Submersion is derived from the body bounds. */
        RUNTIME_API void ApplyBuoyancyImpulse(ECS::FEntity Entity, const FVector3& SurfacePosition, const FVector3& SurfaceNormal,
            float Buoyancy, float LinearDrag, float AngularDrag, const FVector3& FluidVelocity, float InDeltaTime) const;

        /** Sweep hits near-to-far; OutHits is cleared first, so one reused buffer keeps the sweep alloc-free. */
        RUNTIME_API void CastSphere(const SSphereCastSettings& Settings, TVector<SRayResult>& OutHits) const;

        /** Nearest sweep hit only; cheaper than CastSphere because the backend stops at the blocking hit. */
        RUNTIME_API TOptional<SRayResult> CastSphereClosest(const SSphereCastSettings& Settings) const;

        RUNTIME_API STransformComponent& GetEntityTransform(ECS::FEntity Entity) const;
        
        RUNTIME_API FVector3 TranslateEntity(ECS::FEntity Entity, const FVector3& Translation);
        RUNTIME_API void SetEntityLocation(ECS::FEntity Entity, const FVector3& Location);
        RUNTIME_API void SetEntityRotation(ECS::FEntity Entity, const FQuat& Rotation);
        RUNTIME_API void SetEntityScale(ECS::FEntity Entity, const FVector3& Scale);
        
        //~ Begin Debug Drawing
        RUNTIME_API void DrawDebugLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness = 1.0f, float Duration = 1.0f) const;
        RUNTIME_API void DrawDebugBox(const FVector3& Center, const FVector3& Extents, const FQuat& Rotation, const FVector4& Color, float Thickness = 1.0f, float Duration = 1.0f) const;
        RUNTIME_API void DrawDebugSphere(const FVector3& Center, float Radius, const FVector4& Color, uint8 Segments = 16, float Thickness = 1.0f, float Duration = 1.0f) const;
        RUNTIME_API void DrawDebugCone(const FVector3& Apex, const FVector3& Direction, float AngleRadians, float Length, const FVector4& Color, uint8 Segments = 16, uint8 Stacks = 4, float Thickness = 1.0f, float Duration = 1.0f) const;
        RUNTIME_API void DrawFrustum(const FMatrix4& Matrix, float zNear, float zFar, const FVector4& Color, float Thickness = 1.0f, float Duration = 1.0f) const;
        RUNTIME_API void DrawDebugArrow(const FVector3& Start, const FVector3& Direction, float Length, const FVector4& Color, float Thickness = 1.0f, float Duration = 1.0f, float HeadSize = 0.2f) const;
        RUNTIME_API void DrawDebugSolidTriangles(TVector<FSimpleElementVertex>&& Vertices, ESolidDrawMode Mode = ESolidDrawMode::Translucent, float Duration = 1.0f) const;
        //~ End Debug Drawing
        
        RUNTIME_API ECS::FEntity Create(const FTransform& Transform, FName EntityName = "Entity") const;
        RUNTIME_API ECS::FEntity Create(FVector3 Location, FName EntityName = "Entity") const;
        RUNTIME_API ECS::FEntity Create(FName EntityName = "Entity") const;
        void Destroy(ECS::FEntity Entity) const
        {
            ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<SystemResource::EntityStructure>()), true, "Write<SystemResource::EntityStructure>");
            Registry.Destroy(Entity);
        }

        RUNTIME_API size_t GetNumEntities() const;
        RUNTIME_API bool IsValidEntity(ECS::FEntity Entity) const;
        
        RUNTIME_API EWorldType GetWorldType() const;
    
    private:

    private:

        double                  DeltaTime = 0.0;
        double                  Time = 0.0;
        CWorld*                 World = nullptr;
        ECS::FRegistry&         Registry;
        ECS::FEventDispatcher&       Dispatcher;
        EUpdateStage            UpdateStage = EUpdateStage::FrameStart;
    };
    
    
}
