#include "RuntimePCH.h"
#include "SystemContext.h"
#include "World/ECS/Registry.h"
#include "World/World.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/NameComponent.h"

namespace Lumina
{
    namespace SystemContextInternal
    {
        // A parallel system's spawn reserves the handle now and builds the entity at the flush, so it never resizes a pool another system reads.
        template<typename TSetTransform>
        ECS::FEntity CreateEntity(ECS::FRegistry& Registry, ECS::FCommandBus& Commands, FName EntityName, TSetTransform SetTransform)
        {
            auto Build = [EntityName, SetTransform](ECS::FRegistry& InRegistry, ECS::FEntity Entity)
            {
                SetTransform(InRegistry.EmplaceOrReplace<STransformComponent>(Entity));
                InRegistry.EmplaceOrReplace<SNameComponent>(Entity, EntityName);
                InRegistry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
            };

            if (ECS::FCommandBus::ShouldDefer())
            {
                const ECS::FEntity Entity = Commands.Create();
                Commands.Enqueue([Entity, Build](ECS::FRegistry& InRegistry)
                {
                    if (InRegistry.IsValid(Entity))
                    {
                        Build(InRegistry, Entity);
                    }
                });
                return Entity;
            }

            const ECS::FEntity Entity = Registry.Create();
            Build(Registry, Entity);
            return Entity;
        }

        // Owns a one-shot C# callback until it runs, so a discarded command still frees its handle.
        struct FDeferredScriptCallback
        {
            SScriptCallback Callback;

            explicit FDeferredScriptCallback(SScriptCallback InCallback) : Callback(InCallback) {}
            FDeferredScriptCallback(FDeferredScriptCallback&& Other) noexcept : Callback(Other.Callback) { Other.Callback.Token = 0; }
            ~FDeferredScriptCallback() { Scripting::ReleaseScriptCallback(Callback); }

            FDeferredScriptCallback(const FDeferredScriptCallback&) = delete;
            FDeferredScriptCallback& operator=(const FDeferredScriptCallback&) = delete;
            FDeferredScriptCallback& operator=(FDeferredScriptCallback&&) = delete;

            void Invoke(uint64 Payload)
            {
                const SScriptCallback Once = Callback;
                Callback.Token = 0;
                Scripting::InvokeScriptCallback(Once, Payload);
            }
        };
    }

    SSystemContext::SSystemContext(CWorld* InWorld)
        : World(InWorld)
        , Registry(&InWorld->EntityRegistry)
        , Events(&InWorld->EventQueues)
        , CommandBus(&InWorld->CommandBus)
    {}


    const ECS::FHierarchy& SSystemContext::GetHierarchy() const
    {
        return Registry->GetHierarchy();
    }

    void SSystemContext::SetEntityLifetime(ECS::FEntity Entity, float Lifetime) const
    {
        if (ECS::FCommandBus::ShouldDefer())
        {
            CommandBus->Enqueue([Entity, Lifetime](ECS::FRegistry& InRegistry)
            {
                if (InRegistry.IsValid(Entity))
                {
                    InRegistry.GetOrEmplace<SLifetimeComponent>(Entity).Lifetime = Lifetime;
                }
            });
            return;
        }
        Registry->GetOrEmplace<SLifetimeComponent>(Entity).Lifetime = Lifetime;
    }

    // Read, not write, since a declared Write satisfies it and only a total absence is caught.
    Physics::IPhysicsQueries* SSystemContext::GetPhysics() const
    {
        return World ? World->GetPhysicsScene() : nullptr;
    }

    Physics::IPhysicsScene* SExclusiveContext::GetPhysicsScene() const
    {
        return World ? World->GetPhysicsScene() : nullptr;
    }

