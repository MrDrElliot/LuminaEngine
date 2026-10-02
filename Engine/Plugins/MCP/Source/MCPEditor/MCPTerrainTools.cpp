#include "MCPTerrainTools.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentEntityToken.h"
#include "Agent/AgentToolRegistry.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Object/Cast.h"
#include "Session/SessionOps.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "UI/Tools/TerrainEditMode.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/World.h"

namespace Lumina::MCP
{
    namespace
    {
        bool IsPowerOfTwoPlusOne(int32 Value)
        {
            const int32 Quads = Value - 1;
            return Quads >= 32 && (Quads & (Quads - 1)) == 0;
        }

        void RegisterCreateTerrain(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateTerrainParams, SCreateTerrainResult>(
                Owner, "terrain.create",
                "Create a terrain entity from a heightmap image, sized and placed in world units.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateTerrainParams& In, SCreateTerrainResult& Out)
                {
                    if (!IsPowerOfTwoPlusOne(In.Resolution))
                    {
                        return Agent::FToolResult::Error("Resolution must be a power of two plus one, at least 33.");
                    }
                    if (In.TileWorldSize <= 0.0f || In.MaxHeight <= 0.0f)
                    {
                        return Agent::FToolResult::Error("TileWorldSize and MaxHeight must be positive.");
                    }

                    FString SceneError;
                    ECS::FRegistry* Registry = SessionOps::GetSceneRegistry(SceneError);
                    CWorld* World = SessionOps::GetSceneWorld(SceneError);
                    if (Registry == nullptr || World == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    ECS::FEntity Created = ECS::NullEntity;
                    FString ImportError;

                    SessionOps::RunCreationTransacted("Create Terrain (agent)", [&]()
                    {
                        Created = FTerrainEditMode::CreateDefaultTerrain(World);
                        if (Created == ECS::NullEntity)
                        {
                            return;
                        }

                        if (SNameComponent* Name = World->TryGetComponent<SNameComponent>(Created); Name != nullptr && !In.Name.empty())
                        {
                            Name->Name = FName(In.Name.c_str());
                        }
                        World->SetEntityLocation(Created, FVector3(In.Center.X, In.Center.Y, In.Center.Z));

                        STerrainComponent& Terrain = World->GetComponent<STerrainComponent>(Created);
                        Terrain.Resolution    = In.Resolution;
                        Terrain.TileWorldSize = In.TileWorldSize;
                        Terrain.MaxHeight     = In.MaxHeight;

                        // Weights are sized to the grid, and the import only replaces heights.
                        const size_t Samples = (size_t)In.Resolution * (size_t)In.Resolution;
                        Terrain.LayerWeights.assign(Samples * Terrain.Layers.size(), uint8(0));
                        Terrain.CPUState.bFullWeightsDirty = true;

                        if (!FTerrainEditMode::ImportHeightmap(Terrain, In.HeightmapPath.c_str(), ImportError,
                                                               Out.SourceWidth, Out.SourceHeight))
                        {
                            return;
                        }

                        if (In.bCollision)
                        {
                            World->EmplaceComponent<STerrainColliderComponent>(Created);
                        }
                    }, SceneError);

                    if (Created == ECS::NullEntity)
                    {
                        return Agent::FToolResult::Error(SceneError.empty() ? FString("The world refused to create the terrain.") : SceneError);
                    }
                    if (!ImportError.empty())
                    {
                        World->DestroyEntity(Created);
                        return Agent::FToolResult::Error(ImportError);
                    }

                    Out.Entity = Agent::FEntityTokens::Mint(*Registry, Created);
                    return Agent::FToolResult::Ok(Lumina::Format("Created terrain '{}' from a {}x{} heightmap.",
                        In.Name, Out.SourceWidth, Out.SourceHeight));
                });
        }

        // Little-endian stream over a whole file, which reports overruns instead of reading past the end.
        class FByteReader
        {
        public:
            explicit FByteReader(const TVector<uint8>& InData) : Data(InData) {}

            template<typename T>
            bool Read(T& Out)
            {
                if (Offset + sizeof(T) > Data.size())
                {
                    return false;
                }
                Memory::Memcpy(&Out, Data.data() + Offset, sizeof(T));
                Offset += sizeof(T);
                return true;
            }

            bool ReadString(FString& Out, uint16 Length)
            {
                if (Offset + Length > Data.size())
                {
                    return false;
                }
                Out.assign(reinterpret_cast<const char*>(Data.data()) + Offset, Length);
                Offset += Length;
                return true;
            }

        private:
            const TVector<uint8>& Data;
            size_t Offset = 0;
        };

        constexpr uint32 GFoliageFileMagic   = 0x4C4F464Cu;
        constexpr uint32 GFoliageFileVersion = 1u;
        constexpr int32  GFoliageInstanceFloats = 10;

        void RegisterImportFoliage(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SImportFoliageParams, SImportFoliageResult>(
                Owner, "foliage.import",
                "Create a foliage entity from a LFOL file of mesh types and world-space instances.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SImportFoliageParams& In, SImportFoliageResult& Out)
                {
                    TVector<uint8> Bytes;
                    if (!Filesystem::ReadFile(Bytes, FStringView(In.Path)))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not read '{}'.", In.Path));
                    }

                    FByteReader Reader(Bytes);
                    uint32 Magic = 0, Version = 0, TypeCount = 0, InstanceCount = 0;
                    if (!Reader.Read(Magic) || !Reader.Read(Version) || !Reader.Read(TypeCount) || !Reader.Read(InstanceCount)
                        || Magic != GFoliageFileMagic || Version != GFoliageFileVersion)
                    {
                        return Agent::FToolResult::Error("Not a version 1 LFOL file.");
                    }

                    FString SceneError;
                    ECS::FRegistry* Registry = SessionOps::GetSceneRegistry(SceneError);
                    CWorld* World = SessionOps::GetSceneWorld(SceneError);
                    if (Registry == nullptr || World == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    TVector<SFoliageType> Types;
                    TVector<int32> Remap(TypeCount, INDEX_NONE);
                    for (uint32 t = 0; t < TypeCount; ++t)
                    {
                        uint16 Length = 0;
                        FString MeshPath;
                        uint8 bCastShadow = 1;
                        float CullDistance = 0.0f;
                        if (!Reader.Read(Length) || !Reader.ReadString(MeshPath, Length) || !Reader.Read(bCastShadow) || !Reader.Read(CullDistance))
                        {
                            return Agent::FToolResult::Error("The type table ends early.");
                        }

                        CObject* Object = nullptr;
                        FString Ignored;
                        CStaticMesh* Mesh = Agent::ResolveAssetObject(FStringView(MeshPath), Object, Ignored) ? Cast<CStaticMesh>(Object) : nullptr;
                        if (Mesh == nullptr)
                        {
                            Out.MissingMeshes.push_back(MeshPath);
                            continue;
                        }

                        Remap[t] = (int32)Types.size();
                        SFoliageType& Type = Types.emplace_back();
                        Type.Name           = Mesh->GetName().ToString();
                        Type.Mesh           = Mesh;
                        Type.bCastShadow    = bCastShadow != 0;
                        Type.CullDistance   = CullDistance;
                        Type.bFollowTerrain = false;
                        Type.bRandomYaw     = false;
                    }

                    TVector<SFoliageInstance> Instances;
                    Instances.reserve(InstanceCount);
                    for (uint32 i = 0; i < InstanceCount; ++i)
                    {
                        float V[GFoliageInstanceFloats];
                        uint32 TypeIndex = 0;
                        for (float& Component : V)
                        {
                            if (!Reader.Read(Component))
                            {
                                return Agent::FToolResult::Error("The instance table ends early.");
                            }
                        }
                        if (!Reader.Read(TypeIndex))
                        {
                            return Agent::FToolResult::Error("The instance table ends early.");
                        }
                        if (TypeIndex >= TypeCount || Remap[TypeIndex] == INDEX_NONE)
                        {
                            continue;
                        }

                        SFoliageInstance& Instance = Instances.emplace_back();
                        Instance.Position  = FVector3(V[0], V[1], V[2]);
                        Instance.Rotation  = FVector4(V[3], V[4], V[5], V[6]);
                        Instance.Scale     = FVector3(V[7], V[8], V[9]);
                        Instance.TypeIndex = Remap[TypeIndex];
                    }

                    const FName EntityName(In.Name.empty() ? "Foliage" : In.Name.c_str());
                    TVector<ECS::FEntity> Stale;
                    World->View<SFoliageComponent>().ForEach([&](ECS::FEntity Entity, SFoliageComponent&)
                    {
                        const SNameComponent* Name = World->TryGetComponent<SNameComponent>(Entity);
                        if (Name != nullptr && Name->Name == EntityName)
                        {
                            Stale.push_back(Entity);
                        }
                    });
                    for (ECS::FEntity Entity : Stale)
                    {
                        World->DestroyEntity(Entity);
                    }

                    const ECS::FEntity Created = World->ConstructEntity(EntityName);
                    SFoliageComponent& Foliage = World->EmplaceComponent<SFoliageComponent>(Created);
                    Foliage.Types     = Move(Types);
                    Foliage.Instances = Move(Instances);
                    MarkFoliageChanged(*World, Created, Foliage);

                    Out.Entity    = Agent::FEntityTokens::Mint(*Registry, Created);
                    Out.Types     = (int32)Foliage.Types.size();
                    Out.Instances = (int32)Foliage.Instances.size();
                    return Agent::FToolResult::Ok(Lumina::Format("Imported {} instances of {} meshes into '{}'; {} mesh path(s) did not resolve.",
                        Out.Instances, Out.Types, In.Name, Out.MissingMeshes.size()));
                });
        }
    }

    void RegisterTerrainTools(FStringView Owner)
    {
        RegisterCreateTerrain(Owner);
        RegisterImportFoliage(Owner);
    }
}
