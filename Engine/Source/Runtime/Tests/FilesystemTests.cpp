#include <gtest/gtest.h>
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "Platform/Filesystem/PlatformFilesystem.h"

using namespace Lumina;
using namespace Lumina::Filesystem;

namespace
{
    class FilesystemTest : public ::testing::Test
    {
    protected:

        void SetUp() override
        {
            Root = GetTempDirectory();
            while (!Root.empty() && (Root.back() == '/' || Root.back() == '\\'))
            {
                Root.pop_back();
            }

            Root.append("/LuminaFilesystemTests");
            RemoveTree(Root);
            ASSERT_TRUE(MakeDirectoryTree(Root));
        }

        void TearDown() override
        {
            RemoveTree(Root);
        }

        FString Under(FStringView Relative) const
        {
            FString Result = Root;
            Result.push_back('/');
            Result.append(Relative.data(), Relative.size());
            return Result;
        }

        static TSpan<const uint8> Bytes(FStringView Text)
        {
            return TSpan<const uint8>(reinterpret_cast<const uint8*>(Text.data()), Text.size());
        }

        FString Root;
    };
}

TEST_F(FilesystemTest, WriteThenReadRoundTrips)
{
    const FString Path = Under("Hello.txt");
    ASSERT_TRUE(WriteFile(Path, Bytes("Hello Lumina")));

    EXPECT_TRUE(Exists(Path));
    EXPECT_TRUE(IsFile(Path));
    EXPECT_FALSE(IsDirectory(Path));
    EXPECT_EQ(FileSize(Path), 12u);

    FString Text;
    ASSERT_TRUE(ReadFile(Text, Path));
    EXPECT_EQ(Text, FString("Hello Lumina"));

    TVector<uint8> Data;
    ASSERT_TRUE(ReadFile(Data, Path));
    EXPECT_EQ(Data.size(), 12u);
}

TEST_F(FilesystemTest, ReadingAMissingFileFails)
{
    FString Text;
    EXPECT_FALSE(ReadFile(Text, Under("NoSuchFile.txt")));
    EXPECT_EQ(GetLastResult(), EResult::NotFound);
}

TEST_F(FilesystemTest, EmptyFileReadsAsSuccessWithNoBytes)
{
    const FString Path = Under("Empty.bin");
    ASSERT_TRUE(WriteFile(Path, TSpan<const uint8>()));

    TVector<uint8> Data;
    EXPECT_TRUE(ReadFile(Data, Path));
    EXPECT_TRUE(Data.empty());
}

TEST_F(FilesystemTest, WriteCreatesMissingParentDirectories)
{
    const FString Path = Under("A/B/C/Deep.txt");
    ASSERT_TRUE(WriteFile(Path, Bytes("deep")));

    EXPECT_TRUE(IsDirectory(Under("A/B/C")));
    EXPECT_TRUE(IsFile(Path));
}

TEST_F(FilesystemTest, AppendGrowsAnExistingFile)
{
    const FString Path = Under("Log.txt");
    ASSERT_TRUE(WriteFile(Path, Bytes("one")));
    ASSERT_TRUE(AppendFile(Path, Bytes("two")));

    FString Text;
    ASSERT_TRUE(ReadFile(Text, Path));
    EXPECT_EQ(Text, FString("onetwo"));
}

TEST_F(FilesystemTest, ReadFileRangeClampsAtEndOfFile)
{
    const FString Path = Under("Range.bin");
    ASSERT_TRUE(WriteFile(Path, Bytes("0123456789")));

    TVector<uint8> Middle;
    ASSERT_TRUE(ReadFileRange(Middle, Path, 3, 4));
    ASSERT_EQ(Middle.size(), 4u);
    EXPECT_EQ(FString(reinterpret_cast<const char*>(Middle.data()), 4), FString("3456"));

    TVector<uint8> Tail;
    ASSERT_TRUE(ReadFileRange(Tail, Path, 8, 100));
    EXPECT_EQ(Tail.size(), 2u);

    TVector<uint8> PastEnd;
    EXPECT_TRUE(ReadFileRange(PastEnd, Path, 50, 10));
    EXPECT_TRUE(PastEnd.empty());
}

