#include <gtest/gtest.h>

#include "World/Scene/RenderScene/Default/WorldDistanceField.h"

#include <cmath>

using namespace Lumina;

namespace
{
    using FClipmap = FWorldDistanceField;

    FInstanceCullEntry MakeCaster(const FVector4& Sphere)
    {
        FInstanceCullEntry Entry = {};
        Entry.SphereBounds = Sphere;
        const EInstanceFlags Flags = EInstanceFlags::Active | EInstanceFlags::HasGeometry | EInstanceFlags::CastShadow | EInstanceFlags::HasDistanceField;
        Entry.DrawIDAndFlags = (uint32)Flags << 16u;
        return Entry;
    }

    FIntVector3 BrickOf(const FVector3& P, uint32 Level)
    {
        const float Size = FClipmap::BrickWorldSize(Level);
        return FIntVector3((int32)std::floor(P.x / Size), (int32)std::floor(P.y / Size), (int32)std::floor(P.z / Size));
    }

    // Runs updates until nothing is left to write, returning how many it took.
    uint32 Settle(FClipmap& Field, const FVector3& Camera, TSpan<const FInstanceCullEntry> Entries)
    {
        FClipmap::FFrame Frame;
        for (uint32 Updates = 1; Updates <= 200u; ++Updates)
        {
            Field.Update(Camera, {}, Entries, Frame);
            if (Frame.Bricks.IsEmpty())
            {
                return Updates;
            }
        }
        return ~0u;
    }

    bool IsComposite(const FWorldDFBrickGPU& Brick)
    {
        return (Brick.LevelAndMode & FClipmap::kWorldDFComposite) != 0u;
    }
}

// The textures start as garbage, so the first update must write every brick of every level one way or the other.
TEST(WorldDistanceField, TheFirstUpdateCoversEveryBrickNearestFirst)
{
    FClipmap Field;
    FClipmap::FFrame Frame;
    const FVector3 Camera(3.0f, 1.5f, -7.0f);
    Field.Update(Camera, {}, {}, Frame);

    EXPECT_TRUE(Frame.bFresh);
    EXPECT_EQ(Frame.CompositeLevel, 0u);
    ASSERT_EQ(Frame.Bricks.Num(), (int32)(FClipmap::kLevels * FClipmap::kBricksPerLevel));

    const FWorldDFBrickGPU& First = Frame.Bricks[0];
    EXPECT_TRUE(IsComposite(First));
    EXPECT_EQ(FIntVector3(First.X, First.Y, First.Z), BrickOf(Camera, 0u)) << "the camera's own brick merges first";
    EXPECT_TRUE(Field.IsBrickValid(0u, BrickOf(Camera, 0u)));

    int32 Composites = 0;
    for (const FWorldDFBrickGPU& Brick : Frame.Bricks)
    {
        Composites += IsComposite(Brick) ? 1 : 0;
    }
    EXPECT_EQ(Composites, (int32)FClipmap::kCompositeBudget);
}

TEST(WorldDistanceField, AStillCameraSettlesAndThenWritesNothing)
{
    FClipmap Field;
    const uint32 Updates = Settle(Field, FVector3(0.0f), {});
    const uint32 PerLevel = (FClipmap::kBricksPerLevel + FClipmap::kCompositeBudget - 1u) / FClipmap::kCompositeBudget;
    EXPECT_LE(Updates, FClipmap::kLevels * PerLevel + 1u);
    for (uint32 Level = 0; Level < FClipmap::kLevels; ++Level)
    {
        EXPECT_EQ(Field.GetPendingCount(Level), 0u);
    }
}

// Scrolling keeps every brick still inside the window and rewrites only the slab that came in.
TEST(WorldDistanceField, MovingRewritesOnlyWhatScrolledIn)
{
    FClipmap Field;
    const FVector3 Start(0.0f);
    ASSERT_NE(Settle(Field, Start, {}), ~0u);

    // Two finest bricks over, which recenters the finest level and leaves the coarser ones put.
    const FVector3 Moved = Start + FVector3(2.0f * FClipmap::BrickWorldSize(0u), 0.0f, 0.0f);
    FClipmap::FFrame Frame;
    Field.Update(Moved, {}, {}, Frame);

    EXPECT_FALSE(Frame.bFresh);
    EXPECT_EQ(Frame.CompositeLevel, 0u);
    EXPECT_EQ(Frame.Bricks.Num(), (int32)(2u * FClipmap::kBricksPerAxis * FClipmap::kBricksPerAxis));
    const FIntVector3 Origin = Field.GetOriginBrick(0u);
    for (const FWorldDFBrickGPU& Brick : Frame.Bricks)
    {
        EXPECT_TRUE(IsComposite(Brick));
        EXPECT_EQ(Brick.LevelAndMode & 0xFFu, 0u);
        EXPECT_GE(Brick.X, Origin.x + (int32)FClipmap::kBricksPerAxis - 2) << "only the far slab is new";
    }
}

// A moved caster rewrites the bricks around where it was and where it is, and nothing else.
TEST(WorldDistanceField, AMovedCasterRewritesOnlyTheBricksAroundIt)
{
    FClipmap Field;
    const FVector3 Camera(0.0f);
    TVector<FInstanceCullEntry> Entries = { MakeCaster(FVector4(5.0f, 0.0f, 5.0f, 1.0f)) };
    ASSERT_NE(Settle(Field, Camera, Entries), ~0u);

    const FVector4 Old = Entries[0].SphereBounds;
    Entries[0].SphereBounds = FVector4(-9.0f, 2.0f, 3.0f, 1.0f);
    const FVector4 New = Entries[0].SphereBounds;
    const uint32 Dirty[] = { 0u };
    FSunShadowCache::FSlotChanges Changes;
    Changes.Dirty = TSpan<const uint32>(Dirty, 1);

    FClipmap::FFrame Frame;
    Field.Update(Camera, Changes, Entries, Frame);
    ASSERT_FALSE(Frame.Bricks.IsEmpty());

    const uint32 Level = Frame.CompositeLevel;
    const float BrickWorld = FClipmap::BrickWorldSize(Level);
    const float Reach = 1.0f + FClipmap::Band(Level) + FClipmap::VoxelSize(Level) + BrickWorld * 1.75f;
    for (const FWorldDFBrickGPU& Brick : Frame.Bricks)
    {
        const FVector3 Center = (FVector3((float)Brick.X, (float)Brick.Y, (float)Brick.Z) + FVector3(0.5f)) * BrickWorld;
        const float ToOld = Math::Length(Center - FVector3(Old.x, Old.y, Old.z));
        const float ToNew = Math::Length(Center - FVector3(New.x, New.y, New.z));
        EXPECT_LT(Math::Min(ToOld, ToNew), Reach);
    }
}

TEST(WorldDistanceField, AResetRequestStartsOverWithAFreshUpdate)
{
    FClipmap Field;
    ASSERT_NE(Settle(Field, FVector3(0.0f), {}), ~0u);

    Field.RequestReset();
    FClipmap::FFrame Frame;
    Field.Update(FVector3(0.0f), {}, {}, Frame);
    EXPECT_TRUE(Frame.bFresh);
    EXPECT_EQ(Frame.Bricks.Num(), (int32)(FClipmap::kLevels * FClipmap::kBricksPerLevel));
}
