#include "MCPTerrainTools.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentEntityToken.h"
#include "Agent/AgentToolRegistry.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Object/Cast.h"
#include "Session/SessionOps.h"
#include "Tools/Import/ImportHelpers.h"
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
    namespace MCPTerrainToolsInternal
    {
        constexpr int32 GTerrainMinResolution = 33;
        constexpr int32 GTerrainMaxResolution = 8193;
        static_assert(int64(GTerrainMaxResolution) * GTerrainMaxResolution * GTerrainMaxLayers <= INT32_MAX, "Layer weights must stay int32-indexable.");

        bool IsValidTerrainResolution(int32 Value)
        {
            if (Value < GTerrainMinResolution || Value > GTerrainMaxResolution)
            {
                return false;
            }
            const int32 Quads = Value - 1;
            return (Quads & (Quads - 1)) == 0;
        }

        // Nearest-sampled onto the terrain grid, so any image size works and a matching size is exact.
        bool ImportTerrainLayerWeights(STerrainComponent& Terrain, const TVector<FString>& Paths, FString& OutError)
        {
            if (Paths.IsEmpty())
            {
                return true;
            }

            while (Terrain.Layers.Num() < Paths.Num())
            {
                Terrain.Layers.EmplaceGetRef().Name = Lumina::Format("Layer {}", Terrain.Layers.Num());
            }

            const int32 Res = Terrain.Resolution;
            const size_t Samples = (size_t)Res * (size_t)Res;
            Terrain.LayerWeights.Init(uint8(0), IntCastChecked<int32>(Samples * Terrain.Layers.Num()));

            for (int32 Layer = 0; Layer < Paths.Num(); ++Layer)
            {
                TOptional<Import::Textures::FTextureImportResult> Image = Import::Textures::ImportTexture(FStringView(Paths[Layer]), false);
                if (!Image.IsSet() || Image->Dimensions.x == 0 || Image->Dimensions.y == 0)
                {
                    OutError = Lumina::Format("Could not read the weightmap '{}'.", Paths[Layer]);
                    return false;
                }

                const uint32 Width = Image->Dimensions.x;
                const uint32 Height = Image->Dimensions.y;
                const int64 TexelBytes = Image->Pixels.Num() / (static_cast<int64>(Width) * Height);
                const bool b16Bit = Image->Format == EFormat::R16_UNORM || Image->Format == EFormat::RG16_UNORM || Image->Format == EFormat::RGBA16_UNORM;
                if (TexelBytes == 0)
                {
                    OutError = Lumina::Format("'{}' decoded to no pixels.", Paths[Layer]);
                    return false;
                }

                // Little-endian, so a 16-bit sample's high byte is the second one.
                const int64 ByteOffset = b16Bit ? 1 : 0;
                uint8* Out = Terrain.LayerWeights.GetData() + Layer * Samples;
                for (int32 Z = 0; Z < Res; ++Z)
                {
                    const uint32 SrcY = Math::Min((uint32)((uint64)Z * Height / Res), Height - 1);
                    for (int32 X = 0; X < Res; ++X)
                    {
                        const uint32 SrcX = Math::Min((uint32)((uint64)X * Width / Res), Width - 1);
                        Out[Z * Res + X] = Image->Pixels[(static_cast<int64>(SrcY) * Width + SrcX) * TexelBytes + ByteOffset];
                    }
                }
            }

            Terrain.CPUState.bFullWeightsDirty = true;
            return true;
        }

        // Little-endian stream over a whole file, which reports overruns instead of reading past the end.
        class FFoliageFileReader
        {
        public:
            explicit FFoliageFileReader(const TVector<uint8>& InData) : Data(InData) {}

            template<typename T>
            bool Read(T& Out)
            {
                if (Offset + sizeof(T) > static_cast<size_t>(Data.Num()))
                {
                    return false;
                }
                Memory::Memcpy(&Out, Data.GetData() + Offset, sizeof(T));
                Offset += sizeof(T);
                return true;
            }

            bool ReadString(FString& Out, uint16 Length)
            {
                if (Offset + Length > static_cast<size_t>(Data.Num()))
                {
                    return false;
                }
                Out = FStringView(reinterpret_cast<const char*>(Data.GetData()) + Offset, Length);
                Offset += Length;
                return true;
            }

            size_t Remaining() const { return static_cast<size_t>(Data.Num()) - Offset; }

        private:
            const TVector<uint8>& Data;
            size_t Offset = 0;
        };

        constexpr uint32 GFoliageFileMagic   = 0x4C4F464Cu;
        constexpr uint32 GFoliageFileVersion = 1u;
        constexpr int32  GFoliageInstanceFloats = 10;
        constexpr uint64 GFoliageTypeMinBytes   = sizeof(uint16) + sizeof(uint8) + sizeof(float);
        constexpr uint64 GFoliageInstanceBytes  = GFoliageInstanceFloats * sizeof(float) + sizeof(uint32);

        void RegisterTerrainCreate(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateTerrainParams, SCreateTerrainResult>(
                Owner, "terrain.create",
                "Create a terrain entity from a heightmap image, sized and placed in world units.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateTerrainParams& In, SCreateTerrainResult& Out)
                {
                    if (!IsValidTerrainResolution(In.Resolution))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Resolution must be a power of two plus one, from {} to {}.",
                            GTerrainMinResolution, GTerrainMaxResolution));
                    }
                    if (In.LayerWeightmapPaths.Num() > (int32)GTerrainMaxLayers)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("LayerWeightmapPaths has {} entries; terrain supports at most {} layers.",
                            In.LayerWeightmapPaths.Num(), GTerrainMaxLayers));
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

                        if (SNameComponent* Name = World->TryGetComponent<SNameComponent>(Created); Name != nullptr && !In.Name.IsEmpty())
                        {
                            Name->Name = FName(*In.Name);
                        }
                        World->SetEntityLocation(Created, FVector3(In.Center.X, In.Center.Y, In.Center.Z));

                        STerrainComponent& Terrain = World->GetComponent<STerrainComponent>(Created);
                        Terrain.Resolution    = In.Resolution;
                        Terrain.TileWorldSize = In.TileWorldSize;
                        Terrain.MaxHeight     = In.MaxHeight;

                        // Weights are sized to the grid, and the import only replaces heights.
                        const size_t Samples = (size_t)In.Resolution * (size_t)In.Resolution;
                        Terrain.LayerWeights.Init(uint8(0), IntCastChecked<int32>(Samples * Terrain.Layers.Num()));
                        Terrain.CPUState.bFullWeightsDirty = true;

                        if (!FTerrainEditMode::ImportHeightmap(Terrain, *In.HeightmapPath, ImportError, Out.SourceWidth, Out.SourceHeight)
                            || !ImportTerrainLayerWeights(Terrain, In.LayerWeightmapPaths, ImportError))
                        {
                            // Destroyed inside the transaction, so it sees no net creation and records no undo step.
                            World->DestroyEntity(Created);
                            return;
                        }

                        if (In.bCollision)
                        {
                            World->EmplaceComponent<STerrainColliderComponent>(Created);

                            // Editor worlds have no physics scene to add the body, and without one the saved terrain never collides.
                            World->GetOrEmplaceComponent<SRigidBodyComponent>(Created).BodyType = EBodyType::Static;
                        }
                    }, SceneError);

                    if (!ImportError.IsEmpty())
                    {
                        return Agent::FToolResult::Error(ImportError);
                    }
                    if (Created == ECS::NullEntity)
                    {
                        return Agent::FToolResult::Error(SceneError.IsEmpty() ? FString("The world refused to create the terrain.") : SceneError);
                    }

                    Out.Entity = Agent::FEntityTokens::Mint(*Registry, Created);
                    return Agent::FToolResult::Ok(Lumina::Format("Created terrain '{}' from a {}x{} heightmap.",
                        In.Name, Out.SourceWidth, Out.SourceHeight));
                });
        }

        void RegisterTerrainImportFoliage(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SImportFoliageParams, SImportFoliageResult>(
                Owner, "terrain.import_foliage",
                "Create a foliage entity from a LFOL file of mesh types and world-space instances.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SImportFoliageParams& In, SImportFoliageResult& Out)
                {
                    TVector<uint8> Bytes;
                    if (!Filesystem::ReadFile(Bytes, FStringView(In.Path)))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not read '{}'.", In.Path));
                    }

                    FFoliageFileReader Reader(Bytes);
                    uint32 Magic = 0, Version = 0, TypeCount = 0, InstanceCount = 0;
                    if (!Reader.Read(Magic) || !Reader.Read(Version) || !Reader.Read(TypeCount) || !Reader.Read(InstanceCount)
                        || Magic != GFoliageFileMagic || Version != GFoliageFileVersion)
                    {
                        return Agent::FToolResult::Error("Not a version 1 LFOL file.");
                    }
                    if (TypeCount * GFoliageTypeMinBytes + InstanceCount * GFoliageInstanceBytes > Reader.Remaining())
                    {
                        return Agent::FToolResult::Error("The file ends early for the type and instance counts in its header.");
                    }

                    FString SceneError;
                    ECS::FRegistry* Registry = SessionOps::GetSceneRegistry(SceneError);
                    CWorld* World = SessionOps::GetSceneWorld(SceneError);
                    if (Registry == nullptr || World == nullptr)
                    {
                        return Agent::FToolResult::Error(SceneError);
                    }

                    TVector<SFoliageType> Types;
                    TVector<int32> Remap(TypeCount, Constants::kIndexNone);
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
                            Out.MissingMeshes.Add(MeshPath);
                            continue;
                        }

                        Remap[t] = Types.Num();
                        SFoliageType& Type = Types.EmplaceGetRef();
                        Type.Name           = Mesh->GetName().ToString();
                        Type.Mesh           = Mesh;
                        Type.bCastShadow    = bCastShadow != 0;
                        Type.CullDistance   = CullDistance;
                        Type.bFollowTerrain = false;
                        Type.bRandomYaw     = false;
                    }

                    TVector<SFoliageInstance> Instances;
                    Instances.Reserve(InstanceCount);
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
                        if (TypeIndex >= TypeCount || Remap[TypeIndex] == Constants::kIndexNone)
                        {
                            continue;
                        }

                        SFoliageInstance& Instance = Instances.EmplaceGetRef();
                        Instance.Position  = FVector3(V[0], V[1], V[2]);
                        Instance.Rotation  = FVector4(V[3], V[4], V[5], V[6]);
                        Instance.Scale     = FVector3(V[7], V[8], V[9]);
                        Instance.TypeIndex = Remap[TypeIndex];
                    }

                    const FName EntityName(In.Name.IsEmpty() ? "Foliage" : *In.Name);
                    TVector<ECS::FEntity> Stale;
                    World->View<SFoliageComponent>().ForEach([&](ECS::FEntity Entity, SFoliageComponent&)
                    {
                        const SNameComponent* Name = World->TryGetComponent<SNameComponent>(Entity);
                        if (Name != nullptr && Name->Name == EntityName)
                        {
                            Stale.Add(Entity);
                        }
                    });

                    // A creation transaction cannot restore what it destroys, so the replaced foliage gets its own undo step.
                    if (!Stale.IsEmpty())
                    {
                        SessionOps::RunDestroyTransacted("Replace Foliage (agent)", Stale, [&]()
                        {
                            for (ECS::FEntity Entity : Stale)
                            {
                                World->DestroyEntity(Entity);
                            }
                        }, SceneError);
                    }

                    ECS::FEntity Created = ECS::NullEntity;

                    // Transacted like any other creation, so the level is marked dirty and the import survives a save.
                    SessionOps::RunCreationTransacted("Import Foliage (agent)", [&]()
                    {
                        Created = World->ConstructEntity(EntityName);
                        SFoliageComponent& Foliage = World->EmplaceComponent<SFoliageComponent>(Created);
                        Foliage.Types     = Move(Types);
                        Foliage.Instances = Move(Instances);
                        MarkFoliageChanged(*World, Created, Foliage);
                    }, SceneError);

                    if (Created == ECS::NullEntity)
                    {
                        return Agent::FToolResult::Error(SceneError.IsEmpty() ? FString("The world refused to create the foliage.") : SceneError);
                    }

                    const SFoliageComponent& Foliage = World->GetComponent<SFoliageComponent>(Created);
                    Out.Entity    = Agent::FEntityTokens::Mint(*Registry, Created);
                    Out.Types     = Foliage.Types.Num();
                    Out.Instances = Foliage.Instances.Num();
                    return Agent::FToolResult::Ok(Lumina::Format("Imported {} instances of {} meshes into '{}'; {} mesh path(s) did not resolve.",
                        Out.Instances, Out.Types, In.Name, Out.MissingMeshes.Num()));
                });
        }
    }

    using namespace MCPTerrainToolsInternal;

    void RegisterTerrainTools(FStringView Owner)
    {
        RegisterTerrainCreate(Owner);
        RegisterTerrainImportFoliage(Owner);
    }
}
