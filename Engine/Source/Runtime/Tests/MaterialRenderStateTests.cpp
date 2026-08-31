#include "gtest/gtest.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Renderer/MaterialTypes.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

// Blend mode drives which translucency lane a surface takes, and the render-state bools drive the rest.

namespace Lumina
{
    namespace
    {
        CMaterial* MakeMaterial(EBlendMode Blend)
        {
            CMaterial* Material = NewObject<CMaterial>();
            if (Material != nullptr)
            {
                Material->BlendMode = Blend;
            }
            return Material;
        }

        CMaterialInstance* MakeInstanceOf(CMaterialInterface* Parent)
        {
            CMaterialInstance* Instance = NewObject<CMaterialInstance>();
            if (Instance != nullptr)
            {
                Instance->Material = Parent;
            }
            return Instance;
        }
    }

    // The moment lane compiles a moment stage and the unordered lane must not; nothing may claim both.
    TEST(MaterialBlendMode, EachBlendTakesExactlyOneTranslucencyLane)
    {
        struct FCase { EBlendMode Blend; bool bMoment; bool bUnordered; };
        const FCase Cases[] =
        {
            { EBlendMode::Opaque,         false, false },
            { EBlendMode::Masked,         false, false },
            { EBlendMode::Translucent,    true,  false },
            { EBlendMode::Additive,       false, true  },
            { EBlendMode::Modulate,       false, true  },
            { EBlendMode::AlphaComposite, true,  false },
        };

        for (const FCase& Case : Cases)
        {
            CMaterial* Material = MakeMaterial(Case.Blend);
            ASSERT_NE(Material, nullptr);

            EXPECT_EQ(Material->IsMomentResolved(), Case.bMoment)
                << "blend mode " << (int)Case.Blend << " picked the wrong moment lane";
            EXPECT_EQ(Material->IsUnorderedBlend(), Case.bUnordered)
                << "blend mode " << (int)Case.Blend << " picked the wrong unordered lane";
            EXPECT_FALSE(Material->IsMomentResolved() && Material->IsUnorderedBlend())
                << "blend mode " << (int)Case.Blend << " claimed both lanes";
        }
    }

    TEST(MaterialBlendMode, OnlyOpaqueReadsAsOpaque)
    {
        EXPECT_TRUE(MakeMaterial(EBlendMode::Opaque)->IsOpaque());
        EXPECT_FALSE(MakeMaterial(EBlendMode::Masked)->IsOpaque());
        EXPECT_FALSE(MakeMaterial(EBlendMode::Modulate)->IsOpaque());
        EXPECT_FALSE(MakeMaterial(EBlendMode::AlphaComposite)->IsOpaque());
    }

    // Appending is what keeps a saved asset's blend mode meaning what it meant when it was written.
    TEST(MaterialBlendMode, TheExistingModesKeepTheirSerializedOrdinals)
    {
        EXPECT_EQ((uint8)EBlendMode::Opaque,      0);
        EXPECT_EQ((uint8)EBlendMode::Masked,      1);
        EXPECT_EQ((uint8)EBlendMode::Translucent, 2);
        EXPECT_EQ((uint8)EBlendMode::Additive,    3);
    }

    // Zero has to mean today's behavior, or a material saved before the flag existed loses its decals.
    TEST(MaterialRenderState, DecalOptOutIsStoredInverted)
    {
        CMaterial* Material = MakeMaterial(EBlendMode::Opaque);
        ASSERT_NE(Material, nullptr);

        EXPECT_TRUE(Material->bReceivesDecals) << "receiving decals is the default";
        EXPECT_TRUE(Material->ReceivesDecals());
        EXPECT_EQ((uint32)EMaterialGPUFlags::NoDecals & (uint32)EMaterialGPUFlags::Unlit, 0u)
            << "the flag must not land in the shading model field";
        EXPECT_EQ((uint32)EMaterialGPUFlags::NoDecals >> kMaterialShadingModelShift & kMaterialShadingModelMask, 0u)
            << "the flag must not land in the shading model field";
    }

    TEST(MaterialRenderState, TheNewStateDefaultsPreserveExistingBehavior)
    {
        CMaterial* Material = MakeMaterial(EBlendMode::Opaque);
        ASSERT_NE(Material, nullptr);

        EXPECT_TRUE(Material->ReceivesDecals());
        EXPECT_FALSE(Material->WritesDepth());
        EXPECT_FALSE(Material->IsShadowOnly());
        EXPECT_TRUE(Material->DoesCastShadows());
    }

    // An instance never declares render state; it reports whatever its root compiled with.
    TEST(MaterialRenderState, AnInstanceReportsItsRootsRenderState)
    {
        CMaterial* Material = MakeMaterial(EBlendMode::Modulate);
        ASSERT_NE(Material, nullptr);
        Material->bReceivesDecals = false;
        Material->bWriteDepth     = true;
        Material->bShadowOnly     = true;

        CMaterialInstance* Instance = MakeInstanceOf(Material);
        ASSERT_NE(Instance, nullptr);

        EXPECT_FALSE(Instance->ReceivesDecals());
        EXPECT_TRUE(Instance->WritesDepth());
        EXPECT_TRUE(Instance->IsShadowOnly());
        EXPECT_TRUE(Instance->IsUnorderedBlend());
        EXPECT_FALSE(Instance->IsMomentResolved());
    }

    // A parentless instance must not claim state it cannot back, or the resolve reads a lane that is absent.
    TEST(MaterialRenderState, AParentlessInstanceFallsBackToTheSafeDefaults)
    {
        CMaterialInstance* Instance = MakeInstanceOf(nullptr);
        ASSERT_NE(Instance, nullptr);

        EXPECT_TRUE(Instance->ReceivesDecals());
        EXPECT_FALSE(Instance->WritesDepth());
        EXPECT_FALSE(Instance->IsShadowOnly());
        EXPECT_FALSE(Instance->IsMomentResolved());
        EXPECT_FALSE(Instance->IsUnorderedBlend());
    }

    // The instance flag word is packed into 16 bits of FGPUInstance::DrawIDAndFlags.
    TEST(MaterialRenderState, ShadowOnlyFitsThePackedInstanceFlagWord)
    {
        EXPECT_LT((uint32)EInstanceFlags::ShadowOnly, 1u << 16)
            << "instance flags are packed into the high 16 bits of DrawIDAndFlags";
        EXPECT_EQ((uint32)EInstanceFlags::ShadowOnly & (uint32)EInstanceFlags::CastShadow, 0u);
        EXPECT_EQ((uint32)EInstanceFlags::ShadowOnly & (uint32)EInstanceFlags::HasGeometry, 0u);
    }
}