TEST_F(FilesystemTest, PositionalReadsDoNotDependOnTheFilePointer)
{
    const FString Path = Under("Positional.bin");
    ASSERT_TRUE(WriteFile(Path, Bytes("ABCDEFGH")));

    FFileHandle Handle = Open(Path, EAccess::Read);
    ASSERT_TRUE(Handle.IsValid());

    char First[2] = {};
    EXPECT_EQ(Handle.ReadAt(First, 2, 4), 2u);
    EXPECT_EQ(First[0], 'E');
    EXPECT_EQ(First[1], 'F');

    char Second[3] = {};
    EXPECT_EQ(Handle.ReadAt(Second, 3, 0), 3u);
    EXPECT_EQ(Second[0], 'A');
}

TEST_F(FilesystemTest, TruncateShrinksTheFile)
{
    const FString Path = Under("Truncate.bin");
    ASSERT_TRUE(WriteFile(Path, Bytes("0123456789")));

    {
        FFileHandle Handle = Open(Path, EAccess::ReadWrite);
        ASSERT_TRUE(Handle.IsValid());
        ASSERT_TRUE(Handle.Truncate(4));
    }

    EXPECT_EQ(FileSize(Path), 4u);
}

TEST_F(FilesystemTest, AtomicWriteLeavesNoTemporaryBehind)
{
    const FString Path = Under("Atomic.bin");
    ASSERT_TRUE(AtomicWriteFile(Path, Bytes("committed")));

    FString Text;
    ASSERT_TRUE(ReadFile(Text, Path));
    EXPECT_EQ(Text, FString("committed"));

    FString TempPath = Path;
    TempPath.append(".tmp");
    EXPECT_FALSE(Exists(TempPath));
}

TEST_F(FilesystemTest, AtomicWriteReplacesAFileHeldOpenForRead)
{
    const FString Path = Under("Replace.bin");
    ASSERT_TRUE(WriteFile(Path, Bytes("original")));

    FFileHandle Reader = Open(Path, EAccess::Read, ECreateMode::OpenExisting, EShare::All);
    ASSERT_TRUE(Reader.IsValid());

    EXPECT_TRUE(AtomicWriteFile(Path, Bytes("replaced")));

    Reader.Close();

    FString Text;
    ASSERT_TRUE(ReadFile(Text, Path));
    EXPECT_EQ(Text, FString("replaced"));
}

TEST_F(FilesystemTest, SplicedWriteCopiesTheMiddleSectionFromASource)
{
    const FString Source = Under("Source.bin");
    ASSERT_TRUE(WriteFile(Source, Bytes("XXXXPAYLOADXXXX")));

    const FString Target = Under("Target.bin");
    ASSERT_TRUE(AtomicWriteFileSpliced(Target, Bytes("<"), Source, 4, 7, Bytes(">")));

    FString Text;
    ASSERT_TRUE(ReadFile(Text, Target));
    EXPECT_EQ(Text, FString("<PAYLOAD>"));
}

TEST_F(FilesystemTest, SplicedWriteResavesOverItsOwnSource)
{
    const FString Path = Under("SelfSplice.bin");
    ASSERT_TRUE(WriteFile(Path, Bytes("HEADERBULKDATA")));

    ASSERT_TRUE(AtomicWriteFileSpliced(Path, Bytes("NEWHDR"), Path, 6, 8, TSpan<const uint8>()));

    FString Text;
    ASSERT_TRUE(ReadFile(Text, Path));
    EXPECT_EQ(Text, FString("NEWHDRBULKDATA"));
}

TEST_F(FilesystemTest, SplicedWriteRejectsATruncatedSourceAndCommitsNothing)
{
    const FString Source = Under("Short.bin");
    ASSERT_TRUE(WriteFile(Source, Bytes("tiny")));

    const FString Target = Under("NeverWritten.bin");
    EXPECT_FALSE(AtomicWriteFileSpliced(Target, TSpan<const uint8>(), Source, 0, 4096, TSpan<const uint8>()));
    EXPECT_FALSE(Exists(Target));

    FString TempPath = Target;
    TempPath.append(".tmp");
    EXPECT_FALSE(Exists(TempPath));
}

