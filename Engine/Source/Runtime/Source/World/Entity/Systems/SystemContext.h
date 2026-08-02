#pragma once
#include "Core/UpdateStage.h"
#include "Physics/PhysicsTypes.h"
#include "Physics/Ray/RayCast.h"
#include "SystemAccess.h"
#include "SystemResources.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/TransformComponent.h"


namespace Lumina
{
    enum class EWorldType : uint8;
    enum class EMoveMode : uint8;
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
        RUNTIME_API FORCEINLINE double GetTime() const { return Time; }
        RUNTIME_API FORCEINLINE EUpdateStage GetUpdateStage() const { return UpdateStage; }
        
        RUNTIME_API void SetEntityLifetime(entt::entity Entity, float Lifetime) const;


        template<typename T>
        NODISCARD auto EventSink() const
        {
            return Dispatcher.sink<T>();
        }

        template<typename T, typename ... TArgs>
        void DispatchEvent(TArgs&&... Args) const
        {
            Dispatcher.trigger<T>(Forward<TArgs>(Args)...);
        }
        
        template<typename... Ts, typename... TArgs>
        NODISCARD auto CreateView(TArgs&&... Args) const -> decltype(std::declval<entt::registry>().view<Ts...>(std::forward<TArgs>(Args)...))
        {
            return Registry.view<Ts...>(std::forward<TArgs>(Args)...);
        }
        
        template<typename... Ts, typename TFunc, typename... TArgs>
        void ForEach(TFunc&& Function, TArgs&&... Args)
        {
            auto View = Registry.view<Ts...>(std::forward<TArgs>(Args)...);
            View.each(Forward<TFunc>(Function));
        }

        template<typename... Ts, typename TFunc, typename... TArgs>
        void ParallelForEach(TFunc&& Function, TArgs&&... Args)
        {
            auto View = Registry.view<Ts...>(eastl::forward<TArgs>(Args)...);
            auto Entities = View.handle();
    
            Task::ParallelFor(Entities.size(), [&](uint32 Index)
            {
                entt::entity EntityID = (*Entities)[Index];
                
                if (View.contains(EntityID))
                {
                    eastl::apply(Function, View.get(EntityID));
                }
            });
        }

        NODISCARD auto GetRegistryContext() const
        {
            return Registry.ctx();
        }

        NODISCARD entt::registry& GetRegistry() const
        {
            return Registry;
        }
        
        template<typename... Ts, typename ... TArgs>
        NODISCARD auto CreateGroup(TArgs&&... Args) const
        {
            return Registry.group<Ts...>(std::forward<TArgs>(Args)...);
        }

        template<typename... Ts>
        NODISCARD decltype(auto) Get(entt::entity entity) const
        {
            return Registry.get<Ts...>(entity);
        }

        template<typename... Ts>
        NODISCARD decltype(auto) TryGet(entt::entity entity) const
        {
            return Registry.try_get<Ts...>(entity);
        }
        
        template<typename... Ts>
        void Clear() const
        {
            (ValidateSystemAccess(static_cast<uint32>(entt::type_hash<Ts>::value()), true, "Write<> of the cleared component"), ...);
            Registry.clear<Ts...>();
        }

        template<typename... Ts>
        NODISCARD bool HasAnyOf(entt::entity EntityID) const
        {
            return Registry.any_of<Ts...>(EntityID);
        }

        template<typename ... Ts>
        NODISCARD bool HasAllOf(entt::entity EntityID) const
        {
            return Registry.all_of<Ts...>(EntityID);
        }

        template<typename T, typename ... TArgs>
        T& Emplace(entt::entity entity, TArgs&& ... Args)
        {
            ValidateSystemAccess(static_cast<uint32>(entt::type_hash<T>::value()), true, "Write<> of the emplaced component");
            return Registry.emplace<T>(entity, std::forward<TArgs>(Args)...);
        }

        template<typename T, typename ... TArgs>
        T& EmplaceOrReplace(entt::entity entity, TArgs&& ... Args) const
        {
            ValidateSystemAccess(static_cast<uint32>(entt::type_hash<T>::value()), true, "Write<> of the emplaced component");
            return Registry.emplace_or_replace<T>(entity, std::forward<TArgs>(Args)...);
        }
        
        template<typename T>
        decltype(auto) GetStorage() const
        {
            return Registry.storage<T>();
        }
        
        /** The world's physics scene, or null when the world has none. */
        RUNTIME_API Physics::IPhysicsScene* GetPhysicsScene() const;

