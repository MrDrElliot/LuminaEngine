#include "MCPSceneTools.h"
#include "World/ECS/Registry.h"

#include "Agent/AgentEntityToken.h"
#include "Agent/AgentPropertyPath.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "MCPTextMatch.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/ObjectCore.h"
#include "Scripting/EntityScript.h"
#include "Session/SessionOps.h"
#include "UI/Tools/EditorEntityUtils.h"
#include "Scene/SceneOps.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TagComponent.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "World/World.h"

namespace Lumina::MCP
{
    namespace
    {
        constexpr const char* GNoWorldEditorScene = "No world editor is open, so there is no scene to work on.";

        FString NameOf(const ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            const SNameComponent* Name = Registry.TryGet<SNameComponent>(Entity);
            return Name != nullptr ? FString(Name->Name.ToString().c_str()) : FString("Entity");
        }

        // Values come back as text because a free-form component tree has no fixed reflected shape.
        FString DescribeComponentValues(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {

            nlohmann::json Components = nlohmann::json::object();

            ForEachComponentStruct([&](CStruct* Reflected)
            {
                if (!ECS::Utils::HasComponent(Registry, Entity, Reflected))
                {
                    return;
                }

                const std::string Key(Reflected->GetName().ToString().c_str());

                // The direct op table hands back the live pointer, which the meta trampoline will not.
                const FComponentOps* Ops = FindComponentOps(FStringView(Reflected->GetName().ToString()));
                void* Data = Ops != nullptr && Ops->Get != nullptr ? Ops->Get(Registry, Entity) : nullptr;

                if (Data == nullptr)
                {
                    Components[Key] = nlohmann::json::object();
                    return;
                }

                nlohmann::json Written;
                Components[Key] = Agent::WriteStruct(Reflected, Data, Written).IsValid()
                    ? Written
                    : nlohmann::json::object();
            });

            // Scripts are subobjects the component only points at, so their values are listed under their own class.
            if (const SEntityScriptComponent* Scripts = Registry.TryGet<SEntityScriptComponent>(Entity))
            {
                for (const TStrongObjectPtr<CEntityScript>& Held : Scripts->Scripts)
                {
                    CEntityScript* Script = Held.Get();
                    if (Script == nullptr || Script->GetClass() == nullptr)
                    {
                        continue;
                    }

                    nlohmann::json Written;
                    Components[std::string(Script->GetClass()->GetName().ToString().c_str())] =
                        Agent::WriteStruct(Script->GetClass(), Script, Written).IsValid() ? Written : nlohmann::json::object();
                }
            }

            return FString(Components.dump(2).c_str());
        }

        void RegisterListComponentTypes(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListComponentTypesParams, SListComponentTypesResult>(
                Owner, "scene.list_component_types",
                "List every component type that can be attached to an entity.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListComponentTypesParams& In, SListComponentTypesResult& Out)
                {

                    ForEachComponentStruct([&](CStruct* Reflected)
                    {
                        // The editor hides these from its own picker, so an agent should not see them either.
                        if (Reflected->HasMeta("HideInComponentList"))
                        {
                            return;
                        }

                        const FString Name(Reflected->GetName().ToString().c_str());
                        if (!ContainsText(FStringView(Name), In.Contains))
                        {
                            return;
                        }

                        SComponentTypeInfo Info;
                        Info.Name        = Name;
                        Info.DisplayName = FString(Reflected->MakeDisplayName().c_str());
                        Info.Category    = Reflected->HasMeta("Category") ? Reflected->GetMeta("Category") : FString("General");

                        Out.Types.push_back(Move(Info));
                    });

                    return Agent::FToolResult::Ok(Lumina::Format("{} component type(s).", Out.Types.size()));
                });
        }

        void RegisterListEntities(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListEntitiesParams, SListEntitiesResult>(
                Owner, "scene.list_entities",
                "List entities in the open world, with the id every other tool takes.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListEntitiesParams& In, SListEntitiesResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    ECS::FRegistry& Registry = *ScenePtr;
                    const int32 Limit = In.Limit > 0 ? In.Limit : 100;
                    const int32 Offset = In.Offset > 0 ? In.Offset : 0;

                    for (auto Entity : Registry.View<SNameComponent>())
                    {
                        const FString Name = NameOf(Registry, Entity);
                        if (!ContainsText(FStringView(Name), In.Contains))
                        {
                            continue;
                        }

                        ++Out.Matched;

                        if (Out.Matched <= Offset || static_cast<int32>(Out.Entities.size()) >= Limit)
                        {
                            continue;
                        }

                        SEntityInfo Info;
                        Info.Id   = Agent::FEntityTokens::Mint(Registry, Entity);
                        Info.Name = Name;
                        if (const FRelationshipComponent* Relationship = Registry.TryGet<FRelationshipComponent>(Entity);
                            Relationship != nullptr && Relationship->Parent != ECS::NullEntity && Registry.IsValid(Relationship->Parent))
                        {
                            Info.Parent = Agent::FEntityTokens::Mint(Registry, Relationship->Parent);
                        }
                        if (const SPrefabInstanceComponent* Instance = Registry.TryGet<SPrefabInstanceComponent>(Entity))
                        {
                            Info.Prefab      = Instance->SourcePrefab ? FString(Instance->SourcePrefab->GetName().c_str()) : FString("<missing>");
                            Info.bPrefabRoot = Instance->bIsRoot;
                        }

                        Out.Entities.push_back(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} of {} matching entities.",
                        Out.Entities.size(), Out.Matched));
                });
        }

