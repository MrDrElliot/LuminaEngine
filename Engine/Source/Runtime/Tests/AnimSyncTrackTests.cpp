#include <gtest/gtest.h>

#include "Animation/AnimSyncTrack.h"

using namespace Lumina;

namespace
{
    FSyncTrack MakeTrack(std::initializer_list<float> StartTimes)
    {
        TVector<FSyncTrack::FMarker> Markers;
        int32 Index = 0;
        for (float StartTime : StartTimes)
        {
            Markers.push_back({ FName(std::to_string(Index++).c_str()), StartTime });
        }

        FSyncTrack Track;
        Track.BuildFromMarkers(Markers);
        return Track;
    }
}

// A clip with no markers has to behave exactly as the phase matching it replaces.
TEST(AnimSyncTrack, DefaultTrackIsAPlainPhase)
{
    const FSyncTrack Track;

    EXPECT_EQ(Track.NumEvents(), 1);
    EXPECT_NEAR(Track.GetPercentThrough(FSyncPosition(0, 0.25f)), 0.25f, 1e-5f);
    EXPECT_NEAR(Track.GetPercentThrough(FSyncPosition(0, 0.90f)), 0.90f, 1e-5f);

    const FSyncPosition Advanced = Track.Advance(FSyncPosition(0, 0.9f), 0.2f);
    EXPECT_EQ(Advanced.EventIndex, 0);
    EXPECT_NEAR(Advanced.Percent, 0.1f, 1e-5f);
}

// Markers reflect into the spans between them, and the last span wraps past the end of the clip.
TEST(AnimSyncTrack, MarkersReflectIntoSpans)
{
    const FSyncTrack Track = MakeTrack({ 0.1f, 0.6f });

    ASSERT_EQ(Track.NumEvents(), 2);
    EXPECT_NEAR(Track.GetEvent(0).StartTime, 0.1f, 1e-5f);
    EXPECT_NEAR(Track.GetEvent(0).Duration, 0.5f, 1e-5f);
    EXPECT_NEAR(Track.GetEvent(1).StartTime, 0.6f, 1e-5f);
    EXPECT_NEAR(Track.GetEvent(1).Duration, 0.5f, 1e-5f);
}

TEST(AnimSyncTrack, PositionAndPercentRoundTrip)
{
    const FSyncTrack Track = MakeTrack({ 0.1f, 0.6f });

    for (float Percent = 0.0f; Percent < 1.0f; Percent += 0.05f)
    {
        const FSyncPosition Position = Track.GetPosition(Percent);
        EXPECT_NEAR(Track.GetPercentThrough(Position), Percent, 1e-4f) << "at " << Percent;
    }
}

// The wrapped tail of the last event is what covers the clip time before the first marker.
TEST(AnimSyncTrack, TimeBeforeFirstMarkerLandsInTheLastEvent)
{
    const FSyncTrack Track = MakeTrack({ 0.2f, 0.7f });

    const FSyncPosition Position = Track.GetPosition(0.05f);
    EXPECT_EQ(Position.EventIndex, 1);
    EXPECT_NEAR(Track.GetPercentThrough(Position), 0.05f, 1e-4f);
}

// One shared position lands on both clips' markers even when those sit at different times.
TEST(AnimSyncTrack, SharedPositionLinesUpMarkersOfDifferentClips)
{
    const FSyncTrack Walk = MakeTrack({ 0.0f, 0.5f });
    const FSyncTrack Run  = MakeTrack({ 0.15f, 0.55f });

    for (int32 EventIndex = 0; EventIndex < 2; ++EventIndex)
    {
        const FSyncPosition AtMarker(EventIndex, 0.0f);
        EXPECT_NEAR(Walk.GetPercentThrough(AtMarker), Walk.GetEvent(EventIndex).StartTime, 1e-5f);
        EXPECT_NEAR(Run.GetPercentThrough(AtMarker), Run.GetEvent(EventIndex).StartTime, 1e-5f);
    }
}

TEST(AnimSyncTrack, BlendedTrackTakesTheCommonEventCount)
{
    const FSyncTrack TwoStep  = MakeTrack({ 0.0f, 0.5f });
    const FSyncTrack FourStep = MakeTrack({ 0.0f, 0.25f, 0.5f, 0.75f });

    FSyncTrack Blended;
    Blended.BuildBlended(TwoStep, FourStep, 0.5f);
    EXPECT_EQ(Blended.NumEvents(), 4);

    float Total = 0.0f;
    for (int32 i = 0; i < Blended.NumEvents(); ++i)
    {
        Total += Blended.GetEvent(i).Duration;
    }
    EXPECT_NEAR(Total, 1.0f, 1e-4f);
}

// At the extremes the blended track has to be the source track, or a settled blend would retime.
TEST(AnimSyncTrack, BlendEndpointsMatchTheirSource)
{
    const FSyncTrack Even   = MakeTrack({ 0.0f, 0.5f });
    const FSyncTrack Uneven = MakeTrack({ 0.0f, 0.8f });

    FSyncTrack AtZero;
    AtZero.BuildBlended(Even, Uneven, 0.0f);
    FSyncTrack AtOne;
    AtOne.BuildBlended(Even, Uneven, 1.0f);

    for (int32 i = 0; i < 2; ++i)
    {
        EXPECT_NEAR(AtZero.GetEvent(i).Duration, Even.GetEvent(i).Duration, 1e-4f);
        EXPECT_NEAR(AtOne.GetEvent(i).Duration, Uneven.GetEvent(i).Duration, 1e-4f);
    }
}

