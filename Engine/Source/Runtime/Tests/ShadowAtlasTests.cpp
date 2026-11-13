#include "gtest/gtest.h"

#include "World/Scene/RenderScene/SceneRenderTypes.h"

// A point light that fails partway through its 6 faces must return the faces it already got, or those tiles are gone for the rest of the frame.

namespace Lumina
{
    TEST(ShadowAtlas, FailedPointLightFreesAlreadyAllocatedFaces)
    {
        FShadowAtlasConfig Config;
        Config.AtlasResolution   = 512;
        Config.MaxTileResolution = 256;
        Config.MinTileResolution = 128;
        FShadowAtlas Atlas(Config);

        for (int32 i = 0; i < 13; ++i)
        {
            ASSERT_NE(Atlas.AllocateTile(128), Constants::kIndexNone);
        }

        int32 FaceTileIndices[6] = { Constants::kIndexNone, Constants::kIndexNone, Constants::kIndexNone,
                                      Constants::kIndexNone, Constants::kIndexNone, Constants::kIndexNone };
        bool bAllAllocated = true;
        for (uint32 Face = 0; Face < 6; ++Face)
        {
            FaceTileIndices[Face] = Atlas.AllocateTile(128);
            if (FaceTileIndices[Face] == Constants::kIndexNone)
            {
                bAllAllocated = false;
                break;
            }
        }
        ASSERT_FALSE(bAllAllocated);

        ASSERT_EQ(Atlas.AllocateTile(128), Constants::kIndexNone);

        for (int32 Face = 0; Face < 6; ++Face)
        {
            if (FaceTileIndices[Face] != Constants::kIndexNone)
            {
                Atlas.Free(FaceTileIndices[Face]);
            }
        }

        EXPECT_NE(Atlas.AllocateTile(128), Constants::kIndexNone);
    }

    // A light rejected by the ShadowDataCount slot budget must give back its already-granted tiles, the same way AllocateShadowSlot's own per-face failure does.
    TEST(ShadowAtlas, SlotBudgetOverflowFreesGrantedTiles)
    {
        FShadowAtlasConfig Config;
        Config.AtlasResolution   = 512;
        Config.MaxTileResolution = 128;
        Config.MinTileResolution = 128;
        FShadowAtlas Atlas(Config);

        constexpr uint32 kAtlasTileCapacity = 16;   // (512 / 128)^2 roots, one 128px tile each.

        TAtomic<uint32> ShadowDataCount = (uint32)MAX_SHADOWS - 1;
        TArray<FLight, MAX_LIGHTS> Lights = {};
        Lights[0].ShadowDataIndex = Constants::kIndexNone;
        Lights[1].ShadowDataIndex = Constants::kIndexNone;
        Lights[2].ShadowDataIndex = Constants::kIndexNone;
        TArray<FLightShadowData, MAX_SHADOWS> Shadows = {};
        TArray<TVector<FLightShadow>, (uint32)ELightType::Num> PackedShadows = {};

        FShadowRequest PointReq = {};
        PointReq.LightIndex  = 0;
        PointReq.Type        = ELightType::Point;
        PointReq.Attenuation = 100.0f;

        // Fills the last valid slot (MAX_SHADOWS - 1), so it must be granted and keep its 6 tiles.
        AllocateShadowSlot(Atlas, PointReq, 128, ShadowDataCount, Lights, Shadows, PackedShadows);
        ASSERT_EQ(Lights[0].ShadowDataIndex, (int32)MAX_SHADOWS - 1);

        FShadowRequest RejectedPointReq = PointReq;
        RejectedPointReq.LightIndex = 1;
        AllocateShadowSlot(Atlas, RejectedPointReq, 128, ShadowDataCount, Lights, Shadows, PackedShadows);
        EXPECT_EQ(Lights[1].ShadowDataIndex, Constants::kIndexNone);

        FShadowRequest RejectedSpotReq = {};
        RejectedSpotReq.LightIndex      = 2;
        RejectedSpotReq.Type            = ELightType::Spot;
        RejectedSpotReq.Attenuation     = 100.0f;
        RejectedSpotReq.OuterFOVDegrees = 45.0f;
        RejectedSpotReq.Direction       = FVector3(0.0f, 0.0f, -1.0f);
        RejectedSpotReq.Up              = FVector3(0.0f, 1.0f, 0.0f);
        AllocateShadowSlot(Atlas, RejectedSpotReq, 128, ShadowDataCount, Lights, Shadows, PackedShadows);
        EXPECT_EQ(Lights[2].ShadowDataIndex, Constants::kIndexNone);

        EXPECT_EQ(PackedShadows[(uint32)ELightType::Point].Num(), 1);
        EXPECT_TRUE(PackedShadows[(uint32)ELightType::Spot].IsEmpty());

        // Only the granted point light's 6 tiles should still be out; the rest of the atlas must still be free.
        uint32 StillAllocatable = 0;
        for (uint32 i = 0; i < kAtlasTileCapacity; ++i)
        {
            if (Atlas.AllocateTile(128) != Constants::kIndexNone)
            {
                ++StillAllocatable;
            }
        }
        EXPECT_EQ(StillAllocatable, kAtlasTileCapacity - 6);
    }
}