        void RegisterDescribeEntity(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDescribeEntityParams, SDescribeEntityResult>(
                Owner, "entity.describe",
                "Report an entity's name, its components and their current values.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SDescribeEntityParams& In, SDescribeEntityResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    ECS::FRegistry& Registry = *ScenePtr;

                    ECS::FEntity Entity = ECS::NullEntity;
                    FString Error;
                    if (!Agent::FEntityTokens::Resolve(Registry, FStringView(In.Entity), Entity, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Name = NameOf(Registry, Entity);

                    ForEachComponentStruct([&](CStruct* Reflected)
                    {
                        if (ECS::Utils::HasComponent(Registry, Entity, Reflected))
                        {
                            Out.Components.push_back(FString(Reflected->GetName().ToString().c_str()));
                        }
                    });

                    return Agent::FToolResult::Ok(Lumina::Format("{} has {} component(s).\n{}",
                        Out.Name, Out.Components.size(), DescribeComponentValues(Registry, Entity)));
                });
        }

        void RegisterCreateEntity(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateEntityParams, SCreateEntityResult>(
                Owner, "scene.create_entity",
                "Create an entity in the open world, optionally attaching components to it.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateEntityParams& In, SCreateEntityResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    CWorld* World = SessionOps::GetSceneWorld(SceneError);
                    if (World == nullptr)
                    {
                        return Agent::FToolResult::Error(GNoWorldEditorScene);
                    }

                    // Resolved before the transaction opens, so an unknown name costs no snapshot.
                    TVector<CStruct*> Types;
                    for (const FString& Name : In.Components)
                    {
                        CStruct* Type = SceneOps::ResolveComponentType(FStringView(Name));
                        if (Type != nullptr)
                        {
                            Types.push_back(Type);
                            Out.Attached.push_back(Name);
                        }
                        else
                        {
                            Out.Skipped.push_back(Name);
                        }
                    }

                    ECS::FRegistry& Registry = *ScenePtr;
                    const FName EntityName(In.Name.empty() ? "Entity" : In.Name.c_str());

                    ECS::FEntity Created = ECS::NullEntity;

                    SessionOps::RunCreationTransacted("Create Entity (agent)", [&]()
                    {
                        Created = World->ConstructEntity(EntityName);
                        if (Created == ECS::NullEntity)
                        {
                            return;
                        }

                        for (CStruct* Type : Types)
                        {
                            const SceneOps::FAddComponentPlan Plan =
                                SceneOps::PlanAddComponent(Registry, TVector<ECS::FEntity>{ Created }, Type);

                            SceneOps::ApplyAddComponent(Registry, Plan, Type);
                        }
                    }, SceneError);

                    if (Created == ECS::NullEntity)
                    {
                        return Agent::FToolResult::Error("The world refused to create the entity.");
                    }

                    Out.Entity = Agent::FEntityTokens::Mint(Registry, Created);

                    return Agent::FToolResult::Ok(Out.Skipped.empty()
                        ? Lumina::Format("Created '{}' with {} component(s).", In.Name, Out.Attached.size())
                        : Lumina::Format("Created '{}' with {} component(s). {} name(s) matched no component type.",
                            In.Name, Out.Attached.size(), Out.Skipped.size()));
                });
        }

