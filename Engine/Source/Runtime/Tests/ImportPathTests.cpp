#include <gtest/gtest.h>

#include "Tools/Import/ImportPaths.h"
#include "Core/Object/ObjectCore.h"

using namespace Lumina;

namespace
{
    // Unique per test, so a leftover reservation from another case cannot change the answer.
    FFixedString TestPath(const char* Name)
    {
        return FFixedString("/Game/ImportPathTests/").Append(Name);
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
    EXPECT_EQ(Second, FFixedString(Path).Append("_1"));

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
    EXPECT_TRUE(Import::PathReservations::Reserve(FStringView()).IsEmpty());
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

// A package's file name and its object name differ by the extension, so releasing either frees the reservation.
TEST(ImportPaths, ReleasingThePackageFileFreesTheName)
{
    const FFixedString Path = TestPath("WithExtension");
    Import::PathReservations::Release(Path);

    ASSERT_EQ(Import::PathReservations::Reserve(Path), Path);
    Import::PathReservations::Release(FFixedString(Path).Append(".lasset"));

    EXPECT_TRUE(Import::PathReservations::IsFree(Path));
}

// Package names ignore case, so a differently cased request must not slip past a held name.
TEST(ImportPaths, ANameDifferingOnlyInCaseIsTaken)
{
    const FFixedString Path = TestPath("CaseHeld");
    Import::PathReservations::Release(Path);

    ASSERT_EQ(Import::PathReservations::Reserve(Path), Path);
    EXPECT_FALSE(Import::PathReservations::IsFree(TestPath("CASEHELD")));

    Import::PathReservations::Release(Path);
}

// Only the package extension goes, so a dot in a folder or file name survives, and a second pass changes nothing.
TEST(ImportPaths, SanitizingKeepsDotsInFoldersAndFileNames)
{
    EXPECT_EQ(SanitizeObjectName("/Game/Props.v2/Crate.lasset"), FFixedString("/Game/Props.v2/Crate"));
    EXPECT_EQ(SanitizeObjectName("/Game/Textures/rock.diffuse.lasset"), FFixedString("/Game/Textures/rock.diffuse"));
    EXPECT_EQ(SanitizeObjectName(SanitizeObjectName("/Game/Textures/rock.diffuse.lasset")), FFixedString("/Game/Textures/rock.diffuse"));
}

// A name found free is only a name, so the next caller can still be handed it.
TEST(ImportPaths, FindingAFreeNameDoesNotReserveIt)
{
    const FFixedString Path = TestPath("Probed");
    Import::PathReservations::Release(Path);

    EXPECT_EQ(Import::PathReservations::FindFree(Path), Path);
    EXPECT_TRUE(Import::PathReservations::IsFree(Path));

    ASSERT_EQ(Import::PathReservations::Reserve(Path), Path);
    EXPECT_EQ(Import::PathReservations::FindFree(Path), FFixedString(Path).Append("_1"));

    Import::PathReservations::Release(Path);
}
