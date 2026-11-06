#include <gtest/gtest.h>

#include "Assets/AssetTypes/Sequence/Sequence.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/SequencerEditMode.h"
#include "UI/Tools/Transactions/ObjectSnapshotCommand.h"

using namespace Lumina;

// A key lives on a track object, so an undo step of the sequence must bring back both the track list and the keys on it.
TEST(SequencerUndo, UndoRestoresTheTrackListAndTheKeysOnEachTrack)
{
    CSequence::StaticClass()->GetDefaultObject();
    CSequenceTrack_Event::StaticClass()->GetDefaultObject();

    CPackage* Package = CPackage::CreatePackage("/SequencerUndo/Sequence");
    CSequence* Sequence = NewObject<CSequence>(Package, "Sequence");
    CSequenceTrack_Event* Events = NewObject<CSequenceTrack_Event>(Package, "Events");
    Events->Keys.EmplaceGetRef().Time = 1.0f;
    Sequence->Tracks.Add(Events);

    FObjectSnapshotCommand Command(Sequence, FName("Edit"), &FSequencerEditMode::SerializeForUndo);
    Events->Keys[0].Time = 2.0f;
    Sequence->Tracks.Add(NewObject<CSequenceTrack_Event>(Package, "Added"));
    Command.Finalize();
    ASSERT_FALSE(Command.IsNoOp());

    Command.Undo();
    ASSERT_EQ(Sequence->Tracks.Num(), 1);
    EXPECT_EQ(Sequence->Tracks[0].Get(), Events);
    ASSERT_EQ(Events->Keys.Num(), 1);
    EXPECT_FLOAT_EQ(Events->Keys[0].Time, 1.0f);

    Command.Redo();
    EXPECT_EQ(Sequence->Tracks.Num(), 2);
    EXPECT_FLOAT_EQ(Events->Keys[0].Time, 2.0f);
}