        RUNTIME_API void ActivateBody(uint32 BodyID);
        RUNTIME_API void DeactivateBody(uint32 BodyID);
        RUNTIME_API void ChangeBodyMotionType(uint32 BodyID, EBodyType NewType);

        /** Physics body id for an entity, or ~0u when it has no body / no physics scene. */
        RUNTIME_API uint32 GetEntityBodyID(entt::entity Entity) const;

        /** Live physics-body pose (NOT the lagged STransformComponent) and velocity, for physics-stage systems. */
        RUNTIME_API FVector3 GetBodyPosition(entt::entity Entity) const;
        RUNTIME_API FQuat    GetBodyRotation(entt::entity Entity) const;
        RUNTIME_API FVector3 GetVelocityAtPoint(entt::entity Entity, const FVector3& Point) const;

        /** Apply a world-space force at a world-space point (adds torque too). Safe from PrePhysics. */
        RUNTIME_API void AddForceAtPosition(entt::entity Entity, const FVector3& Force, const FVector3& Position) const;

        /** Shape-accurate buoyancy for one frame: pass the fluid surface point + normal (e.g. sampled wave
            surface). Buoyancy 1 = neutral density, >1 floats. Jolt derives submersion from the body shape. */
        RUNTIME_API void ApplyBuoyancyImpulse(entt::entity Entity, const FVector3& SurfacePosition, const FVector3& SurfaceNormal,
            float Buoyancy, float LinearDrag, float AngularDrag, const FVector3& FluidVelocity, float InDeltaTime) const;

        RUNTIME_API TVector<SRayResult> CastSphere(const SSphereCastSettings& Settings) const;

        RUNTIME_API STransformComponent& GetEntityTransform(entt::entity Entity) const;
        
        RUNTIME_API FVector3 TranslateEntity(entt::entity Entity, const FVector3& Translation);
        RUNTIME_API void SetEntityLocation(entt::entity Entity, const FVector3& Location);
        RUNTIME_API void SetEntityRotation(entt::entity Entity, const FQuat& Rotation);
        RUNTIME_API void SetEntityScale(entt::entity Entity, const FVector3& Scale);
        
        //~ Begin Debug Drawing
        RUNTIME_API void DrawDebugLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness = 1.0f, float Duration = 1.0f) const;
        RUNTIME_API void DrawDebugBox(const FVector3& Center, const FVector3& Extents, const FQuat& Rotation, const FVector4& Color, float Thickness = 1.0f, float Duration = 1.0f) const;
        RUNTIME_API void DrawDebugSphere(const FVector3& Center, float Radius, const FVector4& Color, uint8 Segments = 16, float Thickness = 1.0f, float Duration = 1.0f) const;
        RUNTIME_API void DrawDebugCone(const FVector3& Apex, const FVector3& Direction, float AngleRadians, float Length, const FVector4& Color, uint8 Segments = 16, uint8 Stacks = 4, float Thickness = 1.0f, float Duration = 1.0f) const;
        RUNTIME_API void DrawFrustum(const FMatrix4& Matrix, float zNear, float zFar, const FVector4& Color, float Thickness = 1.0f, float Duration = 1.0f) const;
        RUNTIME_API void DrawDebugArrow(const FVector3& Start, const FVector3& Direction, float Length, const FVector4& Color, float Thickness = 1.0f, float Duration = 1.0f, float HeadSize = 0.2f) const;
        RUNTIME_API void DrawDebugSolidTriangles(TVector<FSimpleElementVertex>&& Vertices, bool bDepthTest = true, float Duration = 1.0f) const;
        //~ End Debug Drawing
        
        RUNTIME_API entt::runtime_view CreateRuntimeView(const THashSet<entt::id_type>& Components) const;

        RUNTIME_API entt::entity Create(FVector3 Location) const;
        RUNTIME_API entt::entity Create() const;
        void Destroy(entt::entity Entity) const
        {
            ValidateSystemAccess(static_cast<uint32>(entt::type_hash<SystemResource::EntityStructure>::value()), true, "Write<SystemResource::EntityStructure>");
            Registry.destroy(Entity);
        }

        RUNTIME_API size_t GetNumEntities() const;
        RUNTIME_API bool IsValidEntity(entt::entity Entity) const;
        
        RUNTIME_API EWorldType GetWorldType() const;
    
    private:

    private:

        double                  DeltaTime = 0.0;
        double                  Time = 0.0;
        CWorld*                 World = nullptr;
        entt::registry&         Registry;
        entt::dispatcher&       Dispatcher;
        EUpdateStage            UpdateStage = EUpdateStage::FrameStart;
    };
    
    
}
