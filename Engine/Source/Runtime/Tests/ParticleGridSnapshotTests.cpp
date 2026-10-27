#include <gtest/gtest.h>

#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"

using namespace Lumina;

namespace
{
    // Two by two cells over two meters, so cell centers sit half a meter either side of the origin on X and Z.
    FParticleGridSnapshot MakeSnapshot(bool bWraps)
    {
        FParticleGridSnapshot Snapshot;
        Snapshot.Size         = FUIntVector2(2u, 2u);
        Snapshot.Channels     = 2;
        Snapshot.bWraps       = bWraps;
        Snapshot.WorldSize    = FVector2(2.0f, 2.0f);
        Snapshot.ChannelNames = { FName("Heat"), FName("Density") };
        Snapshot.Values       = { 1.0f, 10.0f,   2.0f, 20.0f,
                                  3.0f, 30.0f,   4.0f, 40.0f };
        return Snapshot;
    }
}

TEST(ParticleGridSnapshot, ACellCenterReadsThatCell)
{
    const FParticleGridSnapshot Snapshot = MakeSnapshot(false);
    EXPECT_FLOAT_EQ(Snapshot.Sample(FName("Heat"), FVector3(-0.5f, 0.0f, -0.5f)), 1.0f);
    EXPECT_FLOAT_EQ(Snapshot.Sample(FName("Heat"), FVector3(0.5f, 0.0f, 0.5f)), 4.0f);
    EXPECT_FLOAT_EQ(Snapshot.Sample(FName("Density"), FVector3(0.5f, 0.0f, -0.5f)), 20.0f);
}

TEST(ParticleGridSnapshot, BetweenCentersBlends)
{
    const FParticleGridSnapshot Snapshot = MakeSnapshot(false);
    EXPECT_FLOAT_EQ(Snapshot.Sample(FName("Heat"), FVector3(0.0f, 0.0f, 0.0f)), 2.5f);
    EXPECT_FLOAT_EQ(Snapshot.Sample(FName("Heat"), FVector3(0.0f, 5.0f, -0.5f)), 1.5f);
}

TEST(ParticleGridSnapshot, PastTheEdgeHoldsTheEdgeOrWraps)
{
    EXPECT_FLOAT_EQ(MakeSnapshot(false).Sample(FName("Heat"), FVector3(-5.0f, 0.0f, -0.5f)), 1.0f);

    // A wrapping grid a cell left of the first column reads the last column.
    EXPECT_FLOAT_EQ(MakeSnapshot(true).Sample(FName("Heat"), FVector3(-1.5f, 0.0f, -0.5f)), 2.0f);
}

TEST(ParticleGridSnapshot, AnUnknownChannelReadsZero)
{
    EXPECT_FLOAT_EQ(MakeSnapshot(false).Sample(FName("Pressure"), FVector3(0.0f)), 0.0f);
    EXPECT_FLOAT_EQ(FParticleGridSnapshot().Sample(FName("Heat"), FVector3(0.0f)), 0.0f);
}
