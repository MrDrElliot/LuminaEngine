#include <gtest/gtest.h>

#include "FileSystem/MemoryFileSystem.h"
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

    TVector<FString> ListedPaths(FStringView Folder, bool bRecursive)
    {
        TVector<FEntry> Entries;
        FContentBrowserEditorTool::ReadFolderListing(Folder, bRecursive, Entries);

        TVector<FString> Paths;
        for (const FEntry& Entry : Entries)
        {
            Paths.Add(Entry.Info.VirtualPath);
        }
        Algo::Sort(Paths);
        return Paths;
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

TEST(ContentBrowserListing, RecursiveReadFindsNestedSubfolderEntries)
{
    VFS::FMemoryFileSystem& FS = VFS::Mount<VFS::FMemoryFileSystem>("/CBListing");
    const uint8 Byte = 0;
    FS.AddFile("/CBListing/Top.cs", TSpan<const uint8>(&Byte, 1));
    FS.AddFile("/CBListing/Props/Rocks/DeepOnly.cs", TSpan<const uint8>(&Byte, 1));
    FS.AddFile("/CBListing/.lmeta/Hidden.cs", TSpan<const uint8>(&Byte, 1));

    EXPECT_EQ(ListedPaths("/CBListing", false), TVector<FString>({ "/CBListing/Props", "/CBListing/Top.cs" }));
    EXPECT_EQ(ListedPaths("/CBListing", true), TVector<FString>({ "/CBListing/Props", "/CBListing/Props/Rocks", "/CBListing/Props/Rocks/DeepOnly.cs", "/CBListing/Top.cs" }));
    EXPECT_EQ(ListedPaths("/CBListing/Props", true), TVector<FString>({ "/CBListing/Props/Rocks", "/CBListing/Props/Rocks/DeepOnly.cs" }));

    VFS::Unmount("/CBListing");
}

TEST(ContentBrowserListing, RenamingAFolderMovesNestedChildrenWithIt)
{
    VFS::FMemoryFileSystem& FS = VFS::Mount<VFS::FMemoryFileSystem>("/CBRename");
    const uint8 Byte = 0;
    FS.AddFile("/CBRename/Props/Rock.cs", TSpan<const uint8>(&Byte, 1));
    FS.AddFile("/CBRename/Props/Rocks/Deep.cs", TSpan<const uint8>(&Byte, 1));

    EXPECT_TRUE(VFS::Rename("/CBRename/Props", "/CBRename/Items"));

    EXPECT_TRUE(VFS::Exists("/CBRename/Items"));
    EXPECT_TRUE(VFS::Exists("/CBRename/Items/Rock.cs"));
    EXPECT_TRUE(VFS::Exists("/CBRename/Items/Rocks"));
    EXPECT_TRUE(VFS::Exists("/CBRename/Items/Rocks/Deep.cs"));

    EXPECT_FALSE(VFS::Exists("/CBRename/Props"));
    EXPECT_FALSE(VFS::Exists("/CBRename/Props/Rock.cs"));
    EXPECT_FALSE(VFS::Exists("/CBRename/Props/Rocks"));
    EXPECT_FALSE(VFS::Exists("/CBRename/Props/Rocks/Deep.cs"));

    VFS::Unmount("/CBRename");
}
