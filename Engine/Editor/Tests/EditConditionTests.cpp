#include <gtest/gtest.h>

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Reflection/PropertyValidation.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/Package/Package.h"
#include "UI/Properties/PropertyTable.h"

using namespace Lumina;

namespace
{
    // The particle emitter's render settings, whose conditions use every form the parser takes.
    struct FEmitterFixture
    {
        FEmitterFixture()
        {
            RegisterPendingTypes();
            Emitter = NewObject<CParticleEmitter>(CPackage::CreatePackage("/Game/Tests/EditConditionEmitter"), "Emitter");
        }

        bool Shown(const char* Name) const
        {
            FProperty* Property = CParticleEmitter::StaticClass()->GetProperty(Name);
            EXPECT_NE(Property, nullptr) << Name;
            return Property == nullptr || IsPropertyEditConditionMet(Property, Emitter);
        }

        CParticleEmitter* Emitter = nullptr;
    };
}

TEST(EditCondition, EnumComparisonsJoinedByAndOnlyShowForTheirMode)
{
    FEmitterFixture F;
    EXPECT_TRUE(F.Shown("FacingMode"));
    EXPECT_FALSE(F.Shown("VelocityStretch")) << "camera-facing sprites have nothing to stretch";
    F.Emitter->FacingMode = EParticleFacingMode::VelocityAligned;
    EXPECT_TRUE(F.Shown("VelocityStretch"));

    F.Emitter->RenderMode = EParticleRenderMode::Mesh;
    EXPECT_FALSE(F.Shown("FacingMode"));
    EXPECT_FALSE(F.Shown("VelocityStretch"));
    EXPECT_TRUE(F.Shown("Mesh"));
    EXPECT_FALSE(F.Shown("RibbonSegments"));
    EXPECT_FALSE(F.Shown("Material")) << "a Particle material shades sprites only";
    EXPECT_TRUE(F.Shown("Texture")) << "so meshes keep the built-in shading";
}

TEST(EditCondition, ParenthesesGroupAnOrInsideAnAnd)
{
    FEmitterFixture F;
    EXPECT_FALSE(F.Shown("FlipbookMode")) << "a 1x1 sheet has no frames to play";
    F.Emitter->SubUVRows = 4;
    EXPECT_TRUE(F.Shown("FlipbookMode"));
    EXPECT_FALSE(F.Shown("FlipbookFPS"));
    F.Emitter->FlipbookMode = EParticleFlipbookMode::Loop;
    EXPECT_TRUE(F.Shown("FlipbookFPS"));

    F.Emitter->RenderMode = EParticleRenderMode::Mesh;
    EXPECT_FALSE(F.Shown("FlipbookMode")) << "the Or binds inside its parentheses, so Mesh fails the And";

    F.Emitter->RenderMode = EParticleRenderMode::Sprite;
    F.Emitter->bWriteDepth = true;
    EXPECT_FALSE(F.Shown("SoftFadeDistance"));
}

TEST(EditCondition, BareFloatsAndStringsCountAsSetWhenNonZeroOrNonEmpty)
{
    FEmitterFixture F;
    EXPECT_FALSE(F.Shown("bInterpolate"));
    F.Emitter->FixedFPS = 30.0f;
    EXPECT_TRUE(F.Shown("bInterpolate"));

    EXPECT_FALSE(F.Shown("SpawnEvent"));
    F.Emitter->SpawnFromEmitter = "Parent";
    EXPECT_TRUE(F.Shown("SpawnEvent"));
    EXPECT_FALSE(F.Shown("EventRate"));
    F.Emitter->SpawnEvent = EParticleEventType::Continuous;
    EXPECT_TRUE(F.Shown("EventRate"));
}

TEST(EditCondition, NoneDisablesEveryRenderSetting)
{
    FEmitterFixture F;
    EXPECT_TRUE(F.Shown("Texture"));
    F.Emitter->RenderMode = EParticleRenderMode::None;
    for (const char* Name : { "Material", "Texture", "BlendMode", "SortMode", "FacingMode", "SubUVColumns" })
    {
        EXPECT_FALSE(F.Shown(Name)) << Name;
    }
}

// The built-in texture and a material shade the same sprite, so assigning one disables the other until it is unassigned.
TEST(EditCondition, ATextureDisablesTheMaterialUntilUnassigned)
{
    FEmitterFixture F;
    EXPECT_TRUE(F.Shown("Material"));
    F.Emitter->Texture = NewObject<CTexture>(F.Emitter->GetPackage(), "Sprite");
    EXPECT_FALSE(F.Shown("Material"));
    EXPECT_FALSE(CParticleEmitter::StaticClass()->GetProperty("Material")->GetMetadata("EditConditionReason").IsEmpty())
        << "a disabled Material says how to enable it";
    F.Emitter->Texture = nullptr;
    EXPECT_TRUE(F.Shown("Material"));
}

TEST(PropertyValidation, AnEmitterRefusesAMaterialOutsideTheParticleDomain)
{
    FEmitterFixture F;
    FProperty* Property = CParticleEmitter::StaticClass()->GetProperty("Material");
    ASSERT_NE(Property, nullptr);

    CMaterial* Material = NewObject<CMaterial>(CPackage::CreatePackage("/Game/Tests/ValidationMaterial"), "Material");
    const TStrongObjectPtr<CObject> Candidate(Material);

    Material->MaterialType = EMaterialType::PBR;
    const FString Refused = PropertyValidation::Check(Property, F.Emitter, &Candidate);
    EXPECT_FALSE(Refused.IsEmpty());
    EXPECT_TRUE(Refused.Contains("PBR")) << *Refused;

    Material->MaterialType = EMaterialType::Particle;
    EXPECT_TRUE(PropertyValidation::Check(Property, F.Emitter, &Candidate).IsEmpty());

    const TStrongObjectPtr<CObject> Nothing;
    EXPECT_TRUE(PropertyValidation::Check(Property, F.Emitter, &Nothing).IsEmpty()) << "an empty slot is always allowed";
}

TEST(PropertyValidation, CheckCurrentReadsTheHeldValue)
{
    FEmitterFixture F;
    FProperty* Property = CParticleEmitter::StaticClass()->GetProperty("Material");
    ASSERT_NE(Property, nullptr);

    CMaterial* Material = NewObject<CMaterial>(CPackage::CreatePackage("/Game/Tests/ValidationHeldMaterial"), "Material");
    F.Emitter->Material = Material;
    EXPECT_FALSE(PropertyValidation::CheckCurrent(Property, F.Emitter).IsEmpty()) << "a new material is PBR";

    FProperty* Unvalidated = CParticleEmitter::StaticClass()->GetProperty("Texture");
    ASSERT_NE(Unvalidated, nullptr);
    EXPECT_TRUE(PropertyValidation::CheckCurrent(Unvalidated, F.Emitter).IsEmpty());
}

TEST(PropertyValidation, AnEmitterHoldingARefusedMaterialIsNotSimulated)
{
    FEmitterFixture F;
    CMaterial* Material = NewObject<CMaterial>(CPackage::CreatePackage("/Game/Tests/ValidationSimulatedMaterial"), "Material");
    F.Emitter->Material = Material;

    Material->MaterialType = EMaterialType::PBR;
    EXPECT_FALSE(F.Emitter->IsSimulated());

    Material->MaterialType = EMaterialType::Particle;
    EXPECT_TRUE(F.Emitter->IsSimulated());

    F.Emitter->bEnabled = false;
    EXPECT_FALSE(F.Emitter->IsSimulated());
}
