#include "gtest/gtest.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Object/ObjectCore.h"
#include "World/Entity/Components/ParticleSystemComponent.h"

// GetRenderMaterial is the gate between the sprite texture path and the Particle material path.

namespace Lumina
{
    namespace
    {
        CMaterial* MakeMaterial(EMaterialType Type)
        {
            CMaterial* Material = NewObject<CMaterial>();
            Material->MaterialType = Type;
            Material->SetReadyForRender(true);
            Material->SetMaterialIndex(3);
            return Material;
        }

        CParticleEmitter* MakeEmitter()
        {
            return NewObject<CParticleEmitter>();
        }
    }

    TEST(ParticleMaterial, NoMaterialLeavesTheEmitterOnTheTexturePath)
    {
        CParticleEmitter* Emitter = MakeEmitter();
        EXPECT_EQ(Emitter->GetRenderMaterial(), nullptr);
    }

    TEST(ParticleMaterial, ParticleDomainMaterialIsAccepted)
    {
        CParticleEmitter* Emitter = MakeEmitter();
        CMaterial* Material = MakeMaterial(EMaterialType::Particle);
        Emitter->Material = Material;

        EXPECT_EQ(Emitter->GetRenderMaterial(), Material);
    }

    // A surface material dropped on an emitter has no sprite stages, so it must not reach the draw.
    TEST(ParticleMaterial, OtherDomainsAreRejected)
    {
        const EMaterialType Rejected[] =
        {
            EMaterialType::None, EMaterialType::PBR, EMaterialType::PostProcess,
            EMaterialType::UI,   EMaterialType::Terrain, EMaterialType::Decal,
        };

        for (EMaterialType Type : Rejected)
        {
            CParticleEmitter* Emitter = MakeEmitter();
            Emitter->Material = MakeMaterial(Type);
            EXPECT_EQ(Emitter->GetRenderMaterial(), nullptr) << "domain " << (int)Type;
        }
    }

    // A still-compiling material has no committed stages, so the emitter falls back for those frames.
    TEST(ParticleMaterial, NotReadyForRenderIsRejected)
    {
        CParticleEmitter* Emitter = MakeEmitter();
        CMaterial* Material = MakeMaterial(EMaterialType::Particle);
        Material->SetReadyForRender(false);
        Emitter->Material = Material;

        EXPECT_EQ(Emitter->GetRenderMaterial(), nullptr);
    }

    // Without a table slot the pixel stage would index another material's uniforms.
    TEST(ParticleMaterial, MissingMaterialSlotIsRejected)
    {
        CParticleEmitter* Emitter = MakeEmitter();
        CMaterial* Material = MakeMaterial(EMaterialType::Particle);
        Material->SetMaterialIndex(-1);
        Emitter->Material = Material;

        EXPECT_EQ(Emitter->GetRenderMaterial(), nullptr);
    }

    // The domain lives on the master, and an instance is how a particle effect gets per-use parameters.
    TEST(ParticleMaterial, InstanceOfAParticleMasterIsAccepted)
    {
        CMaterial* Master = MakeMaterial(EMaterialType::Particle);

        CMaterialInstance* Instance = NewObject<CMaterialInstance>();
        Instance->Material = Master;
        Instance->SetReadyForRender(true);
        Instance->SetMaterialIndex(7);

        CParticleEmitter* Emitter = MakeEmitter();
        Emitter->Material = Instance;

        // The instance itself, since it owns the uniform slot the stages read.
        EXPECT_EQ(Emitter->GetRenderMaterial(), Instance);
    }

    TEST(ParticleMaterial, InstanceOfASurfaceMasterIsRejected)
    {
        CMaterial* Master = MakeMaterial(EMaterialType::PBR);

        CMaterialInstance* Instance = NewObject<CMaterialInstance>();
        Instance->Material = Master;
        Instance->SetReadyForRender(true);
        Instance->SetMaterialIndex(7);

        CParticleEmitter* Emitter = MakeEmitter();
        Emitter->Material = Instance;

        EXPECT_EQ(Emitter->GetRenderMaterial(), nullptr);
    }

    // Per-component texture and scalar writes need a material only this component owns.