TEST_F(FilesystemTest, DirectoryTreeCreationIsIdempotent)
{
    const FString Path = Under("One/Two/Three");
    EXPECT_TRUE(MakeDirectoryTree(Path));
    EXPECT_TRUE(MakeDirectoryTree(Path));
    EXPECT_TRUE(IsDirectory(Path));
    EXPECT_TRUE(IsDirectoryEmpty(Path));
}

TEST_F(FilesystemTest, RemoveTreeDeletesNestedContent)
{
    ASSERT_TRUE(WriteFile(Under("Tree/A/1.txt"), Bytes("a")));
    ASSERT_TRUE(WriteFile(Under("Tree/A/2.txt"), Bytes("b")));
    ASSERT_TRUE(WriteFile(Under("Tree/B/C/3.txt"), Bytes("c")));

    EXPECT_TRUE(RemoveTree(Under("Tree")));
    EXPECT_FALSE(Exists(Under("Tree")));
}

TEST_F(FilesystemTest, MoveRenamesOverAnExistingTarget)
{
    const FString From = Under("From.txt");
    const FString To   = Under("To.txt");

    ASSERT_TRUE(WriteFile(From, Bytes("moved")));
    ASSERT_TRUE(WriteFile(To, Bytes("stale")));

    EXPECT_TRUE(Move(From, To, true));
    EXPECT_FALSE(Exists(From));

    FString Text;
    ASSERT_TRUE(ReadFile(Text, To));
    EXPECT_EQ(Text, FString("moved"));
}

TEST_F(FilesystemTest, CopyDuplicatesContent)
{
    const FString From = Under("Original.txt");
    const FString To   = Under("Duplicate.txt");

    ASSERT_TRUE(WriteFile(From, Bytes("payload")));
    ASSERT_TRUE(Copy(From, To, true));

    FString Text;
    ASSERT_TRUE(ReadFile(Text, To));
    EXPECT_EQ(Text, FString("payload"));
    EXPECT_TRUE(Exists(From));
}

TEST_F(FilesystemTest, StatReportsSizeAndKind)
{
    const FString File = Under("Stat.txt");
    ASSERT_TRUE(WriteFile(File, Bytes("12345")));

    const FFileStat FileInfo = Stat(File);
    ASSERT_TRUE(FileInfo.bValid);
    EXPECT_EQ(FileInfo.Size, 5u);
    EXPECT_TRUE(FileInfo.IsFile());
    EXPECT_FALSE(FileInfo.IsDirectory());
    EXPECT_GT(FileInfo.LastModifyTime, 0);

    const FFileStat DirectoryInfo = Stat(Root);
    ASSERT_TRUE(DirectoryInfo.bValid);
    EXPECT_TRUE(DirectoryInfo.IsDirectory());

    EXPECT_FALSE(Stat(Under("Missing.txt")).bValid);
}

TEST_F(FilesystemTest, ShallowIterationVisitsOnlyDirectChildren)
{
    ASSERT_TRUE(WriteFile(Under("Iter/One.txt"), Bytes("1")));
    ASSERT_TRUE(WriteFile(Under("Iter/Two.txt"), Bytes("2")));
    ASSERT_TRUE(WriteFile(Under("Iter/Nested/Three.txt"), Bytes("3")));

    int32 Files = 0;
    int32 Directories = 0;

    ASSERT_TRUE(IterateDirectory(Under("Iter"), [&](const FDirectoryEntry& Entry)
    {
        if (Entry.IsDirectory())
        {
            ++Directories;
        }
        else
        {
            ++Files;
            EXPECT_EQ(Entry.Depth, 0u);
            EXPECT_EQ(Entry.GetExtension(), FStringView(".txt"));
        }
    }));

    EXPECT_EQ(Files, 2);
    EXPECT_EQ(Directories, 1);
}

TEST_F(FilesystemTest, RecursiveIterationReachesEveryDepth)
{
    ASSERT_TRUE(WriteFile(Under("Deep/1.txt"), Bytes("1")));
    ASSERT_TRUE(WriteFile(Under("Deep/A/2.txt"), Bytes("2")));
    ASSERT_TRUE(WriteFile(Under("Deep/A/B/3.txt"), Bytes("3")));

    uint32 DeepestDepth = 0;
    int32 Files = 0;

    ASSERT_TRUE(IterateDirectoryRecursive(Under("Deep"), [&](const FDirectoryEntry& Entry)
    {
        if (!Entry.IsDirectory())
        {
            ++Files;
            DeepestDepth = Entry.Depth > DeepestDepth ? Entry.Depth : DeepestDepth;
        }
    }));

    EXPECT_EQ(Files, 3);
    EXPECT_EQ(DeepestDepth, 2u);
}

