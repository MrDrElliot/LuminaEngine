#include <gtest/gtest.h>

#include "Tools/Import/ImportPaths.h"

using namespace Lumina;

namespace
{
    // Unique per test, so a leftover reservation from another case cannot change the answer.
    FFixedString TestPath(const char* Name)
    {
        return FFixedString("/Game/ImportPathTests/").append(Name);
    }
}

TEST(ImportPaths, AFreeNameIsHandedBackUnchanged)
{
    const FFixedString Path = TestPath("Unclaimed");
    Import::PathReservations::Release(Path);

    EXPECT_TRUE(Import::PathReservations::IsFree(Path));
    EXPECT_EQ(Import::PathReservations::Reserve(Path), Path);

    Import::PathReservations::Release(Path);
}

// The whole point of the service: a second caller cannot be handed a name the first one holds.
TEST(ImportPaths, AReservedNameIsNotHandedOutTwice)
{
    const FFixedString Path = TestPath("Contested");
    Import::PathReservations::Release(Path);

    const FFixedString First = Import::PathReservations::Reserve(Path);
    ASSERT_EQ(First, Path);
    EXPECT_FALSE(Import::PathReservations::IsFree(Path));

    const FFixedString Second = Import::PathReservations::Reserve(Path);
    EXPECT_NE(Second, First);
    EXPECT_EQ(Second, FFixedString(Path).append("_1"));

    Import::PathReservations::Release(First);
    Import::PathReservations::Release(Second);
}

TEST(ImportPaths, ReleasingANameLetsTheNextCallerTakeIt)
{
    const FFixedString Path = TestPath("Recycled");
    Import::PathReservations::Release(Path);

    ASSERT_EQ(Import::PathReservations::Reserve(Path), Path);
    Import::PathReservations::Release(Path);

    EXPECT_TRUE(Import::PathReservations::IsFree(Path));
    EXPECT_EQ(Import::PathReservations::Reserve(Path), Path);

    Import::PathReservations::Release(Path);
}

TEST(ImportPaths, AnEmptyPathIsNeverFreeAndNeverReserves)
{
    EXPECT_FALSE(Import::PathReservations::IsFree(FStringView()));
    EXPECT_TRUE(Import::PathReservations::Reserve(FStringView()).empty());
}

TEST(ImportPaths, AScopedReservationReleasesUnlessCommitted)
{
    const FFixedString Path = TestPath("Scoped");
    Import::PathReservations::Release(Path);

    {
        Import::PathReservations::FScopedReservation Reservation(Path);
        ASSERT_TRUE(Reservation.IsValid());
        EXPECT_EQ(Reservation.Get(), Path);
        EXPECT_FALSE(Import::PathReservations::IsFree(Path));
    }
    EXPECT_TRUE(Import::PathReservations::IsFree(Path)) << "an uncommitted scope must not burn the name";

    {
        Import::PathReservations::FScopedReservation Reservation(Path);
        Reservation.Commit();
    }
    EXPECT_FALSE(Import::PathReservations::IsFree(Path)) << "a committed scope keeps the name";

    Import::PathReservations::Release(Path);
}
