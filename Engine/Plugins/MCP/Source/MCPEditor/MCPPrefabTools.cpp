#include "MCPPrefabTools.h"
#include "World/Prefab/PrefabLibrary.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentEntityToken.h"
#include "Agent/AgentToolRegistry.h"
#include "Assets/AssetRef.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Session/SessionOps.h"
#include "MCPTextMatch.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/EntityUtils.h"
#include "World/World.h"

namespace Lumina::MCP
{
    namespace
    {
        // Class names in the registry are strings, so each distinct one is resolved once per call.
        bool IsPrefabClassName(const FName& ClassName, THashMap<FName, bool>& Cache)
        {
            const auto Found = Cache.find(ClassName);
            if (Found != Cache.end())
            {
                return Found->second;
            }

            CClass* Class = FindObject<CClass>(ClassName);
            const bool bPrefab = Class != nullptr && Class->IsChildOf(CPrefab::StaticClass());
            Cache.emplace(ClassName, bPrefab);
            return bPrefab;
        }

        void RegisterList(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListPrefabsParams, SListPrefabsResult>(
                Owner, "prefab.list",
                "List prefab assets, with the GUID prefab.spawn takes.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListPrefabsParams& In, SListPrefabsResult& Out)
                {
                    const int32 Limit = In.Limit > 0 ? In.Limit : 50;
                    const int32 Offset = In.Offset > 0 ? In.Offset : 0;

                    THashMap<FName, bool> ClassCache;

                    FAssetRegistry::Get().FindByPredicate([&](const FAssetData& Data)
                    {
                        if (!IsPrefabClassName(Data.AssetClass, ClassCache))
                        {
                            return false;
                        }

                        const FString Name(Data.AssetName.ToString().c_str());
                        const FString Path(Data.Path.c_str());
                        if (!ContainsTextFold(FStringView(Name), In.Contains) && !ContainsTextFold(FStringView(Path), In.Contains))
                        {
                            return false;
                        }

                        ++Out.Matched;

                        if (Out.Matched > Offset && static_cast<int32>(Out.Results.size()) < Limit)
                        {
                            SAssetInfo Info;
                            Info.Name       = Name;
                            Info.Path       = Path;
                            Info.AssetClass = FString(Data.AssetClass.ToString().c_str());
                            Info.Guid       = FString(Data.AssetGUID.ToString().c_str());
                            Out.Results.push_back(Move(Info));
                        }

                        return false;
                    });

                    return Agent::FToolResult::Ok(Lumina::Format("{} of {} matching prefab(s).",
                        Out.Results.size(), Out.Matched));
                });
        }

