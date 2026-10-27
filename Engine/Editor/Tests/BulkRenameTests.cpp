#include <gtest/gtest.h>

#include "UI/Tools/BulkRename.h"

using namespace Lumina;

namespace
{
    FBulkRenameEntry MakeEntry(const char* Path)
    {
        FBulkRenameEntry Entry;
        Entry.SourcePath = Path;
        return Entry;
    }
}

TEST(BulkRename, ARuleReplacesThenWrapsThenNumbers)
{
    FBulkRenameRule Rule;
    Rule.Find = "rock";
    Rule.Replace = "Stone";
    Rule.Prefix = "T_";
    Rule.Suffix = "_D";
    Rule.bNumber = true;
    Rule.NumberStart = 7;
    Rule.NumberDigits = 3;

    EXPECT_EQ(ApplyBulkRenameRule(Rule, "Rock_Rock", 2), "T_Stone_Stone_D_009");

    Rule.bMatchCase = true;
    EXPECT_EQ(ApplyBulkRenameRule(Rule, "Rock_rock", 0), "T_Rock_Stone_D_007");
}

TEST(BulkRename, AnEmptyFindLeavesTheNameAlone)
{
    FBulkRenameRule Rule;
    Rule.Suffix = "_Old";
    EXPECT_EQ(ApplyBulkRenameRule(Rule, "Mesh", 0), "Mesh_Old");
}

TEST(BulkRename, ThePlanFlagsEveryEntryThatCannotMove)
{
    TVector<FBulkRenameEntry> Entries = {
        MakeEntry("/Game/A.lasset"),
        MakeEntry("/Game/B.lasset"),
        MakeEntry("/Game/Keep.lasset"),
        MakeEntry("/Game/Taken.lasset"),
    };

    FBulkRenameRule Rule;
    Rule.Find = "A";
    Rule.Replace = "B";

    const auto Exists = [](FStringView Path) { return Path == "/Game/B.lasset" || Path == "/Game/Taken.lasset"; };
    PlanBulkRename(Rule, Entries, Exists);

    EXPECT_EQ(Entries[0].TargetPath, "/Game/B.lasset");
    EXPECT_EQ(Entries[0].Status, EBulkRenameStatus::Exists);
    EXPECT_EQ(Entries[1].Status, EBulkRenameStatus::Unchanged);
    EXPECT_EQ(Entries[2].Status, EBulkRenameStatus::Unchanged);

    // Without Match Case the a in Taken matches too.
    EXPECT_EQ(Entries[3].NewName, "TBken");
    EXPECT_EQ(Entries[3].Status, EBulkRenameStatus::Ready);

    Rule = FBulkRenameRule();
    Rule.Find = "Keep";
    Rule.Replace = "B";
    PlanBulkRename(Rule, Entries, [](FStringView) { return false; });
    EXPECT_EQ(Entries[2].Status, EBulkRenameStatus::Duplicate);

    Rule.Replace = "Bad?";
    PlanBulkRename(Rule, Entries, [](FStringView) { return false; });
    EXPECT_EQ(Entries[2].Status, EBulkRenameStatus::InvalidCharacters);

    Rule.Replace = "";
    PlanBulkRename(Rule, Entries, [](FStringView) { return false; });
    EXPECT_EQ(Entries[2].Status, EBulkRenameStatus::Empty);
}

TEST(BulkRename, AFolderKeepsADotInItsName)
{
    FBulkRenameEntry Folder = MakeEntry("/Game/v1.2");
    Folder.bDirectory = true;
    TVector<FBulkRenameEntry> Entries = { Folder };

    FBulkRenameRule Rule;
    Rule.Prefix = "Old_";
    PlanBulkRename(Rule, Entries, [](FStringView) { return false; });

    EXPECT_EQ(Entries[0].NewName, "Old_v1.2");
    EXPECT_EQ(Entries[0].TargetPath, "/Game/Old_v1.2");
    EXPECT_EQ(Entries[0].Status, EBulkRenameStatus::Ready);
}