TEST_F(FilesystemTest, EntryPathsResolveBackToRealFiles)
{
    ASSERT_TRUE(WriteFile(Under("Paths/A/File.bin"), Bytes("0123456789")));

    int32 Seen = 0;

    ASSERT_TRUE(IterateDirectoryRecursive(Under("Paths"), [&](const FDirectoryEntry& Entry)
    {
        if (Entry.IsDirectory())
        {
            return;
        }

        ++Seen;
        EXPECT_EQ(Entry.Name, FStringView("File.bin"));
        EXPECT_EQ(Entry.Size, 10u);
        EXPECT_TRUE(Exists(Entry.FullPath));
        EXPECT_TRUE(Entry.FullPath.ends_with("Paths/A/File.bin"));
    }));

    EXPECT_EQ(Seen, 1);
}

TEST_F(FilesystemTest, VisitorCanStopIteration)
{
    for (int32 Index = 0; Index < 8; ++Index)
    {
        FString Name = "Stop/File";
        Name.append(Format("{}", Index));
        ASSERT_TRUE(WriteFile(Under(Name), Bytes("x")));
    }

    int32 Seen = 0;
    ASSERT_TRUE(IterateDirectory(Under("Stop"), [&](const FDirectoryEntry&)
    {
        ++Seen;
        return Seen < 3 ? EVisit::Continue : EVisit::Stop;
    }));

    EXPECT_EQ(Seen, 3);
}

TEST_F(FilesystemTest, VisitorCanSkipASubtree)
{
    ASSERT_TRUE(WriteFile(Under("Skip/Keep/Yes.txt"), Bytes("y")));
    ASSERT_TRUE(WriteFile(Under("Skip/Ignore/No.txt"), Bytes("n")));

    int32 Files = 0;
    ASSERT_TRUE(IterateDirectoryRecursive(Under("Skip"), [&](const FDirectoryEntry& Entry) -> EVisit
    {
        if (Entry.IsDirectory())
        {
            return Entry.Name == FStringView("Ignore") ? EVisit::SkipSubtree : EVisit::Continue;
        }

        ++Files;
        return EVisit::Continue;
    }));

    EXPECT_EQ(Files, 1);
}

TEST_F(FilesystemTest, IteratingSomethingThatIsNotADirectoryFails)
{
    const FString Path = Under("NotADirectory.txt");
    ASSERT_TRUE(WriteFile(Path, Bytes("x")));

    EXPECT_FALSE(IterateDirectory(Path, [](const FDirectoryEntry&) {}));
    EXPECT_FALSE(IterateDirectory(Under("Absent"), [](const FDirectoryEntry&) {}));
}

TEST_F(FilesystemTest, MappedFileExposesTheWholeContent)
{
    const FString Path = Under("Mapped.bin");
    ASSERT_TRUE(WriteFile(Path, Bytes("mapped content")));

    FMappedFile Mapped = FMappedFile::OpenRead(Path);
    ASSERT_TRUE(Mapped.IsValid());
    ASSERT_EQ(Mapped.Size(), 14u);
    EXPECT_EQ(FString(reinterpret_cast<const char*>(Mapped.Data()), 14), FString("mapped content"));

    Mapped.Close();
    EXPECT_FALSE(Mapped.IsValid());
    EXPECT_TRUE(RemoveFile(Path));
}

TEST_F(FilesystemTest, MappingAnEmptyOrMissingFileFails)
{
    const FString Empty = Under("Nothing.bin");
    ASSERT_TRUE(WriteFile(Empty, TSpan<const uint8>()));

    EXPECT_FALSE(FMappedFile::OpenRead(Empty).IsValid());
    EXPECT_FALSE(FMappedFile::OpenRead(Under("Gone.bin")).IsValid());
}

TEST_F(FilesystemTest, CreateNewRefusesAnExistingFile)
{
    const FString Path = Under("Exclusive.txt");
    ASSERT_TRUE(WriteFile(Path, Bytes("first")));

    FFileHandle Handle = Open(Path, EAccess::Write, ECreateMode::CreateNew);
    EXPECT_FALSE(Handle.IsValid());
    EXPECT_EQ(GetLastResult(), EResult::AlreadyExists);
}

