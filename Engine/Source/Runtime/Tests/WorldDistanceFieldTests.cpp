#include <gtest/gtest.h>

#include "Containers/HashTable.h"
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

// A caster under half a voxel of a level only blurs it, so moving one leaves that level and every coarser one untouched.
TEST(WorldDistanceField, ASmallCasterLeavesCoarserLevelsAlone)
{
    FClipmap Field;
    const FVector3 Camera(0.0f);
    TVector<FInstanceCullEntry> Entries = { MakeCaster(FVector4(5.0f, 0.0f, 5.0f, 0.2f)) };
    ASSERT_NE(Settle(Field, Camera, Entries), ~0u);
    ASSERT_GT(FClipmap::MinCasterRadius(1u), 0.2f);

    Entries[0].SphereBounds = FVector4(-6.0f, 1.0f, 2.0f, 0.2f);
    const uint32 Dirty[] = { 0u };
    FSunShadowCache::FSlotChanges Changes;
    Changes.Dirty = TSpan<const uint32>(Dirty, 1);
    FClipmap::FFrame Frame;
    Field.Update(Camera, Changes, Entries, Frame);

    EXPECT_EQ(Frame.CompositeLevel, 0u);
    for (uint32 Level = 1; Level < FClipmap::kLevels; ++Level)
    {
        EXPECT_EQ(Field.GetPendingCount(Level), 0u) << "level " << Level;
    }
}

// A streamed mover is marked once it drifts past the tolerance from where it was merged, not on every small step.
TEST(WorldDistanceField, AStreamedMoverWaitsForItsToleranceBeforeRewriting)
{
    FClipmap Field;
    const FVector3 Camera(0.0f);
    TVector<FInstanceCullEntry> Entries = { MakeCaster(FVector4(5.0f, 0.0f, 5.0f, 1.0f)) };
    ASSERT_NE(Settle(Field, Camera, Entries), ~0u);

    const uint32 Blocks[] = { 0u };
    FSunShadowCache::FSlotChanges Changes;
    Changes.StreamedBlocks = TSpan<const uint32>(Blocks, 1);
    const float Tolerance = FClipmap::kMoveToleranceVoxels * FClipmap::VoxelSize(0u);

    FClipmap::FFrame Frame;
    Entries[0].SphereBounds.x += Tolerance * 0.5f;
    Field.Update(Camera, Changes, Entries, Frame);
    EXPECT_TRUE(Frame.Bricks.IsEmpty()) << "a step under the tolerance changes nothing";

    Entries[0].SphereBounds.x += Tolerance;
    Field.Update(Camera, Changes, Entries, Frame);
    EXPECT_FALSE(Frame.Bricks.IsEmpty()) << "the drift since the merge passed the tolerance";
}

// Enough movers that the marking splits across tasks, every one of them must still leave its new brick to be rewritten.
TEST(WorldDistanceField, ManyMoversMarkEveryBrickTheyReach)
{
    FClipmap Field;
    const FVector3 Camera(0.0f);
    constexpr uint32 Count = 3000u;
    TVector<FInstanceCullEntry> Entries;
    TVector<uint32> Dirty;
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        const float X = -14.0f + (float)(Index % 60u) * 0.47f;
        const float Z = -14.0f + (float)(Index / 60u) * 0.56f;
        Entries.Add(MakeCaster(FVector4(X, -4.0f, Z, 0.5f)));
        Dirty.Add(Index);
    }
    ASSERT_NE(Settle(Field, Camera, Entries), ~0u);

    for (FInstanceCullEntry& Entry : Entries)
    {
        Entry.SphereBounds.y = 6.0f;
    }
    FSunShadowCache::FSlotChanges Changes;
    Changes.Dirty = TSpan<const uint32>(Dirty.GetData(), Dirty.Num());
    FClipmap::FFrame Frame;
    Field.Update(Camera, Changes, Entries, Frame);

    THashSet<uint64> Written;
    for (const FWorldDFBrickGPU& Brick : Frame.Bricks)
    {
        if ((Brick.LevelAndMode & 0xFFu) == 0u)
        {
            Written.Add(((uint64)(uint16)Brick.X << 32u) | ((uint64)(uint16)Brick.Y << 16u) | (uint64)(uint16)Brick.Z);
        }
    }
    for (const FInstanceCullEntry& Entry : Entries)
    {
        const FIntVector3 Brick = BrickOf(FVector3(Entry.SphereBounds.x, Entry.SphereBounds.y, Entry.SphereBounds.z), 0u);
        const uint64 Key = ((uint64)(uint16)Brick.x << 32u) | ((uint64)(uint16)Brick.y << 16u) | (uint64)(uint16)Brick.z;
        EXPECT_TRUE(!Field.IsBrickValid(0u, Brick) || Written.Contains(Key)) << "a mover's new brick kept its old distances";
    }
}
