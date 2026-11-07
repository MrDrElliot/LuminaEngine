#include "MCPSceneTools.h"
#include "World/ECS/Registry.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentEntityToken.h"
#include "Agent/AgentPropertyPath.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "MCPTextMatch.h"
#include "Scene/SceneOps.h"
#include "Scripting/EntityScript.h"
#include "Session/SessionOps.h"
#include "UI/Tools/EditorEntityUtils.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TagComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/EntityUtils.h"
#include "World/World.h"
#include "World/WorldManager.h"

namespace Lumina::MCP
{
    namespace MCPSceneToolsInternal
    {
        constexpr const char* GSceneNoWorldEditor = "No world editor is open, so there is no scene to work on.";
        constexpr const char* GSceneStopPlayFirst      = "Stop play-in-editor first.";

        FString SceneEntityName(const ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            const SNameComponent* Name = Registry.TryGet<SNameComponent>(Entity);
            return Name != nullptr ? FString(*Name->Name.ToString()) : FString("Entity");
        }

        bool ResolveSceneEntity(const FString& Token, ECS::FRegistry*& OutRegistry, ECS::FEntity& OutEntity, FString& OutError)
        {
            OutRegistry = SessionOps::GetSceneRegistry(OutError);
            return OutRegistry != nullptr && Agent::FEntityTokens::Resolve(*OutRegistry, FStringView(Token), OutEntity, OutError);
        }

        TVector<ECS::FEntity> ResolveSceneEntities(ECS::FRegistry& Registry, const TVector<FString>& Tokens, TVector<FString>& OutSkipped)
        {
            TVector<ECS::FEntity> Entities;
            for (const FString& Token : Tokens)
            {
                ECS::FEntity Entity = ECS::NullEntity;
                FString Error;
                if (Agent::FEntityTokens::Resolve(Registry, FStringView(Token), Entity, Error))
                {
                    Entities.Add(Entity);
                }
                else
                {
                    OutSkipped.Add(Token);
                }
            }
            return Entities;
        }

        // Values come back as text because a free-form component tree has no fixed reflected shape.
        FString DescribeEntityComponentValues(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            FJsonValue Components = FJsonValue::MakeObject();

            ForEachComponentStruct([&](CStruct* Reflected)
            {
                if (!ECS::Utils::HasComponent(Registry, Entity, Reflected))
                {
                    return;
                }

                const FString Key(*Reflected->GetName().ToString());

                // The direct op table hands back the live pointer, which the meta trampoline will not.
                const FComponentOps* Ops = FindComponentOps(FStringView(Reflected->GetName().ToString()));
                void* Data = Ops != nullptr ? Ops->Get(Registry, Entity) : nullptr;

                if (Data == nullptr)
                {
                    Components.Set(Key, FJsonValue::MakeObject());
                    return;
                }

                FJsonValue Written;
                Components.Set(Key, Agent::WriteStruct(Reflected, Data, Written).IsValid()
                    ? Move(Written)
                    : FJsonValue::MakeObject());
            });

            if (const SEntityScriptComponent* Scripts = Registry.TryGet<SEntityScriptComponent>(Entity))
            {
                for (const TStrongObjectPtr<CEntityScript>& Held : Scripts->Scripts)
                {
                    CEntityScript* Script = Held.Get();
                    if (Script == nullptr || Script->GetClass() == nullptr)
                    {
                        continue;
                    }

                    FJsonValue Written;
                    Components.Set(FStringView(*Script->GetClass()->GetName().ToString()),
                        Agent::WriteStruct(Script->GetClass(), Script, Written).IsValid() ? Move(Written) : FJsonValue::MakeObject());
                }
            }

            return Components.ToString(2);
        }

        // Entity fields store a raw handle, so the ids every other tool hands out are swapped in for it here.
        bool ResolveEntityTokenFields(const ECS::FRegistry& Registry, FProperty* Property, FJsonValue& Value, FString& OutError)
        {
            if (Property->GetType() == EPropertyTypeFlags::Vector && Value.IsArray())
            {
                FProperty* Inner = static_cast<FArrayProperty*>(Property)->GetInternalProperty();
                for (FJsonValue& Element : Value.AsArray())
                {
                    if (Inner != nullptr && !ResolveEntityTokenFields(Registry, Inner, Element, OutError))
                    {
                        return false;
                    }
                }
                return true;
            }

            if (!Property->IsA(EPropertyTypeFlags::Entity) || !Value.IsString())
            {
                return true;
            }

            const FStringView Token = Value.AsString();
            ECS::FEntity Resolved = ECS::NullEntity;
            if (!Token.IsEmpty() && !Agent::FEntityTokens::Resolve(Registry, Token, Resolved, OutError))
            {
                return false;
            }

            Value = FJsonValue(static_cast<uint32>(Resolved));
            return true;
        }

        CEntityScript* FindEntityScriptByName(ECS::FRegistry& Registry, ECS::FEntity Entity, FStringView Name)
        {
            const SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr || Name.IsEmpty())
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