TEST_F(FilesystemTest, BackslashPathsResolveTheSameAsForwardSlashes)
{
    ASSERT_TRUE(WriteFile(Under("Slash/File.txt"), Bytes("s")));

    FString Backslashed = Under("Slash/File.txt");
    for (char& Character : Backslashed)
    {
        if (Character == '/')
        {
            Character = '\\';
        }
    }

    EXPECT_TRUE(Exists(Backslashed));
}

TEST_F(FilesystemTest, HandlesPathsLongerThanTheLegacyLimit)
{
    FString Deep = Root;
    for (int32 Index = 0; Index < 14; ++Index)
    {
        Deep.append("/SegmentOfMeaningfulLength");
    }

    ASSERT_GT(Deep.size(), 260u);
    ASSERT_TRUE(MakeDirectoryTree(Deep));

    FString Path = Deep;
    Path.append("/File.txt");
    ASSERT_TRUE(WriteFile(Path, Bytes("long")));

    FString Text;
    ASSERT_TRUE(ReadFile(Text, Path));
    EXPECT_EQ(Text, FString("long"));
    EXPECT_EQ(FileSize(Path), 4u);
}

TEST_F(FilesystemTest, SetReadOnlyIsReflectedInStat)
{
    const FString Path = Under("Locked.txt");
    ASSERT_TRUE(WriteFile(Path, Bytes("locked")));

    ASSERT_TRUE(SetReadOnly(Path, true));
    EXPECT_TRUE(Stat(Path).IsReadOnly());

    ASSERT_TRUE(SetReadOnly(Path, false));
    EXPECT_FALSE(Stat(Path).IsReadOnly());
}

TEST_F(FilesystemTest, RemoveFileClearsAReadOnlyFlagFirst)
{
    const FString Path = Under("ReadOnly.txt");
    ASSERT_TRUE(WriteFile(Path, Bytes("x")));
    ASSERT_TRUE(SetReadOnly(Path, true));

    EXPECT_TRUE(RemoveFile(Path));
    EXPECT_FALSE(Exists(Path));
}

#include "FileSystem/NativeFileSystem.h"
#include "Containers/StringFormat.h"

TEST(NativeFileSystemTest, ResolveVirtualPathMapsUnderTheAlias)
{
    const VFS::FNativeFileSystem Mount("/Game", "C:/Projects/Demo/Game");

    EXPECT_EQ(Mount.ResolveVirtualPath("/Game/Content/A.lasset"),
              FPathString("C:/Projects/Demo/Game/Content/A.lasset"));

    EXPECT_EQ(Mount.ResolveVirtualPath("/Game"), FPathString("C:/Projects/Demo/Game"));
}

TEST(NativeFileSystemTest, ResolveVirtualPathRejectsForeignAliases)
{
    const VFS::FNativeFileSystem Mount("/Game", "C:/Projects/Demo/Game");

    EXPECT_TRUE(Mount.ResolveVirtualPath("/Engine/Content/A.lasset").empty());

    // A shared prefix is not a match, or "/GameData" would resolve as "<Base>Data".
    EXPECT_TRUE(Mount.ResolveVirtualPath("/GameData/A.lasset").empty());
    EXPECT_TRUE(Mount.ResolveVirtualPath("/Gam").empty());
}

TEST(NativeFileSystemTest, ResolveVirtualPathHandlesPathsPastTheFixedStringLimit)
{
    const VFS::FNativeFileSystem Mount("/Game", "C:/Projects/Demo/Game");

    FString Virtual = "/Game";
    for (int32 Index = 0; Index < 14; ++Index)
    {
        Virtual.append("/SegmentOfMeaningfulLength");
    }

    const FPathString Resolved = Mount.ResolveVirtualPath(Virtual);

    ASSERT_GT(Resolved.size(), 260u);
    EXPECT_TRUE(FStringView(Resolved.data(), Resolved.size()).starts_with("C:/Projects/Demo/Game/Segment"));
    EXPECT_TRUE(FStringView(Resolved.data(), Resolved.size()).ends_with("SegmentOfMeaningfulLength"));
}
