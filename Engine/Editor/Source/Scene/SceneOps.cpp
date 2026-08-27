#include "EditorPCH.h"
#include "Scene/SceneOps.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Containers/String.h"
#include "Core/Engine/EngineMetaContext.h"
#include "Core/Object/Class.h"
#include "Core/Object/Class/StructTraits.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/EntityUtils.h"

namespace Lumina::SceneOps
{
    entt::meta_type ResolveComponentType(FStringView TypeName)
    {
        if (TypeName.empty())
        {
            return entt::meta_type();
        }

        // hashed_string reads to a terminator, so a view has to be copied before it can be hashed.
        const FString Name(TypeName.data(), TypeName.size());
        return entt::resolve(GetEngineMetaContext(), entt::hashed_string(Name.c_str()));
    }

    FAddComponentPlan PlanAddComponent(FEntityRegistry& Registry, const TVector<FEntity>& Targets, entt::meta_type Type)
    {
        using namespace entt::literals;

        FAddComponentPlan Plan;

        if (Targets.empty())
        {
            return Plan;
        }

        // The reflected struct is what lets an instance record the add in its override ledger.
        if (entt::meta_any Resolved = ECS::Utils::InvokeMetaFunc(Type, "static_struct"_hs))
        {
            Plan.Component = Resolved.cast<CStruct*>();
        }

        Plan.Missing.reserve(Targets.size());

        for (FEntity Target : Targets)
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

    void ApplyAddComponent(FEntityRegistry& Registry, const FAddComponentPlan& Plan, entt::meta_type Type)
    {
        using namespace entt::literals;

        if (!Type)
        {
            return;
        }

        for (FEntity Target : Plan.Missing)
        {
            // The reflected emplace replaces data components, so only targets known to lack it come here.
            ECS::Utils::InvokeMetaFunc(Type, "emplace"_hs, entt::forward_as_meta(Registry), Target,
                entt::forward_as_meta(entt::meta_any{}));

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

    FPropertyEditScope::FPropertyEditScope(FEntityRegistry& InRegistry, FEntity InEntity, CStruct* OuterType,
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

        if (!Registry.valid(Entity))
        {
            return;
        }

        // A raw store leaves none of the flags the transform setters would have raised.
        if (Event.OuterType == STransformComponent::StaticStruct())
        {
            Registry.emplace_or_replace<FNeedsTransformUpdate>(Entity);
        }

        CPrefab::RecaptureComponentOverrides(Registry, Entity, Event.OuterType);
    }
}