    void SSystemContext::DrawDebugLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness, float Duration) const
    {
        World->GetDebugInterface()->DrawLine(Start, End, Color, Thickness, true, Duration);
    }

    void SSystemContext::DrawDebugBox(const FVector3& Center, const FVector3& Extents, const FQuat& Rotation, const FVector4& Color, float Thickness, float Duration) const
    {
        World->GetDebugInterface()->DrawBox(Center, Extents, Rotation, Color, Thickness, true, Duration);
    }

    void SSystemContext::DrawDebugSphere(const FVector3& Center, float Radius, const FVector4& Color, uint8 Segments, float Thickness, float Duration) const
    {
        World->GetDebugInterface()->DrawSphere(Center, Radius, Color, Segments, Thickness, true, Duration);
    }

    void SSystemContext::DrawDebugCone(const FVector3& Apex, const FVector3& Direction, float AngleRadians, float Length, const FVector4& Color, uint8 Segments, uint8 Stacks, float Thickness, float Duration) const
    {
        World->GetDebugInterface()->DrawCone(Apex, Direction, AngleRadians, Length, Color, Segments, Stacks, Thickness, true, Duration);
    }

    void SSystemContext::DrawFrustum(const FMatrix4& Matrix, float zNear, float zFar, const FVector4& Color, float Thickness, float Duration) const
    {
        World->GetDebugInterface()->DrawFrustum(Matrix, zNear, zFar, Color, Thickness, true, Duration);
    }

    void SSystemContext::DrawDebugArrow(const FVector3& Start, const FVector3& Direction, float Length, const FVector4& Color, float Thickness, float Duration, float HeadSize) const
    {
        World->GetDebugInterface()->DrawArrow(Start, Direction, Length, Color, Thickness, true, Duration, HeadSize);
    }

    void SSystemContext::DrawDebugSolidTriangles(TVector<FSimpleElementVertex>&& Vertices, ESolidDrawMode Mode, float Duration) const
    {
        World->GetDebugInterface()->DrawSolidTriangles(Move(Vertices), Mode, Duration);
    }

    ECS::FEntity SSystemContext::Create(const FTransform& Transform, FName EntityName) const
    {
        LUMINA_PROFILE_SCOPE();
        return SystemContextInternal::CreateEntity(*Registry, *CommandBus, EntityName, [Transform](STransformComponent& Component) { Component.SetLocalTransform(Transform); });
    }

    ECS::FEntity SSystemContext::Create(FVector3 Location, FName EntityName) const
    {
        LUMINA_PROFILE_SCOPE();
        return SystemContextInternal::CreateEntity(*Registry, *CommandBus, EntityName, [Location](STransformComponent& Component) { Component.SetLocation(Location); });
    }

    ECS::FEntity SSystemContext::Create(FName EntityName) const
    {
        LUMINA_PROFILE_SCOPE();
        return SystemContextInternal::CreateEntity(*Registry, *CommandBus, EntityName, [](STransformComponent&) {});
    }

    bool SSystemContext::IsValidEntity(ECS::FEntity Entity) const
    {
        return Registry->IsValid(Entity);
    }

    EWorldType SSystemContext::GetWorldType() const
    {
        return World->GetWorldType();
    }

    ECS::FEntity SSystemContext::ScriptCreate(const FTransform& Transform, FName EntityName) const
    {
        return Create(Transform, EntityName);
    }

    void* SSystemContext::ScriptEmplace(ECS::FEntity Entity, void* Ops) const
    {
        const FComponentOps* Component = static_cast<const FComponentOps*>(Ops);
        if (Component == nullptr)
        {
            return nullptr;
        }

        if (ECS::FCommandBus::ShouldDefer())
        {
            return Component->StageEmplace(*CommandBus, Entity);
        }

        void* Value = Component->EmplaceDefault(*Registry, Entity);
        if (ECS::FComponentTypeRegistry::Get().GetInfo(Component->GetTypeID()).bEmpty)
        {
            return nullptr;
        }
        Registry->AssureStorage(Component->GetTypeID()).MarkPointersEscaped();
        return Value;
    }

    void SSystemContext::ScriptEmplaceCopy(ECS::FEntity Entity, void* Ops, void* Source) const
    {
        const FComponentOps* Component = static_cast<const FComponentOps*>(Ops);
        if (Component == nullptr || Source == nullptr)
        {
            return;
        }

        const bool bDefer = ECS::FCommandBus::ShouldDefer();
        void* Value = bDefer ? Component->StageEmplace(*CommandBus, Entity) : Component->New();
        const ECS::FComponentTypeInfo& Info = ECS::FComponentTypeRegistry::Get().GetInfo(Component->GetTypeID());
        if (CStruct* Struct = Info.GetBoundStruct(); Struct != nullptr && !Info.bEmpty)
        {
            Struct->CopyStruct(Value, Source);
        }

        if (!bDefer)
        {
            Component->EmplaceCopy(*Registry, Entity, Value);
            Component->Delete(Value);
        }
    }

    void SSystemContext::ScriptRemove(ECS::FEntity Entity, void* Ops) const
    {
        const FComponentOps* Component = static_cast<const FComponentOps*>(Ops);
        if (Component == nullptr)
        {
            return;
        }

        if (ECS::FCommandBus::ShouldDefer())
        {
            CommandBus->Enqueue([Entity, Component](ECS::FRegistry& InRegistry)
            {
                if (InRegistry.IsValid(Entity))
                {
                    Component->Remove(InRegistry, Entity);
                }
            });
            return;
        }
        Component->Remove(*Registry, Entity);
    }

    void SSystemContext::ScriptDefer(SScriptCallback Callback) const
    {
        Defer([Owned = SystemContextInternal::FDeferredScriptCallback(Callback)](const SExclusiveContext& Exclusive) mutable
        {
            Owned.Invoke(reinterpret_cast<uint64>(&Exclusive));
        });
    }

    bool SSystemContext::CanReachWorld()
    {
        return !ECS::FCommandBus::ShouldDefer();
    }
}
