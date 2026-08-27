#include "EditorPCH.h"
#include "World/ECS/Registry.h"
#include "Scene/SceneOps.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Containers/String.h"
#include "Core/Object/Class.h"
#include "Core/Object/Class/StructTraits.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/EntityUtils.h"

namespace Lumina::SceneOps
{
    CStruct* ResolveComponentType(FStringView TypeName)
    {
        return TypeName.empty() ? nullptr : FindComponentStruct(TypeName);
    }

    FAddComponentPlan PlanAddComponent(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Targets, CStruct* Type)
    {
        FAddComponentPlan Plan;

        if (Targets.empty() || Type == nullptr)
        {
            return Plan;
        }

        // The reflected struct is what lets an instance record the add in its override ledger.
        Plan.Component = Type;

        Plan.Missing.reserve(Targets.size());

        for (ECS::FEntity Target : Targets)
        {
            if (ECS::Utils::HasComponent(Registry, Target, Type))
            {
                ++Plan.AlreadyPresent;
            }
            else
            {
                Plan.Missing.push_back(Target);
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

        // A raw store leaves none of the flags the transform setters would have raised.
        if (Event.OuterType == STransformComponent::StaticStruct())
        {
            Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
        }

        CPrefab::RecaptureComponentOverrides(Registry, Entity, Event.OuterType);
    }
}
