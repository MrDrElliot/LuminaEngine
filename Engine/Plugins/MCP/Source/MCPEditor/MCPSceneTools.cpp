#include "MCPSceneTools.h"
#include "World/ECS/Registry.h"

#include "Agent/AgentEntityToken.h"
#include "Agent/AgentPropertyPath.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "MCPTextMatch.h"
#include "Core/Engine/Engine.h"
#include "LuminaEditor.h"
#include "Scene/SceneOps.h"
#include "UI/EditorUI.h"
#include "UI/Tools/WorldEditorTool.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/EntityUtils.h"
#include "World/World.h"

namespace Lumina::MCP
{
    namespace
    {
        constexpr const char* GNoWorldEditor = "No world editor is open, so there is no scene to work on.";

        FWorldEditorTool* FindWorldEditor()
        {
            if (GEditorEngine == nullptr)
            {
                return nullptr;
            }

            FEditorUI* UI = static_cast<FEditorUI*>(GEditorEngine->GetDevelopmentToolsUI());
            return UI != nullptr ? UI->FindTool<FWorldEditorTool>() : nullptr;
        }

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
                    FWorldEditorTool* Tool = FindWorldEditor();
                    if (Tool == nullptr)
                    {
                        return Agent::FToolResult::Error(GNoWorldEditor);
                    }

                    ECS::FRegistry& Registry = Tool->GetSceneEntityRegistry();
                    const int32 Limit = In.Limit > 0 ? In.Limit : 100;

                    for (auto Entity : Registry.View<SNameComponent>())
                    {
                        const FString Name = NameOf(Registry, Entity);
                        if (!ContainsText(FStringView(Name), In.Contains))
                        {
                            continue;
                        }

                        ++Out.Matched;

                        if (static_cast<int32>(Out.Entities.size()) >= Limit)
                        {
                            continue;
                        }

                        SEntityInfo Info;
                        Info.Id   = Agent::FEntityTokens::Mint(Registry, Entity);
                        Info.Name = Name;

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
                    FWorldEditorTool* Tool = FindWorldEditor();
                    if (Tool == nullptr)
                    {
                        return Agent::FToolResult::Error(GNoWorldEditor);
                    }

                    ECS::FRegistry& Registry = Tool->GetSceneEntityRegistry();

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
                    FWorldEditorTool* Tool = FindWorldEditor();
                    if (Tool == nullptr)
                    {
                        return Agent::FToolResult::Error(GNoWorldEditor);
                    }

                    CWorld* World = Tool->GetSceneWorld();
                    if (World == nullptr)
                    {
                        return Agent::FToolResult::Error(GNoWorldEditor);
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

                    ECS::FRegistry& Registry = Tool->GetSceneEntityRegistry();
                    const FName EntityName(In.Name.empty() ? "Entity" : In.Name.c_str());

                    ECS::FEntity Created = ECS::NullEntity;

                    Tool->RunCreationTransacted("Create Entity (agent)", [&]()
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
                    });

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
                    FWorldEditorTool* Tool = FindWorldEditor();
                    if (Tool == nullptr)
                    {
                        return Agent::FToolResult::Error(GNoWorldEditor);
                    }

                    ECS::FRegistry& Registry = Tool->GetSceneEntityRegistry();

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

                    Tool->RunTransacted("Add Component (agent)", [&]()
                    {
                        SceneOps::ApplyAddComponent(Registry, Plan, Type);
                    });

                    Out.bAdded = true;

                    return Agent::FToolResult::Ok(Lumina::Format("Added {} to '{}'.",
                        In.Component, NameOf(Registry, Entity)));
                });
        }

        void RegisterSetProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetPropertyParams, SSetPropertyResult>(
                Owner, "entity.set_property",
                "Set one field on one component of an entity.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetPropertyParams& In, SSetPropertyResult& Out)
                {
                    FWorldEditorTool* Tool = FindWorldEditor();
                    if (Tool == nullptr)
                    {
                        return Agent::FToolResult::Error(GNoWorldEditor);
                    }

                    ECS::FRegistry& Registry = Tool->GetSceneEntityRegistry();

                    ECS::FEntity Entity = ECS::NullEntity;
                    FString Error;
                    if (!Agent::FEntityTokens::Resolve(Registry, FStringView(In.Entity), Entity, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CStruct* Type = SceneOps::ResolveComponentType(FStringView(In.Component));
                    if (!Type || !ECS::Utils::HasComponent(Registry, Entity, Type))
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "'{}' has no component named '{}'.", NameOf(Registry, Entity), In.Component));
                    }

                    CStruct* Reflected = Type;

                    const FComponentOps* Ops = Reflected != nullptr
                        ? FindComponentOps(FStringView(Reflected->GetName().ToString()))
                        : nullptr;

                    void* Data = Ops != nullptr && Ops->Get != nullptr ? Ops->Get(Registry, Entity) : nullptr;
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

                    const nlohmann::json Value = nlohmann::json::parse(
                        In.Value.c_str(), In.Value.c_str() + In.Value.size(), nullptr, false);

                    if (Value.is_discarded())
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "Value is not JSON. Strings need quotes, so \"Torch\" rather than Torch."));
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
                    Tool->RunTransacted("Set Property (agent)", [&]()
                    {
                        // A bare store reaches no hook, so the renderer keeps serving the old baked record.
                        SceneOps::FPropertyEditScope Edit(Registry, Entity, Reflected, Data,
                            Target.Property, Target.ValuePtr);

                        Applied = Agent::ReadProperty(Value, Target.Property, Target.ValuePtr, FStringView(In.Path));
                    });

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
    }

    void RegisterSceneTools(FStringView Owner)
    {
        RegisterListComponentTypes(Owner);
        RegisterListEntities(Owner);
        RegisterDescribeEntity(Owner);
        RegisterCreateEntity(Owner);
        RegisterAddComponent(Owner);
        RegisterSetProperty(Owner);
    }
}
