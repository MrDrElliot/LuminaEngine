#include <gtest/gtest.h>

#include "Settings/EditorSettings.h"
#include "UI/Properties/PropertyTable.h"
#include "UI/Tools/ContentBrowserEditorTool.h"

using namespace Lumina;

namespace
{
    using FEntry = FContentBrowserEditorTool::FListingEntry;

    FEntry MakeEntry(const char* Name, bool bDirectory, const char* Type, uint64 Size, int64 Modified)
    {
        FEntry Entry;
        Entry.Info.Name = Name;
        Entry.Info.Flags = bDirectory ? VFS::EFileFlags::Directory : VFS::EFileFlags::File;
        Entry.Info.Size = Size;
        Entry.Info.LastModifyTime = Modified;
        Entry.TypeLabel = FName(Type);
        return Entry;
    }

    TVector<FString> SortedNames(EContentBrowserSortMode Mode, bool bAscending)
    {
        TVector<FEntry> Entries;
        Entries.Add(MakeEntry("Bravo", false, "TEXTURE", 300, 20));
        Entries.Add(MakeEntry("ZFolder", true, "FOLDER", 0, 5));
        Entries.Add(MakeEntry("Alpha", false, "MESH", 100, 30));
        Entries.Add(MakeEntry("AFolder", true, "FOLDER", 0, 50));
        Entries.Add(MakeEntry("Charlie", false, "AUDIO", 200, 10));
        FContentBrowserEditorTool::SortListing(Entries, Mode, bAscending);

        TVector<FString> Names;
        for (const FEntry& Entry : Entries)
        {
            Names.Add(Entry.Info.Name);
        }
        return Names;
    }
}

TEST(ContentBrowserSort, FoldersListFirstUnderEveryMode)
{
    EXPECT_EQ(SortedNames(EContentBrowserSortMode::Name, true),  TVector<FString>({ "AFolder", "ZFolder", "Alpha", "Bravo", "Charlie" }));
    EXPECT_EQ(SortedNames(EContentBrowserSortMode::Name, false), TVector<FString>({ "ZFolder", "AFolder", "Charlie", "Bravo", "Alpha" }));
    EXPECT_EQ(SortedNames(EContentBrowserSortMode::Type, true),  TVector<FString>({ "AFolder", "ZFolder", "Charlie", "Alpha", "Bravo" }));
    EXPECT_EQ(SortedNames(EContentBrowserSortMode::Size, true),  TVector<FString>({ "AFolder", "ZFolder", "Alpha", "Charlie", "Bravo" }));
    EXPECT_EQ(SortedNames(EContentBrowserSortMode::Size, false), TVector<FString>({ "ZFolder", "AFolder", "Bravo", "Charlie", "Alpha" }));
    EXPECT_EQ(SortedNames(EContentBrowserSortMode::DateModified, true),  TVector<FString>({ "ZFolder", "AFolder", "Charlie", "Bravo", "Alpha" }));
    EXPECT_EQ(SortedNames(EContentBrowserSortMode::DateModified, false), TVector<FString>({ "AFolder", "ZFolder", "Alpha", "Bravo", "Charlie" }));
}