        void RegisterDescribePrefab(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDescribePrefabParams, SDescribePrefabResult>(
                Owner, "prefab.describe",
                "Report what a prefab contains: its root entities, their components, and any parent variant.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SDescribePrefabParams& In, SDescribePrefabResult& Out)
                {
                    CPrefab* Prefab = nullptr;
                    FString Error;
                    if (!Agent::ResolveAsset<CPrefab>(FStringView(In.Asset), Prefab, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Name  = FString(Prefab->GetName().ToString().c_str());
                    Out.ClassName = FString(Prefab->GetClass()->GetName().ToString().c_str());

                    if (CPackage* Package = Prefab->GetPackage())
                    {
                        Out.Path = FString(Package->GetPackagePath().c_str());
                    }

                    if (Prefab->ParentPrefab != nullptr)
                    {
                        Out.ParentPrefab = FString(Prefab->ParentPrefab->GetGUID().ToString().c_str());
                    }

                    ECS::FRegistry& Registry = Prefab->Registry;

                    Registry.ForEachEntity([&](ECS::FEntity Entity)
                    {
                        ++Out.EntityCount;

                        const FRelationshipComponent* Link = Registry.TryGet<FRelationshipComponent>(Entity);
                        if (Link != nullptr && Link->Parent != ECS::NullEntity)
                        {
                            return;
                        }

                        SPrefabEntityInfo Info;
                        if (const SNameComponent* Named = Registry.TryGet<SNameComponent>(Entity))
                        {
                            Info.Name = FString(Named->Name.ToString().c_str());
                        }

                        ForEachComponentStruct([&](CStruct* Reflected)
                        {
                            if (ECS::Utils::HasComponent(Registry, Entity, Reflected))
                            {
                                Info.Components.push_back(FString(Reflected->GetName().ToString().c_str()));
                            }
                        });

                        Out.RootEntities.push_back(Move(Info));
                    });

                    return Agent::FToolResult::Ok(Lumina::Format("'{}' holds {} entit{} with {} root(s).{}",
                        Out.Name, Out.EntityCount, Out.EntityCount == 1 ? "y" : "ies", Out.RootEntities.size(),
                        Out.ParentPrefab.empty() ? "" : " It is a variant."));
                });
        }

        void RegisterSpawn(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSpawnPrefabParams, SSpawnPrefabResult>(
                Owner, "prefab.spawn",
                "Place an instance of a prefab in the open world, as one undo step.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSpawnPrefabParams& In, SSpawnPrefabResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error("No world editor is open, so there is nowhere to spawn.");
                    }

                    if (SessionOps::IsSimulating())
                    {
                        return Agent::FToolResult::Error("Stop play-in-editor first.");
                    }

                    CPrefab* Prefab = nullptr;
                    FString Error;
                    if (!Agent::ResolveAsset<CPrefab>(FStringView(In.Asset), Prefab, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CWorld* World = SessionOps::GetSceneWorld(SceneError);
                    ECS::FRegistry& Registry = *ScenePtr;

                    ECS::FEntity Parent = ECS::NullEntity;
                    if (!In.Parent.empty() && !Agent::FEntityTokens::Resolve(Registry, FStringView(In.Parent), Parent, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FTransform Transform;
                    Transform.SetLocation(FVector3(In.Position.X, In.Position.Y, In.Position.Z));
                    Transform.SetRotationFromEuler(FVector3(In.Rotation.X, In.Rotation.Y, In.Rotation.Z));
                    Transform.SetScale(FVector3(In.Scale, In.Scale, In.Scale));

                    FAssetRef Ref;
                    Ref.Guid = In.Asset;
                    if (CPackage* Package = Prefab->GetPackage())
                    {
                        Ref.Path = FString(Package->GetPackagePath().c_str());
                    }

                    ECS::FEntity Root = ECS::NullEntity;
                    SessionOps::RunCreationTransacted("Spawn Prefab (agent)", [&]()
                    {
                        Root = CPrefabLibrary::SpawnPrefabAt(World, Ref, Transform, Parent);

                        if (Root != ECS::NullEntity && !In.Name.empty())
                        {
                            if (SNameComponent* Named = Registry.TryGet<SNameComponent>(Root))
                            {
                                Named->Name = FName(In.Name);
                            }
                        }
                    }, SceneError);

                    if (Root == ECS::NullEntity)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("'{}' could not be instantiated.", Prefab->GetName()));
                    }

                    Out.Entity = Agent::FEntityTokens::Mint(Registry, Root);
                    if (const SNameComponent* Named = Registry.TryGet<SNameComponent>(Root))
                    {
                        Out.Name = FString(Named->Name.ToString().c_str());
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Spawned '{}' as {}.", Out.Name, Out.Entity));
                });
        }

        // How you add components to a prefab through MCP: the entity tools only reach the main world, so
        // edit a placed instance there and write it back over the asset.
        void RegisterCapture(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCapturePrefabParams, SCapturePrefabResult>(
                Owner, "prefab.capture",
                "Overwrite a prefab asset with a placed instance's entities, save it, and refresh every other instance.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCapturePrefabParams& In, SCapturePrefabResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error("No world editor is open, so there is nothing to capture from.");
                    }

                    if (SessionOps::IsSimulating())
                    {
                        return Agent::FToolResult::Error("Stop play-in-editor first.");
                    }

                    CPrefab* Prefab = nullptr;
                    FString Error;
                    if (!Agent::ResolveAsset<CPrefab>(FStringView(In.Asset), Prefab, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    ECS::FRegistry& Registry = *ScenePtr;
                    ECS::FEntity Root = ECS::NullEntity;
                    if (!Agent::FEntityTokens::Resolve(Registry, FStringView(In.Entity), Root, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CWorld* World = SessionOps::GetSceneWorld(SceneError);
                    Prefab->CaptureFromWorld(World, Root);

                    // Anchor the captured root at origin so the prefab opens centered in its editor.
                    Prefab->Registry.ForEachEntity([&](ECS::FEntity E)
                    {
                        Out.EntityCount++;
                        const FRelationshipComponent* Rel = Prefab->Registry.TryGet<FRelationshipComponent>(E);
                        if (Rel != nullptr && Rel->Parent != ECS::NullEntity)
                        {
                            return;
                        }
                        if (STransformComponent* Tx = Prefab->Registry.TryGet<STransformComponent>(E))
                        {
                            Tx->SetLocalTransform(FTransform());
                        }
                    });

                    CPackage* Package = Prefab->GetPackage();
                    if (Package == nullptr)
                    {
                        return Agent::FToolResult::Error("That prefab has no package to save.");
                    }

                    const FFixedString Path = Package->GetPackagePath();
                    if (!CPackage::SavePackage(Package, Path))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not save {}.", Path));
                    }

                    FAssetRegistry::Get().AssetSaved(Prefab);
                    Prefab->RefreshInstancesInLoadedWorlds();

                    Out.Path = FString(Path.c_str());
                    return Agent::FToolResult::Ok(Lumina::Format("Captured {} into {}.", In.Entity, Out.Path));
                });
        }
    }

    void RegisterPrefabTools(FStringView Owner)
    {
        RegisterList(Owner);
        RegisterDescribePrefab(Owner);
        RegisterSpawn(Owner);
        RegisterCapture(Owner);
    }
}