                const int32 Dot = FStringView(Full).FindLastOf('.');
                if (Dot != Constants::kIndexNone && EqualsTextFold(FStringView(Full).Mid(Dot + 1), Name))
                {
                    return Script;
                }
            }
            return nullptr;
        }

        SEntityInfo MakeSceneEntityInfo(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            SEntityInfo Info;
            Info.Id   = Agent::FEntityTokens::Mint(Registry, Entity);
            Info.Name = SceneEntityName(Registry, Entity);
            if (const ECS::FEntity Parent = Registry.GetHierarchy().GetParent(Entity); Parent != ECS::NullEntity)
            {
                Info.Parent = Agent::FEntityTokens::Mint(Registry, Parent);
            }
            if (const SPrefabInstanceComponent* Instance = Registry.TryGet<SPrefabInstanceComponent>(Entity))
            {
                Info.Prefab      = Instance->SourcePrefab ? FString(*Instance->SourcePrefab->GetName()) : FString("<missing>");
                Info.bPrefabRoot = Instance->bIsRoot;
            }
            return Info;
        }

        Agent::FToolResult SceneFind(const SFindEntitiesParams& In, SFindEntitiesResult& Out)
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

            auto Visit = [&](ECS::FEntity Entity)
            {
                if (!ContainsText(FStringView(SceneEntityName(Registry, Entity)), In.Contains))
                {
                    return;
                }
                ++Out.Matched;
                if (Out.Matched > Offset && Out.Entities.Num() < Limit)
                {
                    Out.Entities.Add(MakeSceneEntityInfo(Registry, Entity));
                }
            };

            if (In.Tag.IsEmpty())
            {
                for (auto Entity : Registry.View<SNameComponent>())
                {
                    Visit(Entity);
                }
                return Agent::FToolResult::Ok(Lumina::Format("{} of {} matching entities.", Out.Entities.Num(), Out.Matched));
            }

            if (const ECS::FSparseSet* Storage = Registry.FindNamedStorage(ECS::GetComponentTypeID<STagComponent>(), FName(*In.Tag)))
            {
                for (const ECS::FEntity Entity : *Storage)
                {
                    if (!Entity.IsTombstone())
                    {
                        Visit(Entity);
                    }
                }
            }
            return Agent::FToolResult::Ok(Lumina::Format("{} of {} entities tagged '{}'.", Out.Entities.Num(), Out.Matched, In.Tag));
        }

        void RegisterSceneFind(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SFindEntitiesParams, SFindEntitiesResult>(
                Owner, "scene.find",
                "List entities in the open world with the id every other tool takes. Contains (name) and Tag filter and combine; Limit and Offset page.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SFindEntitiesParams& In, SFindEntitiesResult& Out) { return SceneFind(In, Out); });
        }

        Agent::FToolResult SceneSelect(const SSelectEntitiesParams& In, SSelectEntitiesResult& Out)
        {
            FString SceneError;
            ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
            if (ScenePtr == nullptr)
            {
                return Agent::FToolResult::Error(SceneError);
            }

            TVector<ECS::FEntity> Entities;
            for (const FString& Token : In.Entities)
            {
                ECS::FEntity Entity = ECS::NullEntity;
                FString Error;
                if (!Agent::FEntityTokens::Resolve(*ScenePtr, FStringView(Token), Entity, Error))
                {
                    return Agent::FToolResult::Error(Error);
                }
                Entities.Add(Entity);
            }

            if (!SessionOps::SelectEntities(Entities, SceneError))
            {
                return Agent::FToolResult::Error(SceneError);
            }
            Out.Selected = Entities.Num();
            return Agent::FToolResult::Ok(Lumina::Format("Selected {} entities.", Out.Selected));
        }

        void RegisterSceneSelect(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSelectEntitiesParams, SSelectEntitiesResult>(
                Owner, "scene.select",
                "Replace the targeted scene's selection with these entity ids, as clicking in the outliner does. An empty list clears it.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSelectEntitiesParams& In, SSelectEntitiesResult& Out) { return SceneSelect(In, Out); });
        }

        Agent::FToolResult SceneListSystems(const SListSystemsParams& In, SListSystemsResult& Out)
        {
            FString Error;
            CWorld* World = SessionOps::GetSceneWorld(Error);
            if (World == nullptr)
            {
                return Agent::FToolResult::Error(Error);
            }

            TVector<CWorld::FSystemInfo> Infos;
            World->GetAllSystems(Infos);
            for (const CWorld::FSystemInfo& Info : Infos)
            {
                const FString Name(*Info.Name.ToString());
                if (!ContainsTextFold(FStringView(Name), In.Contains))
                {
                    continue;
                }
                SSystemListing& Listing = Out.Systems.EmplaceGetRef();
                Listing.Name = Name;
                Listing.bEnabled = Info.bEnabled;
                for (EUpdateStage Stage : Info.Stages)
                {
                    Listing.Stages.Add((int32)Stage);
                }
                for (FName System : Info.RunsAfter)
                {
                    Listing.RunsAfter.Emplace(*System.ToString());
                }
                for (FName System : Info.RunsBefore)
                {
                    Listing.RunsBefore.Emplace(*System.ToString());
                }
            }

            TVector<FSystemScheduleEntry> Schedule;
            World->GetSystemSchedule(Schedule);
            for (const FSystemScheduleEntry& Entry : Schedule)
            {
                const FString Name(*Entry.Name.ToString());
                if (!ContainsTextFold(FStringView(Name), In.Contains))
                {
                    continue;
                }
                SScheduledSystem& Scheduled = Out.Schedule.EmplaceGetRef();
                Scheduled.Name       = Name;
                Scheduled.Stage      = Entry.Stage;
                Scheduled.Batch      = Entry.Batch;
                Scheduled.BatchSize  = Entry.BatchSize;
                Scheduled.bExclusive = Entry.bExclusive;
            }
            return Agent::FToolResult::Ok(Lumina::Format("{} system(s).", Out.Systems.Num()));
        }

        void RegisterSceneListSystems(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListSystemsParams, SListSystemsResult>(
                Owner, "scene.list_systems",
                "List the world's entity systems, C++ and C#, with their stages and declared order, plus the batches the running schedule placed them in.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListSystemsParams& In, SListSystemsResult& Out) { return SceneListSystems(In, Out); });
        }

        Agent::FToolResult SceneListTags(const SListTagsParams& In, SListTagsResult& Out)
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

                const FString Tag(*First->Tag.ToString());
                if (ContainsText(FStringView(Tag), In.Contains))
                {
                    STagCount& Count = Out.Tags.EmplaceGetRef();
                    Count.Tag      = Tag;
                    Count.Entities = Live;
                }
            }

            return Agent::FToolResult::Ok(Lumina::Format("{} tag(s).", Out.Tags.Num()));
        }

        void RegisterSceneListTags(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListTagsParams, SListTagsResult>(
                Owner, "scene.list_tags",
                "List every tag in the open world with how many entities carry it.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListTagsParams& In, SListTagsResult& Out) { return SceneListTags(In, Out); });
        }

        Agent::FToolResult SceneListComponentTypes(const SListComponentTypesParams& In, SListComponentTypesResult& Out)
        {
            ForEachComponentStruct([&](CStruct* Reflected)
            {
                // The editor hides these from its own picker, so an agent should not see them either.
                if (Reflected->HasMeta("HideInComponentList"))
                {
                    return;
                }

                const FString Name(*Reflected->GetName().ToString());
                if (!ContainsText(FStringView(Name), In.Contains))
                {
                    return;
                }

                SComponentTypeInfo Info;
                Info.Name        = Name;
                Info.DisplayName = FString(*Reflected->MakeDisplayName());
                Info.Category    = Reflected->HasMeta("Category") ? Reflected->GetMeta("Category") : FString("General");

                Out.Types.Add(Move(Info));
            });

            return Agent::FToolResult::Ok(Lumina::Format("{} component type(s).", Out.Types.Num()));
        }

        void RegisterSceneListComponentTypes(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListComponentTypesParams, SListComponentTypesResult>(
                Owner, "scene.list_component_types",
                "List every component type that can be attached to an entity.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListComponentTypesParams& In, SListComponentTypesResult& Out) { return SceneListComponentTypes(In, Out); });
        }

        Agent::FToolResult SceneCreateEntity(const SCreateEntityParams& In, SCreateEntityResult& Out)
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
                return Agent::FToolResult::Error(GSceneNoWorldEditor);
            }

            // Resolved before the transaction opens, so an unknown name costs no snapshot.
            TVector<CStruct*> Types;
            for (const FString& Name : In.Components)
            {
                CStruct* Type = SceneOps::ResolveComponentType(FStringView(Name));
                if (Type != nullptr)
                {
                    Types.Add(Type);
                    Out.Attached.Add(Name);
                }
                else
                {
                    Out.Skipped.Add(Name);
                }
            }

            ECS::FRegistry& Registry = *ScenePtr;
            const FName EntityName(In.Name.IsEmpty() ? "Entity" : *In.Name);

            ECS::FEntity Created = ECS::NullEntity;

            SessionOps::RunCreationTransacted("Create Entity (agent)", [&]()
            {
                Created = World->ConstructEntity(EntityName);
                if (Created == ECS::NullEntity)
                {
                    return;
                }

                SessionOps::AdoptCreatedEntity(Created);

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

            return Agent::FToolResult::Ok(Out.Skipped.IsEmpty()
                ? Lumina::Format("Created '{}' with {} component(s).", In.Name, Out.Attached.Num())
                : Lumina::Format("Created '{}' with {} component(s). {} name(s) matched no component type.",
                    In.Name, Out.Attached.Num(), Out.Skipped.Num()));
        }

        void RegisterSceneCreateEntity(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateEntityParams, SCreateEntityResult>(
                Owner, "scene.create_entity",
                "Create an entity in the open world, optionally attaching components to it.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateEntityParams& In, SCreateEntityResult& Out) { return SceneCreateEntity(In, Out); });
        }

        Agent::FToolResult SceneGroundHeight(const SGroundHeightParams& In, SGroundHeightResult& Out)
        {
            FJsonValue Points;
            if (!FJsonValue::Parse(In.Points, Points) || !Points.IsArray())
            {
                return Agent::FToolResult::Error("Points is an array of [x,z] pairs.");
            }
            for (const FJsonValue& Point : Points.AsArray())
            {
                if (!Point.IsArray() || Point.Num() < 2 || !Point[0].IsNumber() || !Point[1].IsNumber() || !Point[Point.Num() - 1].IsNumber())
                {
                    return Agent::FToolResult::Error("Each point is [x,z].");
                }
                Out.Heights.Add(GroundHeightAt(Point[0].AsFloat(), Point[Point.Num() - 1].AsFloat()));
            }
            return Agent::FToolResult::Ok(Lumina::Format("Probed {} point(s).", Out.Heights.Num()));
        }

        void RegisterSceneGroundHeight(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SGroundHeightParams, SGroundHeightResult>(
                Owner, "scene.ground_height",
                "Report the height of the first surface under each [x,z] point, in the game world when one runs, else the editor's.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SGroundHeightParams& In, SGroundHeightResult& Out) { return SceneGroundHeight(In, Out); });
        }

        Agent::FToolResult SceneCameraGet(const SSceneCameraParams& In, SSceneCameraResult& Out)
        {
            FString Error;
            CWorld* World = In.bEditorWorld ? nullptr : FindGameWorld();
            Out.World = World != nullptr ? "Game" : "Editor";
            if (World == nullptr)
            {
                World = SessionOps::GetSceneWorld(Error);
            }
            if (World == nullptr)
            {
                return Agent::FToolResult::Error(Error.IsEmpty() ? FString("No world is open.") : Error);
            }

            const ECS::FEntity Camera = World->GetCamera()->GetActiveCameraEntity();
            const STransformComponent* Transform = Camera != ECS::NullEntity && World->IsValidEntity(Camera)
                ? World->TryGetComponent<STransformComponent>(Camera) : nullptr;
            if (Transform == nullptr)
            {
                return Agent::FToolResult::Error("The world has no live camera.");
            }

            const FVector3 Location = Transform->GetWorldLocation();
            const FVector3 Rotation = Math::Degrees(Math::YawFirstEulerAngles(Transform->GetWorldRotation()));
            FVector3 Forward = Transform->GetWorldRotation() * FVector3(0.0f, 0.0f, -1.0f);
            Out.FOV = 90.0f;
            if (const SCameraComponent* Component = World->TryGetComponent<SCameraComponent>(Camera))
            {
                Forward = Component->GetForwardVector();
                Out.FOV = Component->FOV;
            }

            Out.Location = { Location.x, Location.y, Location.z };
            Out.Rotation = { Rotation.x, Rotation.y, Rotation.z };
            Out.Forward = { Forward.x, Forward.y, Forward.z };
            Out.ShotKey = Lumina::Format("{{\"Time\":0,\"Location\":[{:.2f},{:.2f},{:.2f}],\"Rotation\":[{:.2f},{:.2f},{:.2f}],\"FOV\":{:.1f}}}",
                Location.x, Location.y, Location.z, Rotation.x, Rotation.y, Rotation.z, Out.FOV);
            return Agent::FToolResult::Ok(Out.ShotKey);
        }

        void RegisterSceneCamera(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSceneCameraParams, SSceneCameraResult>(
                Owner, "scene.camera",
                "Get reports the live camera's view (game world when playing) with a key for sequence.shot. Set moves the editor viewport camera to Location, facing Target or Rotation.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSceneCameraParams& In, SSceneCameraResult& Out)
                {
                    return In.Action == ESceneCameraAction::Set ? SceneCameraSet(In, Out) : SceneCameraGet(In, Out);
                });
        }

        Agent::FToolResult EntityDescribe(const SDescribeEntityParams& In, SDescribeEntityResult& Out)
        {
            ECS::FRegistry* ScenePtr = nullptr;
            ECS::FEntity Entity = ECS::NullEntity;
            FString Error;
            if (!ResolveSceneEntity(In.Entity, ScenePtr, Entity, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            ECS::FRegistry& Registry = *ScenePtr;
            Out.Name = SceneEntityName(Registry, Entity);

            ForEachComponentStruct([&](CStruct* Reflected)
            {
                if (ECS::Utils::HasComponent(Registry, Entity, Reflected))
                {
                    Out.Components.Emplace(*Reflected->GetName().ToString());
                }
            });

            return Agent::FToolResult::Ok(Lumina::Format("{} has {} component(s).\n{}",
                Out.Name, Out.Components.Num(), DescribeEntityComponentValues(Registry, Entity)));
        }

        void RegisterEntityDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDescribeEntityParams, SDescribeEntityResult>(
                Owner, "entity.describe",
                "Report an entity's name, its components and their current values.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SDescribeEntityParams& In, SDescribeEntityResult& Out) { return EntityDescribe(In, Out); });
        }

        Agent::FToolResult EntityAddComponent(const SEntityComponentParams& In, SEntityComponentResult& Out)
        {
            ECS::FRegistry* ScenePtr = nullptr;
            ECS::FEntity Entity = ECS::NullEntity;
            FString Error;
            if (!ResolveSceneEntity(In.Entity, ScenePtr, Entity, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            ECS::FRegistry& Registry = *ScenePtr;
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
                    SceneEntityName(Registry, Entity), In.Component));
            }

            FString SceneError;
            if (!SessionOps::AddComponent(TVector<ECS::FEntity>{ Entity }, Type, SceneError))
            {
                return Agent::FToolResult::Error(SceneError);
            }

            Out.bAdded = true;
            return Agent::FToolResult::Ok(Lumina::Format("Added {} to '{}'.", In.Component, SceneEntityName(Registry, Entity)));
        }

        Agent::FToolResult EntityDetachScript(ECS::FRegistry& Registry, ECS::FEntity Entity, CEntityScript* Script,
            const SEntityComponentParams& In, SEntityComponentResult& Out)
        {
            FString SceneError;
            bool bDetached = false;
            SessionOps::RunComponentTransacted("Remove Script (agent)", Entity, SEntityScriptComponent::StaticStruct(), [&]()
            {
                bDetached = EntityScripts::Remove(Registry, Entity, Script);
            }, SceneError);

            Out.bRemoved = bDetached;
            return bDetached
                ? Agent::FToolResult::Ok(Lumina::Format("Removed script {} from '{}'.", In.Component, SceneEntityName(Registry, Entity)))
                : Agent::FToolResult::Error(Lumina::Format("{} could not be detached from '{}'.", In.Component, SceneEntityName(Registry, Entity)));
        }

        Agent::FToolResult EntityRemoveComponent(const SEntityComponentParams& In, SEntityComponentResult& Out)
        {
            FString SceneError;
            ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
            if (ScenePtr == nullptr)
            {
                return Agent::FToolResult::Error(SceneError);
            }

            if (SessionOps::IsSceneTargetSimulating())
            {
                return Agent::FToolResult::Error(GSceneStopPlayFirst);
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
                if (CEntityScript* Script = FindEntityScriptByName(Registry, Entity, FStringView(In.Component)))
                {
                    return EntityDetachScript(Registry, Entity, Script, In, Out);
                }

                return Agent::FToolResult::Error(Lumina::Format(
                    "'{}' has no component or script named '{}'.", SceneEntityName(Registry, Entity), In.Component));
            }

            SessionOps::RemoveComponentTransacted("Remove Component (agent)", Entity, Type, SceneError);

            Out.bRemoved = !ECS::Utils::HasComponent(Registry, Entity, Type);
            if (!Out.bRemoved)
            {
                return Agent::FToolResult::Error(Lumina::Format("{} could not be removed from '{}'.",
                    In.Component, SceneEntityName(Registry, Entity)));
            }

            return Agent::FToolResult::Ok(Lumina::Format("Removed {} from '{}'.", In.Component, SceneEntityName(Registry, Entity)));
        }

        void RegisterEntityComponent(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SEntityComponentParams, SEntityComponentResult>(
                Owner, "entity.component",
                "Add or Remove a component on an entity, as one undo step. Remove also detaches a script named as Component.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SEntityComponentParams& In, SEntityComponentResult& Out)
                {
                    return In.Action == EEntityComponentAction::Remove ? EntityRemoveComponent(In, Out) : EntityAddComponent(In, Out);
                });
        }

        Agent::FToolResult EntityAddScript(ECS::FRegistry& Registry, ECS::FEntity Entity, CClass* ScriptClass,
            const SEntityScriptParams& In, SEntityScriptResult& Out)
        {
            FString SceneError;
            CEntityScript* Script = nullptr;
            SessionOps::RunComponentTransacted("Add Script (agent)", Entity, SEntityScriptComponent::StaticStruct(), [&]()
            {
                Script = EntityScripts::Attach(Registry, Entity, ScriptClass);
            }, SceneError);

            if (Script == nullptr)
            {
                return Agent::FToolResult::Error(Lumina::Format("'{}' could not be attached.", In.ScriptClass));
            }

            Out.bAdded = true;
            return Agent::FToolResult::Ok(Lumina::Format("Attached {} to '{}'.", In.ScriptClass, SceneEntityName(Registry, Entity)));
        }

        Agent::FToolResult EntityRemoveScript(ECS::FRegistry& Registry, ECS::FEntity Entity, CClass* ScriptClass,
            const SEntityScriptParams& In, SEntityScriptResult& Out)
        {
            if (EntityScripts::Find(Registry, Entity, ScriptClass) == nullptr)
            {
                return Agent::FToolResult::Error(Lumina::Format("'{}' has no {} script.", SceneEntityName(Registry, Entity), In.ScriptClass));
            }

            FString SceneError;
            SessionOps::RunComponentTransacted("Remove Script (agent)", Entity, SEntityScriptComponent::StaticStruct(), [&]()
            {
                CEntityScript* Script = EntityScripts::Find(Registry, Entity, ScriptClass);
                Out.bRemoved = Script != nullptr && EntityScripts::Remove(Registry, Entity, Script);
            }, SceneError);

            if (!Out.bRemoved)
            {
                return Agent::FToolResult::Error(Lumina::Format("'{}' could not be removed.", In.ScriptClass));
            }
            return Agent::FToolResult::Ok(Lumina::Format("Removed {} from '{}'.", In.ScriptClass, SceneEntityName(Registry, Entity)));
        }

        Agent::FToolResult EntityEditScript(const SEntityScriptParams& In, SEntityScriptResult& Out)
        {
            ECS::FRegistry* ScenePtr = nullptr;
            ECS::FEntity Entity = ECS::NullEntity;
            FString Error;
            if (!ResolveSceneEntity(In.Entity, ScenePtr, Entity, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            CClass* ScriptClass = Agent::FindClassByName(FStringView(In.ScriptClass), CEntityScript::StaticClass(), Error);
            if (ScriptClass == nullptr)
            {
                return Agent::FToolResult::Error(Error);
            }

            return In.Action == EEntityScriptAction::Remove
                ? EntityRemoveScript(*ScenePtr, Entity, ScriptClass, In, Out)
                : EntityAddScript(*ScenePtr, Entity, ScriptClass, In, Out);
        }

        // Scripts are subobjects, not assets, so entity.set_property cannot put one in SEntityScriptComponent.
        void RegisterEntityScript(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SEntityScriptParams, SEntityScriptResult>(
                Owner, "entity.script",
                "Add or Remove a C# or C++ entity script class on an entity, as the inspector does. Remove detaches the first of that class.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SEntityScriptParams& In, SEntityScriptResult& Out) { return EntityEditScript(In, Out); });
        }

        // Tags live one per named storage, as the editor's tag chips add them, so an entity can carry several.
        Agent::FToolResult EntityEditTags(const SEntityTagParams& In, SEntityTagResult& Out)
        {
            const bool bAdd = In.Action == EEntityTagAction::Add;

            FString SceneError;
            ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
            if (ScenePtr == nullptr)
            {
                return Agent::FToolResult::Error(SceneError);
            }
            if (SessionOps::IsSceneTargetSimulating())
            {
                return Agent::FToolResult::Error(GSceneStopPlayFirst);
            }
            if (In.Tag.IsEmpty())
            {
                return Agent::FToolResult::Error("Tag is empty.");
            }

            ECS::FRegistry& Registry = *ScenePtr;
            const TVector<ECS::FEntity> Entities = ResolveSceneEntities(Registry, In.Entities, Out.Skipped);
            if (Entities.IsEmpty())
            {
                return Agent::FToolResult::Error("None of the ids named a live entity.");
            }

            const FName Tag(*In.Tag);
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
                    }
                    ++Out.Changed;
                }
            }, SceneError);

            return Agent::FToolResult::Ok(Lumina::Format("{} '{}' {} {} entit{}.{}", bAdd ? "Added" : "Removed", In.Tag,
                bAdd ? "to" : "from", Out.Changed, Out.Changed == 1 ? "y" : "ies",
                Out.Skipped.IsEmpty() ? "" : Lumina::Format(" {} id(s) named nothing.", Out.Skipped.Num())));
        }

        void RegisterEntityTag(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SEntityTagParams, SEntityTagResult>(
                Owner, "entity.tag",
                "Add or Remove a tag on a batch of entities, as one undo step. An entity can carry several tags.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SEntityTagParams& In, SEntityTagResult& Out) { return EntityEditTags(In, Out); });
        }

        Agent::FToolResult EntitySetProperty(const SSetPropertyParams& In, SSetPropertyResult& Out)
        {
            ECS::FRegistry* ScenePtr = nullptr;
            ECS::FEntity Entity = ECS::NullEntity;
            FString Error;
            if (!ResolveSceneEntity(In.Entity, ScenePtr, Entity, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            ECS::FRegistry& Registry = *ScenePtr;
            CStruct* Reflected = nullptr;
            const FComponentOps* Ops = nullptr;
            void* Data = nullptr;

            CStruct* Type = SceneOps::ResolveComponentType(FStringView(In.Component));
            if (Type != nullptr && ECS::Utils::HasComponent(Registry, Entity, Type))
            {
                Reflected = Type;
                Ops = FindComponentOps(FStringView(Reflected->GetName().ToString()));
                Data = Ops != nullptr ? Ops->Get(Registry, Entity) : nullptr;
            }
            else if (CEntityScript* Script = FindEntityScriptByName(Registry, Entity, FStringView(In.Component)))
            {
                // A script's minted class carries real FPropertys, so the instance stands in for component data.
                Reflected = Script->GetClass();
                Data = Script;
            }
            else
            {
                return Agent::FToolResult::Error(Lumina::Format(
                    "'{}' has no component or script named '{}'.", SceneEntityName(Registry, Entity), In.Component));
            }

            if (Data == nullptr)
            {
                return Agent::FToolResult::Error(Lumina::Format("'{}' carries no data that can be edited.", In.Component));
            }

            Agent::FResolvedProperty Target;
            if (!Agent::ResolvePropertyPath(Reflected, Data, FStringView(In.Path), Target, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            FJsonValue Value;
            if (!FJsonValue::Parse(In.Value, Value))
            {
                return Agent::FToolResult::Error("Value is not JSON. Strings need quotes, so \"Torch\" rather than Torch.");
            }

            // The details panel never writes a read-only field, so an agent does not either.
            if (Target.Property->IsReadOnly())
            {
                return Agent::FToolResult::Error(Lumina::Format("'{}' is read-only, so it cannot be set directly.", In.Path));
            }

            if (!ResolveEntityTokenFields(Registry, Target.Property, Value, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            // Checked before any undo step opens, so a bad value leaves the history untouched.
            if (const Agent::FMarshalResult Check = Agent::ValidatePropertyValue(Value, Target.Property, FStringView(In.Path));
                !Check.IsValid())
            {
                return Agent::FToolResult::Error(Check.Error);
            }

            FJsonValue Before;
            Agent::WriteProperty(Target.Property, Target.ValuePtr, Before);
            Out.Previous = Before.ToString();

            Agent::FMarshalResult Applied;
            auto Apply = [&]()
            {
                Applied = Agent::ReadProperty(Value, Target.Property, Target.ValuePtr, FStringView(In.Path));
            };

            FString SceneError;
            // A script owns its own state outside any component record, so like the details panel it snapshots the scene.
            if (Ops == nullptr)
            {
                SessionOps::RunTransacted("Set Property (agent)", [&]()
                {
                    Apply();
                    CPrefab::RecaptureComponentOverrides(Registry, Entity, SEntityScriptComponent::StaticStruct());
                }, SceneError);
            }
            else
            {
                SessionOps::RunComponentTransacted("Set Property (agent)", Entity, Reflected, [&]()
                {
                    // A bare store reaches no hook, so the renderer keeps serving the old baked record.
                    SceneOps::FPropertyEditScope Edit(Registry, Entity, Reflected, Data, Target.Property, Target.ValuePtr);
                    Apply();
                }, SceneError);
            }

            if (!Applied.IsValid())
            {
                return Agent::FToolResult::Error(Applied.Error);
            }

            FJsonValue After;
            Agent::WriteProperty(Target.Property, Target.ValuePtr, After);
            Out.Current = After.ToString();

            return Agent::FToolResult::Ok(Lumina::Format("{}.{} is now {}.", In.Component, In.Path, Out.Current));
        }

        void RegisterEntitySetProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetPropertyParams, SSetPropertyResult>(
                Owner, "entity.set_property",
                "Set one field on one component or script of an entity, as one undo step.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetPropertyParams& In, SSetPropertyResult& Out) { return EntitySetProperty(In, Out); });
        }

        Agent::FToolResult EntityDuplicate(const SDuplicateEntityParams& In, SDuplicateEntityResult& Out)
        {
            FString SceneError;
            ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
            CWorld* World = SessionOps::GetSceneWorld(SceneError);
            if (ScenePtr == nullptr || World == nullptr)
            {
                return Agent::FToolResult::Error(SceneError);
            }

            if (SessionOps::IsSceneTargetSimulating())
            {
                return Agent::FToolResult::Error(GSceneStopPlayFirst);
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
                Copy = SessionOps::DuplicateEntity(Source);
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
        }

        void RegisterEntityDuplicate(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDuplicateEntityParams, SDuplicateEntityResult>(
                Owner, "entity.duplicate",
                "Duplicate an entity with its components, scripts and children, as the editor's Duplicate does, as one undo step.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SDuplicateEntityParams& In, SDuplicateEntityResult& Out) { return EntityDuplicate(In, Out); });
        }

        Agent::FToolResult EntitySetParent(const SSetParentParams& In, SSetParentResult& Out)
        {
            FString SceneError;
            ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
            if (ScenePtr == nullptr)
            {
                return Agent::FToolResult::Error(SceneError);
            }

            if (SessionOps::IsSceneTargetSimulating())
            {
                return Agent::FToolResult::Error(GSceneStopPlayFirst);
            }

            ECS::FRegistry& Registry = *ScenePtr;
            ECS::FEntity Child  = ECS::NullEntity;
            ECS::FEntity Parent = ECS::NullEntity;
            FString Error;
            if (!Agent::FEntityTokens::Resolve(Registry, FStringView(In.Entity), Child, Error))
            {
                return Agent::FToolResult::Error(Error);
            }
            if (!In.Parent.IsEmpty() && !Agent::FEntityTokens::Resolve(Registry, FStringView(In.Parent), Parent, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            // The outliner's own reparent, so a scene's rules, such as a prefab's single root, apply here too.
            if (!SessionOps::ReparentEntity(Child, Parent, In.bKeepWorldTransform, SceneError))
            {
                return Agent::FToolResult::Error(SceneError);
            }

            Out.bReparented = true;
            return Agent::FToolResult::Ok(In.Parent.IsEmpty()
                ? Lumina::Format("Moved {} to the world root.", In.Entity)
                : Lumina::Format("Parented {} under {}.", In.Entity, In.Parent));
        }

        void RegisterEntitySetParent(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetParentParams, SSetParentResult>(
                Owner, "entity.set_parent",
                "Parent an entity under another, or move it to the world root with an empty Parent, as one undo step.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetParentParams& In, SSetParentResult& Out) { return EntitySetParent(In, Out); });
        }

        Agent::FToolResult EntityDestroy(const SDestroyEntitiesParams& In, SDestroyEntitiesResult& Out)
        {
            FString SceneError;
            ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
            if (ScenePtr == nullptr)
            {
                return Agent::FToolResult::Error(SceneError);
            }

            if (SessionOps::IsSceneTargetSimulating())
            {
                return Agent::FToolResult::Error(GSceneStopPlayFirst);
            }

            ECS::FRegistry& Registry = *ScenePtr;
            CWorld* World = SessionOps::GetSceneWorld(SceneError);

            const TVector<ECS::FEntity> Doomed = ResolveSceneEntities(Registry, In.Entities, Out.Skipped);
            if (Doomed.IsEmpty())
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
                Out.Skipped.IsEmpty() ? "" : Lumina::Format(" {} id(s) named nothing.", Out.Skipped.Num())));
        }

        void RegisterEntityDestroy(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDestroyEntitiesParams, SDestroyEntitiesResult>(
                Owner, "entity.destroy",
                "Destroy entities and their children, as one undo step.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SDestroyEntitiesParams& In, SDestroyEntitiesResult& Out) { return EntityDestroy(In, Out); });
        }
    }

    using namespace MCPSceneToolsInternal;

    CWorld* FindGameWorld()
    {
        if (GWorldManager == nullptr)
        {
            return nullptr;
        }

        for (const TUniquePtr<FWorldContext>& Context : GWorldManager->GetContexts())
        {
            if (Context && Context->World.IsValid() && (Context->Type == EWorldType::Game || Context->Type == EWorldType::Simulation))
            {
                return Context->World.Get();
            }
        }
        return nullptr;
    }

    Agent::FToolResult SceneCameraSet(const SSceneCameraParams& In, SSceneCameraResult& Out)
    {
        if (In.Location.Num() != 3 || (In.Target.Num() != 3 && In.Rotation.Num() != 3))
        {
            return Agent::FToolResult::Error("Location is [x,y,z], plus a Target [x,y,z] or a Rotation [pitch,yaw,roll].");
        }

        FString Error;
        CWorld* World = SessionOps::GetSceneWorld(Error);
        if (World == nullptr)
        {
            return Agent::FToolResult::Error(Error.IsEmpty() ? FString("No world is open.") : Error);
        }

        const ECS::FEntity Camera = World->GetCamera()->GetActiveCameraEntity();
        STransformComponent* Transform = Camera != ECS::NullEntity && World->IsValidEntity(Camera)
            ? World->TryGetComponent<STransformComponent>(Camera) : nullptr;
        if (Transform == nullptr)
        {
            return Agent::FToolResult::Error("The editor world has no viewport camera.");
        }

        const FVector3 Location(In.Location[0], In.Location[1], In.Location[2]);
        const FQuat Rotation = In.Target.Num() == 3
            ? Math::FindLookAtRotation(FVector3(In.Target[0], In.Target[1], In.Target[2]), Location)
            : FQuat(Math::Radians(FVector3(In.Rotation[0], In.Rotation[1], In.Rotation[2])));
        Transform->SetLocation(Location);
        Transform->SetRotation(Rotation);

        const FVector3 Euler = Math::Degrees(Math::YawFirstEulerAngles(Rotation));
        Out.World = "Editor";
        Out.Location = { Location.x, Location.y, Location.z };
        Out.Rotation = { Euler.x, Euler.y, Euler.z };
        return Agent::FToolResult::Ok(Lumina::Format("Camera at [{:.2f},{:.2f},{:.2f}].", Location.x, Location.y, Location.z));
    }

    float GroundHeightAt(float X, float Z)
    {
        FString Error;
        CWorld* World = FindGameWorld();
        if (World == nullptr)
        {
            World = SessionOps::GetSceneWorld(Error);
        }
        return World != nullptr ? GroundHeightAt(World, X, Z) : GNoGroundHeight;
    }

    float GroundHeightAt(CWorld* World, float X, float Z)
    {
        const SRayResult Hit = World->GetPhysics()->Raycast(FVector3(X, 5000.0f, Z), FVector3(X, -5000.0f, Z), ECS::NullEntity);
        return Hit.bHit ? Hit.Location.y : GNoGroundHeight;
    }

    void RegisterSceneTools(FStringView Owner)
    {
        RegisterSceneFind(Owner);
        RegisterSceneSelect(Owner);
        RegisterSceneListSystems(Owner);
        RegisterSceneListTags(Owner);
        RegisterSceneListComponentTypes(Owner);
        RegisterSceneCreateEntity(Owner);
        RegisterSceneGroundHeight(Owner);
        RegisterSceneCamera(Owner);
        RegisterEntityDescribe(Owner);
        RegisterEntityComponent(Owner);
        RegisterEntityScript(Owner);
        RegisterEntityTag(Owner);
        RegisterEntitySetProperty(Owner);
        RegisterEntityDuplicate(Owner);
        RegisterEntitySetParent(Owner);
        RegisterEntityDestroy(Owner);
    }
}