// A chained blend passes its own destination back in as a source, which must not read cleared data.
TEST(AnimSyncTrack, BlendIntoItsOwnSourceIsSafe)
{
    const FSyncTrack Other = MakeTrack({ 0.0f, 0.4f });

    FSyncTrack Accumulated = MakeTrack({ 0.0f, 0.6f });
    Accumulated.BuildBlended(Accumulated, Other, 0.5f);

    ASSERT_EQ(Accumulated.NumEvents(), 2);
    EXPECT_NEAR(Accumulated.GetEvent(0).Duration, 0.5f, 1e-4f);
    EXPECT_NEAR(Accumulated.GetEvent(1).Duration, 0.5f, 1e-4f);
}

// Scaling to the common event count is what keeps a short cycle from outrunning a long one.
TEST(AnimSyncTrack, BlendDurationScalesToTheCommonEventCount)
{
    EXPECT_NEAR(FSyncTrack::BlendDuration(1.0f, 2.0f, 2, 4, 0.0f), 2.0f, 1e-4f);
    EXPECT_NEAR(FSyncTrack::BlendDuration(1.0f, 2.0f, 2, 4, 1.0f), 2.0f, 1e-4f);
    EXPECT_NEAR(FSyncTrack::BlendDuration(1.0f, 2.0f, 2, 2, 0.5f), 1.5f, 1e-4f);
}

// Advancing a whole cycle has to land back where it started, or a looping blend would drift.
TEST(AnimSyncTrack, AdvanceWrapsAtTheEndOfTheTrack)
{
    const FSyncTrack Track = MakeTrack({ 0.1f, 0.6f });

    FSyncPosition Position(0, 0.0f);
    for (int32 Step = 0; Step < 10; ++Step)
    {
        Position = Track.Advance(Position, 0.1f);
    }

    EXPECT_EQ(Position.EventIndex, 0);
    EXPECT_NEAR(Position.Percent, 0.0f, 1e-3f);
}

// The VM carries a position through one state slot, so the packing has to survive the round trip.
TEST(AnimSyncTrack, PositionPacksIntoOneFloat)
{
    for (int32 EventIndex = 0; EventIndex < 8; ++EventIndex)
    {
        const FSyncPosition Position(EventIndex, 0.375f);
        const FSyncPosition Restored = FSyncPosition::FromFloat(Position.ToFloat());

        EXPECT_EQ(Restored.EventIndex, EventIndex);
        EXPECT_NEAR(Restored.Percent, 0.375f, 1e-5f);
    }
}

// Two markers on one frame would leave a zero-length span, which no conversion survives.
TEST(AnimSyncTrack, DegenerateMarkersCollapseToTheDefaultTrack)
{
    TVector<FSyncTrack::FMarker> Markers;
    Markers.push_back({ FName("a"), 0.25f });
    Markers.push_back({ FName("b"), 0.25f });

    FSyncTrack Track;
    Track.BuildFromMarkers(Markers);

    EXPECT_EQ(Track.NumEvents(), 1);
}

// A long span has to take proportionally longer, or a blend would retime an asymmetric cycle.
TEST(AnimSyncTrack, AdvanceFollowsSpanLengths)
{
    const FSyncTrack Track = MakeTrack({ 0.0f, 0.8f });

    ASSERT_EQ(Track.NumEvents(), 2);
    EXPECT_NEAR(Track.GetEvent(0).Duration, 0.8f, 1e-4f);
    EXPECT_NEAR(Track.GetEvent(1).Duration, 0.2f, 1e-4f);

    const FSyncPosition Half = Track.Advance(FSyncPosition(0, 0.0f), 0.4f);
    EXPECT_EQ(Half.EventIndex, 0);
    EXPECT_NEAR(Half.Percent, 0.5f, 1e-3f);

    const FSyncPosition Late = Track.Advance(FSyncPosition(0, 0.0f), 0.9f);
    EXPECT_EQ(Late.EventIndex, 1);
    EXPECT_NEAR(Late.Percent, 0.5f, 1e-3f);
}

// A blended track starts its spans at zero, which must not shift where the playhead lands.
TEST(AnimSyncTrack, BlendedTrackAdvancesLikeItsSource)
{
    const FSyncTrack Uneven = MakeTrack({ 0.1f, 0.9f });

    FSyncTrack Blended;
    Blended.BuildBlended(Uneven, Uneven, 0.5f);

    const FSyncPosition Direct = Uneven.Advance(FSyncPosition(0, 0.0f), 0.3f);
    const FSyncPosition Copy   = Blended.Advance(FSyncPosition(0, 0.0f), 0.3f);

    EXPECT_EQ(Direct.EventIndex, Copy.EventIndex);
    EXPECT_NEAR(Direct.Percent, Copy.Percent, 1e-3f);
}
