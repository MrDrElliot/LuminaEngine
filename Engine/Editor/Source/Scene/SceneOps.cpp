#include "EditorPCH.h"
#include "World/ECS/Registry.h"
#include "Scene/SceneOps.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Containers/String.h"
#include "Core/Object/Class.h"
#include "Core/Object/Class/StructTraits.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Settings/EditorSettings.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/EntityUtils.h"

namespace Lumina::SceneOps
{
    CStruct* ResolveComponentType(FStringView TypeName)
    {
        return TypeName.IsEmpty() ? nullptr : FindComponentStruct(TypeName);
    }

    FAddComponentPlan PlanAddComponent(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Targets, CStruct* Type)
    {
        FAddComponentPlan Plan;

        if (Targets.IsEmpty() || Type == nullptr)
        {
            return Plan;
        }

        // The reflected struct is what lets an instance record the add in its override ledger.
        Plan.Component = Type;

        Plan.Missing.Reserve(Targets.Num());

        for (ECS::FEntity Target : Targets)
        {
            if (ECS::Utils::HasComponent(Registry, Target, Type))
            {
                ++Plan.AlreadyPresent;
            }
            else
            {
                Plan.Missing.Add(Target);
            }
        }

        return Plan;
    }

    void ApplyAddComponent(ECS::FRegistry& Registry, const FAddComponentPlan& Plan, CStruct* Type)
    {
        const FComponentOps* Ops = Type != nullptr ? Type->GetComponentOps() : nullptr;
        if (Ops == nullptr)
        {
            return;
        }

        for (ECS::FEntity Target : Plan.Missing)
        {
            Ops->EmplaceDefault(Registry, Target);

            if (Plan.Component != nullptr)
            {
                CPrefab::NoteComponentAdded(Registry, Target, Plan.Component);
            }
        }
    }
}

namespace Lumina::SceneOps
{
    namespace
    {
        // -1 when the value sits in a separate heap block rather than inside the component itself.
        int64 ValueOffsetInOuter(const void* ValuePtr, const void* OuterInstance, const CStruct* OuterType)
        {
            if (ValuePtr == nullptr || OuterInstance == nullptr || OuterType == nullptr)
            {
                return -1;
            }

            const int64 Offset = static_cast<const uint8*>(ValuePtr) - static_cast<const uint8*>(OuterInstance);
            return (Offset >= 0 && Offset < (int64)OuterType->GetSize()) ? Offset : -1;
        }
    }

    FPropertyEditScope::FPropertyEditScope(ECS::FRegistry& InRegistry, ECS::FEntity InEntity, CStruct* OuterType,
        void* InOuterInstance, FProperty* Property, const void* ValuePtr)
        : Registry(InRegistry)
        , Entity(InEntity)
        , OuterInstance(InOuterInstance)
        , Event{OuterType, Property, Property != nullptr ? Property->Name : FName(), true,
            ValueOffsetInOuter(ValuePtr, InOuterInstance, OuterType)}
    {
        FStructOps* Ops = OuterType != nullptr ? OuterType->GetStructOps() : nullptr;

        if (Ops != nullptr && Ops->HasPreEdit() && OuterInstance != nullptr)
        {
            Ops->PreEdit(OuterInstance, Event);
        }
    }

    FPropertyEditScope::~FPropertyEditScope()
    {
        if (Event.OuterType == nullptr || OuterInstance == nullptr)
        {
            return;
        }

        FStructOps* Ops = Event.OuterType->GetStructOps();

        if (Ops != nullptr && Ops->HasPostEdit())
        {
            Ops->PostEdit(OuterInstance, Event);
        }

        if (!Registry.IsValid(Entity))
        {
            return;
        }

        // A raw store stamps nothing by itself, and the edit stays silent like the details panel's live ops.
        if (const FComponentOps* ComponentOps = Event.OuterType->GetComponentOps())
        {
            Registry.MarkChangedRaw(ComponentOps->GetTypeID(), Entity);
        }

        // A raw store leaves none of the flags the transform setters would have raised.
        if (Event.OuterType == STransformComponent::StaticStruct())
        {
            Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
        }

        CPrefab::RecaptureComponentOverrides(Registry, Entity, Event.OuterType);
    }

    bool HasCollider(const ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        return Registry.HasAny<SBoxColliderComponent, SSphereColliderComponent, SCapsuleColliderComponent, SCylinderColliderComponent,
                               STaperedCapsuleColliderComponent, STaperedCylinderColliderComponent, SPlaneColliderComponent,
                               SDynamicMeshColliderComponent, STerrainColliderComponent, SCompoundColliderComponent,
                               SMeshColliderComponent>(Entity);
    }

    bool CanAddMeshCollision(const ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (!Registry.IsValid(Entity) || HasCollider(Registry, Entity))
        {
            return false;
        }
        const SStaticMeshComponent* MeshComponent = Registry.TryGet<SStaticMeshComponent>(Entity);
        return MeshComponent != nullptr && MeshComponent->StaticMesh.Get() != nullptr;
    }

    bool AddMeshCollision(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (!CanAddMeshCollision(Registry, Entity))
        {
            return false;
        }

        // Before the collider, whose construction would otherwise add a body of the default, dynamic, type.
        if (!Registry.HasAll<SRigidBodyComponent>(Entity))
        {
            SRigidBodyComponent Body;
            Body.BodyType = EBodyType::Static;
            Registry.Emplace<SRigidBodyComponent>(Entity, Body);
        }

        Registry.Emplace<SMeshColliderComponent>(Entity);
        return true;
    }

    bool AddPlacementCollision(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const CPlacementSettings* Settings = GetDefault<CPlacementSettings>();
        return Settings != nullptr && Settings->bAddCollisionToPlacedMeshes && AddMeshCollision(Registry, Entity);
    }
}
