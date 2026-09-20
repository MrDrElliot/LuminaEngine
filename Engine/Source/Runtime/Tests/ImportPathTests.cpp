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
    Import::Paths::Release(Path);

    EXPECT_TRUE(Import::Paths::IsFree(Path));
    EXPECT_EQ(Import::Paths::Reserve(Path), Path);

    Import::Paths::Release(Path);
}

// The whole point of the service: a second caller cannot be handed a name the first one holds.
TEST(ImportPaths, AReservedNameIsNotHandedOutTwice)
{
    const FFixedString Path = TestPath("Contested");
    Import::Paths::Release(Path);

    const FFixedString First = Import::Paths::Reserve(Path);
    ASSERT_EQ(First, Path);
    EXPECT_FALSE(Import::Paths::IsFree(Path));

    const FFixedString Second = Import::Paths::Reserve(Path);
    EXPECT_NE(Second, First);
    EXPECT_EQ(Second, FFixedString(Path).append("_1"));

    Import::Paths::Release(First);
    Import::Paths::Release(Second);
}

TEST(ImportPaths, ReleasingANameLetsTheNextCallerTakeIt)
{
    const FFixedString Path = TestPath("Recycled");
    Import::Paths::Release(Path);

    ASSERT_EQ(Import::Paths::Reserve(Path), Path);
    Import::Paths::Release(Path);

    EXPECT_TRUE(Import::Paths::IsFree(Path));
    EXPECT_EQ(Import::Paths::Reserve(Path), Path);

    Import::Paths::Release(Path);
}

TEST(ImportPaths, AnEmptyPathIsNeverFreeAndNeverReserves)
{
    EXPECT_FALSE(Import::Paths::IsFree(FStringView()));
    EXPECT_TRUE(Import::Paths::Reserve(FStringView()).empty());
}

TEST(ImportPaths, AScopedReservationReleasesUnlessCommitted)
{
    const FFixedString Path = TestPath("Scoped");
    Import::Paths::Release(Path);

    {
        Import::Paths::FScopedReservation Reservation(Path);
        ASSERT_TRUE(Reservation.IsValid());
        EXPECT_EQ(Reservation.Get(), Path);
        EXPECT_FALSE(Import::Paths::IsFree(Path));
    }
    EXPECT_TRUE(Import::Paths::IsFree(Path)) << "an uncommitted scope must not burn the name";

    {
        Import::Paths::FScopedReservation Reservation(Path);
        Reservation.Commit();
    }
    EXPECT_FALSE(Import::Paths::IsFree(Path)) << "a committed scope keeps the name";

    Import::Paths::Release(Path);
}
