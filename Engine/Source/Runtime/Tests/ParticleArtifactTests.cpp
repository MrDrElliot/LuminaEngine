#include <gtest/gtest.h>
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"

using namespace Lumina;

namespace
{
    constexpr const char* kArtifactAlias = "/PkgParticleArtifact";

    class ParticleArtifact : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite()
        {
            RegisterPendingTypes();
            VFS::Mount<VFS::FMemoryFileSystem>(kArtifactAlias);
            CParticleEmitter::StaticClass()->Link();
        }
    };

    void UnloadPackage(CPackage* Package)
    {
        TVector<CObject*> Objects;
        GetObjectsWithPackage(Package, Objects);
        for (CObject* Object : Objects)
        {
            if (Object != Package)
            {
                Object->ConditionalBeginDestroy();
            }
        }
        Package->ExportTable.Reset();
        Package->ImportTable.Reset();

        // The root set holds the package's only strong reference, so unrooting is what destroys it.
        Package->RemoveFromRoot();
    }
}

// An editor save leaves the bytecode to the artifact cache, while the emitter in memory keeps simulating with it.
TEST_F(ParticleArtifact, EditorSaveLeavesBinariesOutOfTheAsset)
{
    const char* Path = "/PkgParticleArtifact/Saved.lasset";
    const FGuid Guid = FGuid::New();
    const TVector<uint32> Binaries = { 0x07230203u, 1u, 2u, 3u };

    CPackage* Package = CPackage::CreatePackage(Path);
    ASSERT_NE(Package, nullptr);
    CParticleEmitter* Emitter = Cast<CParticleEmitter>(NewObject(CParticleEmitter::StaticClass(), Package, "ArtifactEmitter", Guid));
    ASSERT_NE(Emitter, nullptr);
    Emitter->ShaderMode            = EParticleShaderMode::Custom;
    Emitter->ComputeShaderBinaries = Binaries;
    Emitter->CompiledCodeHash      = 0x1234u;

    ASSERT_TRUE(CPackage::SavePackage(Package, Path));
    EXPECT_EQ(Emitter->ComputeShaderBinaries, Binaries);

    UnloadPackage(Package);

    CPackage* Loaded = CPackage::LoadPackage(Path);
    ASSERT_NE(Loaded, nullptr);
    ASSERT_TRUE(Loaded->FullyLoad());

    CParticleEmitter* Read = Cast<CParticleEmitter>(FindObject<CObject>(Guid));
    ASSERT_NE(Read, nullptr);
    EXPECT_TRUE(Read->ComputeShaderBinaries.IsEmpty());
    EXPECT_EQ(Read->CompiledCodeHash, 0x1234u);
}

// Without a cache entry the emitter does not run a stale shader, and asks the editor for a recompile instead.
TEST_F(ParticleArtifact, MissingCacheEntryLeavesTheEmitterWaitingForACompile)
{
    const char* Path = "/PkgParticleArtifact/Missing.lasset";
    const FGuid Guid = FGuid::New();

    CPackage* Package = CPackage::CreatePackage(Path);
    ASSERT_NE(Package, nullptr);
    CParticleEmitter* Emitter = Cast<CParticleEmitter>(NewObject(CParticleEmitter::StaticClass(), Package, "WaitingEmitter", Guid));
    ASSERT_NE(Emitter, nullptr);
    Emitter->ShaderMode            = EParticleShaderMode::Custom;
    Emitter->ComputeShaderBinaries = { 0x07230203u, 1u };
    Emitter->CompiledCodeHash      = 0x5678u;
    Emitter->MaxParticles          = 64;

    ASSERT_TRUE(CPackage::SavePackage(Package, Path));
    UnloadPackage(Package);

    CPackage* Loaded = CPackage::LoadPackage(Path);
    ASSERT_NE(Loaded, nullptr);
    ASSERT_TRUE(Loaded->FullyLoad());

    CParticleEmitter* Read = Cast<CParticleEmitter>(FindObject<CObject>(Guid));
    ASSERT_NE(Read, nullptr);
    EXPECT_FALSE(Read->HasCustomComputeShader());
    EXPECT_TRUE(Read->NeedsShaderCompile());
    EXPECT_FALSE(Read->IsReadyForSimulation());
}

// An emitter on the built-in simulation has nothing to compile and is never queued.
TEST_F(ParticleArtifact, BuiltInSimulationNeverNeedsACompile)
{
    CParticleEmitter* Emitter = NewObject<CParticleEmitter>();
    Emitter->ShaderMode = EParticleShaderMode::Default;
    EXPECT_FALSE(Emitter->NeedsShaderCompile());
}