    namespace
    {
        CParticleSystem* MakeSystemWithMaterial(CMaterialInterface* EmitterMaterial)
        {
            CParticleSystem* System = NewObject<CParticleSystem>();
            System->AddEmitter()->Material = EmitterMaterial;
            return System;
        }
    }

    TEST(ParticleComponentMaterial, WithNoOverrideTheAssetMaterialIsUsed)
    {
        CMaterial* Authored = MakeMaterial(EMaterialType::Particle);

        SParticleSystemComponent Component;
        Component.ParticleSystem = MakeSystemWithMaterial(Authored);

        EXPECT_EQ(Component.GetMaterialForEmitter(0), Authored);
    }

    TEST(ParticleComponentMaterial, AnOverrideBeatsTheAssetMaterial)
    {
        CMaterial* Authored = MakeMaterial(EMaterialType::Particle);
        CMaterial* Override = MakeMaterial(EMaterialType::Particle);

        SParticleSystemComponent Component;
        Component.ParticleSystem = MakeSystemWithMaterial(Authored);
        Component.SetMaterialForEmitter(Override, 0);

        EXPECT_EQ(Component.GetMaterialForEmitter(0), Override);
        // The asset is shared by every component using it, so an override must not write through to it.
        EXPECT_EQ(Component.ParticleSystem->Emitters[0]->Material.Get(), Authored);
    }

    // Nothing sizes the array, so the setter has to grow it rather than index or append.
    TEST(ParticleComponentMaterial, SettingAnEmitterPastTheEndGrowsToCoverIt)
    {
        CMaterial* Override = MakeMaterial(EMaterialType::Particle);

        SParticleSystemComponent Component;
        Component.SetMaterialForEmitter(Override, 3);

        EXPECT_EQ(Component.MaterialOverrides.size(), 4u);
        EXPECT_EQ(Component.GetMaterialForEmitter(3), Override);
        EXPECT_EQ(Component.GetMaterialForEmitter(0), nullptr);
    }

    TEST(ParticleComponentMaterial, ClearingAnOverrideFallsBackToTheAsset)
    {
        CMaterial* Authored = MakeMaterial(EMaterialType::Particle);
        CMaterial* Override = MakeMaterial(EMaterialType::Particle);

        SParticleSystemComponent Component;
        Component.ParticleSystem = MakeSystemWithMaterial(Authored);
        Component.SetMaterialForEmitter(Override, 0);
        Component.SetMaterialForEmitter(nullptr, 0);

        EXPECT_EQ(Component.GetMaterialForEmitter(0), Authored);
    }

    TEST(ParticleComponentMaterial, AnEmitterWithNoMaterialHasNothingToInstance)
    {
        SParticleSystemComponent Component;
        Component.ParticleSystem = MakeSystemWithMaterial(nullptr);

        EXPECT_EQ(Component.CreateDynamicMaterialInstance(0), nullptr);
        EXPECT_TRUE(Component.MaterialOverrides.empty());
    }

    TEST(ParticleComponentMaterial, ADynamicInstanceIsInstalledOverTheEmitterAndLeavesTheAssetAlone)
    {
        CMaterial* Authored = MakeMaterial(EMaterialType::Particle);

        SParticleSystemComponent Component;
        Component.ParticleSystem = MakeSystemWithMaterial(Authored);

        CMaterialInstance* Dynamic = Component.CreateDynamicMaterialInstance(0);
        ASSERT_NE(Dynamic, nullptr);
        EXPECT_FALSE(Dynamic->IsAsset());
        EXPECT_EQ(Dynamic->GetMaterial(), Authored);
        EXPECT_EQ(Component.GetMaterialForEmitter(0), Dynamic);
        EXPECT_EQ(Component.ParticleSystem->Emitters[0]->Material.Get(), Authored);
    }

    // Re-wrapping would add a chain level per call until the depth limit refused the parent outright.
    TEST(ParticleComponentMaterial, AskingTwiceReturnsTheSameInstance)
    {
        CMaterial* Authored = MakeMaterial(EMaterialType::Particle);

        SParticleSystemComponent Component;
        Component.ParticleSystem = MakeSystemWithMaterial(Authored);

        CMaterialInstance* First = Component.CreateDynamicMaterialInstance(0);
        ASSERT_NE(First, nullptr);
        EXPECT_EQ(Component.CreateDynamicMaterialInstance(0), First);
    }
}