        void RegisterAddComponent(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddComponentParams, SAddComponentResult>(
                Owner, "entity.add_component",
                "Attach a component to an entity that does not already have it.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddComponentParams& In, SAddComponentResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    ECS::FRegistry& Registry = *ScenePtr;

                    ECS::FEntity Entity = ECS::NullEntity;
                    FString Error;
                    if (!Agent::FEntityTokens::Resolve(Registry, FStringView(In.Entity), Entity, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CStruct* Type = SceneOps::ResolveComponentType(FStringView(In.Component));
                    if (!Type)
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "No component type is named '{}'. Use scene.list_component_types.", In.Component));
                    }

                    const SceneOps::FAddComponentPlan Plan =
                        SceneOps::PlanAddComponent(Registry, TVector<ECS::FEntity>{ Entity }, Type);

                    if (!Plan.HasWork())
                    {
                        Out.bAlreadyPresent = true;
                        return Agent::FToolResult::Ok(Lumina::Format("'{}' already has {}.",
                            NameOf(Registry, Entity), In.Component));
                    }

                    SessionOps::RunTransacted("Add Component (agent)", [&]()
                    {
                        SceneOps::ApplyAddComponent(Registry, Plan, Type);
                    }, SceneError);

                    Out.bAdded = true;

                    return Agent::FToolResult::Ok(Lumina::Format("Added {} to '{}'.",
                        In.Component, NameOf(Registry, Entity)));
                });
        }

        // Entity fields store a raw handle, so the ids every other tool hands out are swapped in for it here.
        bool ResolveEntityTokens(const ECS::FRegistry& Registry, FProperty* Property, nlohmann::json& Value, FString& OutError)
        {
            if (Property->GetType() == EPropertyTypeFlags::Vector && Value.is_array())
            {
                FProperty* Inner = static_cast<FArrayProperty*>(Property)->GetInternalProperty();
                for (nlohmann::json& Element : Value)
                {
                    if (Inner != nullptr && !ResolveEntityTokens(Registry, Inner, Element, OutError))
                    {
                        return false;
                    }
                }
                return true;
            }

            if (!Property->IsA(EPropertyTypeFlags::Entity) || !Value.is_string())
            {
                return true;
            }

            const std::string Token = Value.get<std::string>();
            ECS::FEntity Resolved = ECS::NullEntity;
            if (!Token.empty() && !Agent::FEntityTokens::Resolve(Registry, FStringView(Token.c_str()), Resolved, OutError))
            {
                return false;
            }

            Value = static_cast<uint32>(Resolved);
            return true;
        }

        /** The script attached to Entity whose class matches Name, full or short. Null when none does. */
        CEntityScript* FindEntityScriptByName(ECS::FRegistry& Registry, ECS::FEntity Entity, FStringView Name)
        {
            const SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr || Name.empty())
            {
                return nullptr;
            }

            for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script == nullptr || Script->GetClass() == nullptr)
                {
                    continue;
                }

                const FString Full = Script->GetClass()->GetName().ToString();
                if (EqualsTextFold(FStringView(Full), Name))
                {
                    return Script;
                }

                const size_t Dot = FStringView(Full).find_last_of('.');
                if (Dot != FStringView::npos && EqualsTextFold(FStringView(Full).substr(Dot + 1), Name))
                {
                    return Script;
                }
            }
            return nullptr;
        }

        // Scripts are subobjects, not assets, so entity.set_property cannot put one in SEntityScriptComponent.
        void RegisterAddScript(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddScriptParams, SAddScriptResult>(
                Owner, "entity.add_script",
                "Attach a C# or C++ entity script class to an entity, as the inspector's script picker does.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddScriptParams& In, SAddScriptResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    ECS::FRegistry& Registry = *ScenePtr;

                    ECS::FEntity Entity = ECS::NullEntity;
                    FString Error;
                    if (!Agent::FEntityTokens::Resolve(Registry, FStringView(In.Entity), Entity, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CClass* ScriptClass = FindObject<CClass>(FName(In.ScriptClass));
                    if (ScriptClass == nullptr || !ScriptClass->IsChildOf(CEntityScript::StaticClass()))
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "'{}' is not a loaded CEntityScript class.", In.ScriptClass));
                    }

                    CEntityScript* Script = nullptr;
                    SessionOps::RunTransacted("Add Script (agent)", [&]()
                    {
                        Script = EntityScripts::Attach(Registry, Entity, ScriptClass);
                    }, SceneError);

                    if (Script == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("'{}' could not be attached.", In.ScriptClass));
                    }

                    Out.bAdded = true;
                    return Agent::FToolResult::Ok(Lumina::Format("Attached {} to '{}'.",
                        In.ScriptClass, NameOf(Registry, Entity)));
                });
        }

        void RegisterSetEntityProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetPropertyParams, SSetPropertyResult>(
                Owner, "entity.set_property",
                "Set one field on one component of an entity.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetPropertyParams& In, SSetPropertyResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    ECS::FRegistry& Registry = *ScenePtr;

                    ECS::FEntity Entity = ECS::NullEntity;
                    FString Error;
                    if (!Agent::FEntityTokens::Resolve(Registry, FStringView(In.Entity), Entity, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CStruct* Reflected = nullptr;
                    const FComponentOps* Ops = nullptr;
                    void* Data = nullptr;

                    CStruct* Type = SceneOps::ResolveComponentType(FStringView(In.Component));
                    if (Type != nullptr && ECS::Utils::HasComponent(Registry, Entity, Type))
                    {
                        Reflected = Type;
                        Ops = FindComponentOps(FStringView(Reflected->GetName().ToString()));
                        Data = Ops != nullptr && Ops->Get != nullptr ? Ops->Get(Registry, Entity) : nullptr;
                    }
                    else if (CEntityScript* Script =
                                 FindEntityScriptByName(Registry, Entity, FStringView(In.Component)))
                    {
                        // A script is a CObject whose minted class carries real FPropertys, so the same path
                        // resolve and marshal work over it once the instance stands in for the component data.
                        Reflected = Script->GetClass();
                        Data = Script;
                    }
                    else
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "'{}' has no component or script named '{}'.", NameOf(Registry, Entity), In.Component));
                    }

                    if (Data == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "'{}' carries no data that can be edited.", In.Component));
                    }

                    Agent::FResolvedProperty Target;
                    if (!Agent::ResolvePropertyPath(Reflected, Data, FStringView(In.Path), Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    nlohmann::json Value = nlohmann::json::parse(
                        In.Value.c_str(), In.Value.c_str() + In.Value.size(), nullptr, false);

                    if (Value.is_discarded())
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "Value is not JSON. Strings need quotes, so \"Torch\" rather than Torch."));
                    }

                    if (!ResolveEntityTokens(Registry, Target.Property, Value, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    // Checked first, because opening the transaction snapshots the whole registry.
                    if (const Agent::FMarshalResult Check =
                            Agent::ValidatePropertyValue(Value, Target.Property, FStringView(In.Path));
                        !Check.IsValid())
                    {
                        return Agent::FToolResult::Error(Check.Error);
                    }

                    nlohmann::json Before;
                    Agent::WriteProperty(Target.Property, Target.ValuePtr, Before);
                    Out.Previous = FString(Before.dump().c_str());

                    Agent::FMarshalResult Applied;
                    auto Apply = [&]()
                    {
                        Applied = Agent::ReadProperty(Value, Target.Property, Target.ValuePtr, FStringView(In.Path));
                    };

                    SessionOps::RunTransacted("Set Property (agent)", [&]()
                    {
                        // A script owns its own state, so there is no component record to rebake.
                        if (Ops == nullptr)
                        {
                            Apply();
                            CPrefab::RecaptureComponentOverrides(Registry, Entity, SEntityScriptComponent::StaticStruct());
                            return;
                        }

                        // A bare store reaches no hook, so the renderer keeps serving the old baked record.
                        SceneOps::FPropertyEditScope Edit(Registry, Entity, Reflected, Data,
                            Target.Property, Target.ValuePtr);
                        Apply();
                    }, SceneError);

                    if (!Applied.IsValid())
                    {
                        return Agent::FToolResult::Error(Applied.Error);
                    }

                    nlohmann::json After;
                    Agent::WriteProperty(Target.Property, Target.ValuePtr, After);
                    Out.Current = FString(After.dump().c_str());

                    return Agent::FToolResult::Ok(Lumina::Format("{}.{} is now {}.",
                        In.Component, In.Path, Out.Current));
                });
        }

        void RegisterRemoveComponent(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SRemoveComponentParams, SRemoveComponentResult>(
                Owner, "entity.remove_component",
                "Detach a component from an entity, as one undo step.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SRemoveComponentParams& In, SRemoveComponentResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    if (SessionOps::IsSimulating())
                    {
                        return Agent::FToolResult::Error("Stop play-in-editor first.");
                    }

                    ECS::FRegistry& Registry = *ScenePtr;

                    ECS::FEntity Entity = ECS::NullEntity;
                    FString Error;
                    if (!Agent::FEntityTokens::Resolve(Registry, FStringView(In.Entity), Entity, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CStruct* Type = SceneOps::ResolveComponentType(FStringView(In.Component));
                    if (Type == nullptr || !ECS::Utils::HasComponent(Registry, Entity, Type))
                    {
                        // A script is not a component, so removing one detaches it rather than the holder.
                        if (CEntityScript* Script =
                                FindEntityScriptByName(Registry, Entity, FStringView(In.Component)))
                        {
                            bool bDetached = false;
                            SessionOps::RunTransacted("Remove Script (agent)", [&]()
                            {
                                bDetached = EntityScripts::Remove(Registry, Entity, Script);
                            }, SceneError);

                            Out.bRemoved = bDetached;
                            return bDetached
                                ? Agent::FToolResult::Ok(Lumina::Format("Removed script {} from '{}'.",
                                    In.Component, NameOf(Registry, Entity)))
                                : Agent::FToolResult::Error(Lumina::Format("{} could not be detached from '{}'.",
                                    In.Component, NameOf(Registry, Entity)));
                        }

                        return Agent::FToolResult::Error(Lumina::Format(
                            "'{}' has no component or script named '{}'.",
                            NameOf(Registry, Entity), In.Component));
                    }

                    SessionOps::RemoveComponentTransacted("Remove Component (agent)", Entity, Type, SceneError);

                    Out.bRemoved = !ECS::Utils::HasComponent(Registry, Entity, Type);
                    if (!Out.bRemoved)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} could not be removed from '{}'.",
                            In.Component, NameOf(Registry, Entity)));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Removed {} from '{}'.",
                        In.Component, NameOf(Registry, Entity)));
                });
        }

        void RegisterDestroyEntities(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDuplicateEntityParams, SDuplicateEntityResult>(
                Owner, "entity.duplicate",
                "Duplicate an entity with its components, scripts and children, as the editor's Duplicate does, as one undo step.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SDuplicateEntityParams& In, SDuplicateEntityResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    CWorld* World = SessionOps::GetSceneWorld(SceneError);
                    if (ScenePtr == nullptr || World == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    if (SessionOps::IsSimulating())
                    {
                        return Agent::FToolResult::Error("Stop play-in-editor first.");
                    }

                    ECS::FRegistry& Registry = *ScenePtr;
                    ECS::FEntity Source = ECS::NullEntity;
                    FString Error;
                    if (!Agent::FEntityTokens::Resolve(Registry, FStringView(In.Entity), Source, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    // The editor refuses the same, since a copy of one member would sit half inside the instance.
                    if (const SPrefabInstanceComponent* Instance = Registry.TryGet<SPrefabInstanceComponent>(Source);
                        Instance != nullptr && !Instance->bIsRoot)
                    {
                        return Agent::FToolResult::Error("That entity is part of a prefab instance; duplicate the instance root instead.");
                    }

                    ECS::FEntity Copy = ECS::NullEntity;
                    if (!SessionOps::RunCreationTransacted("Duplicate (agent)", [&]()
                    {
                        World->DuplicateEntity(Copy, Source, &EditorEntityUtils::DefaultDuplicateFilter);
                    }, SceneError))
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    if (Copy == ECS::NullEntity || !Registry.IsValid(Copy))
                    {
                        return Agent::FToolResult::Error("The duplicate was not created.");
                    }

                    Out.Entity = Agent::FEntityTokens::Mint(Registry, Copy);
                    return Agent::FToolResult::Ok(Lumina::Format("Duplicated {} as {}.", In.Entity, Out.Entity));
                });

            Agent::FToolRegistry::Get().Register<SSetParentParams, SSetParentResult>(
                Owner, "entity.set_parent",
                "Parent an entity under another, or move it to the world root with an empty Parent, as one undo step.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetParentParams& In, SSetParentResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    if (SessionOps::IsSimulating())
                    {
                        return Agent::FToolResult::Error("Stop play-in-editor first.");
                    }

                    ECS::FRegistry& Registry = *ScenePtr;
                    ECS::FEntity Child  = ECS::NullEntity;
                    ECS::FEntity Parent = ECS::NullEntity;
                    FString Error;
                    if (!Agent::FEntityTokens::Resolve(Registry, FStringView(In.Entity), Child, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (!In.Parent.empty() && !Agent::FEntityTokens::Resolve(Registry, FStringView(In.Parent), Parent, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (Parent == Child || (Parent != ECS::NullEntity && ECS::Utils::IsDescendantOf(Registry, Parent, Child)))
                    {
                        return Agent::FToolResult::Error("The new parent is inside the entity's own subtree, which would make a cycle.");
                    }

                    if (!SessionOps::RunTransacted("Reparent Entity (agent)", [&]()
                    {
                        ECS::Utils::ReparentEntity(Registry, Child, Parent, In.bKeepWorldTransform);
                    }, SceneError))
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    Out.bReparented = true;
                    return Agent::FToolResult::Ok(In.Parent.empty()
                        ? Lumina::Format("Moved {} to the world root.", In.Entity)
                        : Lumina::Format("Parented {} under {}.", In.Entity, In.Parent));
                });

            Agent::FToolRegistry::Get().Register<SDestroyEntitiesParams, SDestroyEntitiesResult>(
                Owner, "entity.destroy",
                "Destroy entities and their children, as one undo step.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SDestroyEntitiesParams& In, SDestroyEntitiesResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    if (SessionOps::IsSimulating())
                    {
                        return Agent::FToolResult::Error("Stop play-in-editor first.");
                    }

                    ECS::FRegistry& Registry = *ScenePtr;
                    CWorld* World = SessionOps::GetSceneWorld(SceneError);

                    TVector<ECS::FEntity> Doomed;
                    for (const FString& Token : In.Entities)
                    {
                        ECS::FEntity Entity = ECS::NullEntity;
                        FString Error;
                        if (Agent::FEntityTokens::Resolve(Registry, FStringView(Token), Entity, Error))
                        {
                            Doomed.push_back(Entity);
                        }
                        else
                        {
                            Out.Skipped.push_back(Token);
                        }
                    }

                    if (Doomed.empty())
                    {
                        return Agent::FToolResult::Error("None of the ids named a live entity.");
                    }

                    SessionOps::RunDestroyTransacted("Delete Entity (agent)", Doomed, [&]()
                    {
                        for (ECS::FEntity Entity : Doomed)
                        {
                            // A child already taken down with its parent is no longer valid here.
                            if (Registry.IsValid(Entity))
                            {
                                World->DestroyEntity(Entity);
                                ++Out.Destroyed;
                            }
                        }
                    }, SceneError);

                    return Agent::FToolResult::Ok(Lumina::Format("Destroyed {} entit{}.{}",
                        Out.Destroyed, Out.Destroyed == 1 ? "y" : "ies",
                        Out.Skipped.empty() ? "" : Lumina::Format(" {} id(s) named nothing.", Out.Skipped.size())));
                });
        }

        // The entities a call names, with any id that resolves to nothing kept for the reply.
        TVector<ECS::FEntity> ResolveEntities(ECS::FRegistry& Registry, const TVector<FString>& Tokens, TVector<FString>& OutSkipped)
        {
            TVector<ECS::FEntity> Entities;
            for (const FString& Token : Tokens)
            {
                ECS::FEntity Entity = ECS::NullEntity;
                FString Error;
                if (Agent::FEntityTokens::Resolve(Registry, FStringView(Token), Entity, Error))
                {
                    Entities.push_back(Entity);
                }
                else
                {
                    OutSkipped.push_back(Token);
                }
            }
            return Entities;
        }

        // Tags live one per named storage, as the editor's tag chips add them, so an entity can carry several.
        Agent::FToolResult EditTags(const SEntityTagParams& In, SEntityTagResult& Out, bool bAdd)
        {
            FString SceneError;
            ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
            if (ScenePtr == nullptr)
            {
                return Agent::FToolResult::Error(SceneError);
            }
            if (SessionOps::IsSimulating())
            {
                return Agent::FToolResult::Error("Stop play-in-editor first.");
            }
            if (In.Tag.empty())
            {
                return Agent::FToolResult::Error("Tag is empty.");
            }

            ECS::FRegistry& Registry = *ScenePtr;
            const TVector<ECS::FEntity> Entities = ResolveEntities(Registry, In.Entities, Out.Skipped);
            if (Entities.empty())
            {
                return Agent::FToolResult::Error("None of the ids named a live entity.");
            }

            const FName Tag(In.Tag.c_str());
            SessionOps::RunTransacted(bAdd ? "Add Tag (agent)" : "Remove Tag (agent)", [&]()
            {
                ECS::TComponentStorage<STagComponent> Storage = Registry.NamedStorage<STagComponent>(Tag);
                for (ECS::FEntity Entity : Entities)
                {
                    if (bAdd == Storage.Contains(Entity))
                    {
                        continue;
                    }
                    if (bAdd)
                    {
                        Storage.Emplace(Entity).Tag = Tag;
                    }
                    else
                    {
                        Storage.RemoveEntity(Entity);

                        // The single-tag path also records its tag in the unnamed storage, which would otherwise outlive it.
                        const STagComponent* Unnamed = Registry.TryGet<STagComponent>(Entity);
                        if (Unnamed != nullptr && Unnamed->Tag == Tag)
                        {
                            Registry.Remove<STagComponent>(Entity);
                        }
                    }
                    ++Out.Changed;
                }
            }, SceneError);

            return Agent::FToolResult::Ok(Lumina::Format("{} '{}' {} {} entit{}.{}", bAdd ? "Added" : "Removed", In.Tag,
                bAdd ? "to" : "from", Out.Changed, Out.Changed == 1 ? "y" : "ies",
                Out.Skipped.empty() ? "" : Lumina::Format(" {} id(s) named nothing.", Out.Skipped.size())));
        }

        Agent::FToolResult FindByTag(const SFindByTagParams& In, SListEntitiesResult& Out)
        {
            FString SceneError;
            ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
            if (ScenePtr == nullptr)
            {
                return Agent::FToolResult::Error(SceneError);
            }

            ECS::FRegistry& Registry = *ScenePtr;
            const int32 Limit = In.Limit > 0 ? In.Limit : 100;
            const int32 Offset = In.Offset > 0 ? In.Offset : 0;
            if (const ECS::FSparseSet* Storage = Registry.FindNamedStorage(ECS::GetComponentTypeID<STagComponent>(), FName(In.Tag.c_str())))
            {
                for (const ECS::FEntity Entity : *Storage)
                {
                    if (Entity.IsTombstone())
                    {
                        continue;
                    }
                    ++Out.Matched;
                    if (Out.Matched <= Offset || static_cast<int32>(Out.Entities.size()) >= Limit)
                    {
                        continue;
                    }
                    SEntityInfo Info;
                    Info.Id   = Agent::FEntityTokens::Mint(Registry, Entity);
                    Info.Name = NameOf(Registry, Entity);
                    Out.Entities.push_back(Move(Info));
                }
            }

            return Agent::FToolResult::Ok(Lumina::Format("{} of {} entities tagged '{}'.", Out.Entities.size(), Out.Matched, In.Tag));
        }

        Agent::FToolResult ListTags(const SListTagsParams& In, SListTagsResult& Out)
        {
            FString SceneError;
            ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
            if (ScenePtr == nullptr)
            {
                return Agent::FToolResult::Error(SceneError);
            }

            ECS::FRegistry& Registry = *ScenePtr;
            const ECS::FComponentTypeID TagType = ECS::GetComponentTypeID<STagComponent>();
            for (ECS::FSparseSet* Storage : Registry.GetActiveStorages())
            {
                if (Storage->GetTypeInfo().TypeID != TagType)
                {
                    continue;
                }

                // The unnamed storage mixes every tag, so only a storage that is its own tag's named one counts.
                const STagComponent* First = nullptr;
                int32 Live = 0;
                for (const ECS::FEntity Entity : *Storage)
                {
                    if (Entity.IsTombstone())
                    {
                        continue;
                    }
                    if (First == nullptr)
                    {
                        First = static_cast<const STagComponent*>(Storage->GetRaw(Entity));
                    }
                    ++Live;
                }
                if (First == nullptr || Registry.FindNamedStorage(TagType, First->Tag) != Storage)
                {
                    continue;
                }

                const FString Tag(First->Tag.ToString().c_str());
                if (ContainsText(FStringView(Tag), In.Contains))
                {
                    STagCount& Count = Out.Tags.emplace_back();
                    Count.Tag      = Tag;
                    Count.Entities = Live;
                }
            }

            return Agent::FToolResult::Ok(Lumina::Format("{} tag(s).", Out.Tags.size()));
        }

        void RegisterTagTools(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SEntityTagParams, SEntityTagResult>(
                Owner, "entity.add_tag",
                "Add a tag to entities, as one undo step. Entities is a list of ids, and an entity can carry several tags.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SEntityTagParams& In, SEntityTagResult& Out) { return EditTags(In, Out, true); });

            Agent::FToolRegistry::Get().Register<SEntityTagParams, SEntityTagResult>(
                Owner, "entity.remove_tag",
                "Remove a tag from entities, as one undo step. Entities without it are left alone.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SEntityTagParams& In, SEntityTagResult& Out) { return EditTags(In, Out, false); });

            Agent::FToolRegistry::Get().Register<SFindByTagParams, SListEntitiesResult>(
                Owner, "scene.find_by_tag",
                "List the entities carrying a tag, with the id every other tool takes. Limit and Offset page a long list.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SFindByTagParams& In, SListEntitiesResult& Out) { return FindByTag(In, Out); });

            Agent::FToolRegistry::Get().Register<SListTagsParams, SListTagsResult>(
                Owner, "scene.list_tags",
                "List every tag in the open world with how many entities carry it. Contains filters by name.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListTagsParams& In, SListTagsResult& Out) { return ListTags(In, Out); });
        }
    }

    void RegisterSceneTools(FStringView Owner)
    {
        RegisterTagTools(Owner);
        RegisterListComponentTypes(Owner);
        RegisterListEntities(Owner);
        RegisterDescribeEntity(Owner);
        RegisterCreateEntity(Owner);
        RegisterAddComponent(Owner);
        RegisterAddScript(Owner);
        RegisterSetEntityProperty(Owner);
        RegisterRemoveComponent(Owner);
        RegisterDestroyEntities(Owner);
    }
}
