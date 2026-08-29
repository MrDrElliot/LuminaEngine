#include <gtest/gtest.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "Platform/Time/PlatformTime.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Platform/Filesystem/PlatformFilesystem.h"

using namespace Lumina;

namespace
{
    double Millis(Lumina::uint64 Start, Lumina::uint64 End)
    {
        return Lumina::PlatformTime::ToMilliseconds(End - Start);
    }

    struct FWalkResult
    {
        std::vector<std::string> Paths;
        uint64                   TotalBytes = 0;
    };

    FWalkResult WalkWithStdFilesystem(const std::string& Root)
    {
        FWalkResult Result;

        std::error_code EC;
        std::filesystem::recursive_directory_iterator Iterator(Root, std::filesystem::directory_options::skip_permission_denied, EC);
        const std::filesystem::recursive_directory_iterator End{};

        while (!EC && Iterator != End)
        {
            const std::filesystem::directory_entry& Entry = *Iterator;

            std::error_code EntryEC;
            const bool bDirectory = Entry.is_directory(EntryEC);

            Result.Paths.push_back(Entry.path().generic_string());

            if (!bDirectory)
            {
                Result.TotalBytes += std::filesystem::file_size(Entry, EntryEC);
            }

            Iterator.increment(EC);
        }

        return Result;
    }

    FWalkResult WalkWithLumina(const std::string& Root)
    {
        FWalkResult Result;

        Filesystem::IterateDirectoryRecursive(FStringView(Root.data(), Root.size()),
            [&Result](const Filesystem::FDirectoryEntry& Entry)
            {
                Result.Paths.emplace_back(Entry.FullPath.data(), Entry.FullPath.size());

                if (!Entry.IsDirectory())
                {
                    Result.TotalBytes += Entry.Size;
                }
            });

        return Result;
    }

    std::string FindWalkRoot()
    {
        const char* EngineDir = std::getenv("LUMINA_DIR");
        if (EngineDir != nullptr)
        {
            std::string Candidate = std::string(EngineDir) + "/Engine/Source";
            std::replace(Candidate.begin(), Candidate.end(), '\\', '/');
            if (std::filesystem::is_directory(Candidate))
            {
                return Candidate;
            }
        }

        std::error_code EC;
        std::filesystem::path Cursor = std::filesystem::current_path(EC);
        for (int32 Level = 0; Level < 6 && !Cursor.empty(); ++Level)
        {
            const std::filesystem::path Candidate = Cursor / "Engine" / "Source";
            if (std::filesystem::is_directory(Candidate, EC))
            {
                return Candidate.generic_string();
            }

            Cursor = Cursor.parent_path();
        }

        return {};
    }
}

TEST(FilesystemBench, RecursiveWalkMatchesStdFilesystemAndIsFaster)
{
    const std::string Root = FindWalkRoot();
    if (Root.empty())
    {
        GTEST_SKIP() << "No engine source tree to walk";
    }

    FWalkResult Reference = WalkWithStdFilesystem(Root);
    FWalkResult Ours      = WalkWithLumina(Root);

    std::sort(Reference.Paths.begin(), Reference.Paths.end());
    std::sort(Ours.Paths.begin(), Ours.Paths.end());

    EXPECT_EQ(Ours.Paths, Reference.Paths);
    EXPECT_EQ(Ours.TotalBytes, Reference.TotalBytes);

    constexpr int32 kIterations = 5;

    const Lumina::uint64 StdStart = Lumina::PlatformTime::Cycles();
    for (int32 Index = 0; Index < kIterations; ++Index)
    {
        WalkWithStdFilesystem(Root);
    }
    const double StdMillis = Millis(StdStart, Lumina::PlatformTime::Cycles()) / kIterations;

    const Lumina::uint64 OurStart = Lumina::PlatformTime::Cycles();
    for (int32 Index = 0; Index < kIterations; ++Index)
    {
        WalkWithLumina(Root);
    }
    const double OurMillis = Millis(OurStart, Lumina::PlatformTime::Cycles()) / kIterations;

    std::printf("[ WALK     ] %zu entries under %s\n", Reference.Paths.size(), Root.c_str());
    std::printf("[ WALK     ] std::filesystem %8.3f ms\n", StdMillis);
    std::printf("[ WALK     ] Lumina          %8.3f ms  (%.2fx)\n", OurMillis, StdMillis / OurMillis);
}

TEST(FilesystemBench, SmallFileReadsBeatIfstream)
{
    const std::string Root = FindWalkRoot();
    if (Root.empty())
    {
        GTEST_SKIP() << "No engine source tree to walk";
    }

    std::vector<std::string> Files;
    Filesystem::IterateDirectoryRecursive(FStringView(Root.data(), Root.size()),
        [&Files](const Filesystem::FDirectoryEntry& Entry)
        {
            if (!Entry.IsDirectory() && Entry.GetExtension() == FStringView(".h") && Files.size() < 400)
            {
                Files.emplace_back(Entry.FullPath.data(), Entry.FullPath.size());
            }
        });

    if (Files.size() < 32)
    {
        GTEST_SKIP() << "Not enough headers to measure";
    }

    uint64 StreamBytes = 0;
    const Lumina::uint64 StreamStart = Lumina::PlatformTime::Cycles();
    for (const std::string& Path : Files)
    {
        std::ifstream File(Path, std::ios::binary | std::ios::ate);
        if (!File)
        {
            continue;
        }

        const std::streamsize Size = File.tellg();
        File.seekg(0, std::ios::beg);

        std::vector<char> Data(static_cast<size_t>(Size));
        File.read(Data.data(), Size);
        StreamBytes += static_cast<uint64>(Size);
    }
    const double StreamMillis = Millis(StreamStart, Lumina::PlatformTime::Cycles());

    uint64 NativeBytes = 0;
    const Lumina::uint64 NativeStart = Lumina::PlatformTime::Cycles();
    for (const std::string& Path : Files)
    {
        TVector<uint8> Data;
        if (Filesystem::ReadFile(Data, FStringView(Path.data(), Path.size())))
        {
            NativeBytes += Data.size();
        }
    }
    const double NativeMillis = Millis(NativeStart, Lumina::PlatformTime::Cycles());

    EXPECT_EQ(NativeBytes, StreamBytes);

    std::printf("[ READ     ] %zu files, %llu bytes\n", Files.size(), (unsigned long long)NativeBytes);
    std::printf("[ READ     ] std::ifstream   %8.3f ms\n", StreamMillis);
    std::printf("[ READ     ] Lumina          %8.3f ms  (%.2fx)\n", NativeMillis, StreamMillis / NativeMillis);
}

TEST(FilesystemBench, StatBeatsIndividualFilesystemQueries)
{
    const std::string Root = FindWalkRoot();
    if (Root.empty())
    {
        GTEST_SKIP() << "No engine source tree to walk";
    }

    std::vector<std::string> Files;
    Filesystem::IterateDirectoryRecursive(FStringView(Root.data(), Root.size()),
        [&Files](const Filesystem::FDirectoryEntry& Entry)
        {
            if (!Entry.IsDirectory() && Files.size() < 2000)
            {
                Files.emplace_back(Entry.FullPath.data(), Entry.FullPath.size());
            }
        });

    if (Files.size() < 32)
    {
        GTEST_SKIP() << "Not enough files to measure";
    }

    uint64 StdTotal = 0;
    const Lumina::uint64 StdStart = Lumina::PlatformTime::Cycles();
    for (const std::string& Path : Files)
    {
        std::error_code EC;
        if (std::filesystem::exists(Path, EC) && !std::filesystem::is_directory(Path, EC))
        {
            StdTotal += std::filesystem::file_size(Path, EC);
        }
    }
    const double StdMillis = Millis(StdStart, Lumina::PlatformTime::Cycles());

    uint64 OurTotal = 0;
    const Lumina::uint64 OurStart = Lumina::PlatformTime::Cycles();
    for (const std::string& Path : Files)
    {
        const Filesystem::FFileStat Info = Filesystem::Stat(FStringView(Path.data(), Path.size()));
        if (Info.IsFile())
        {
            OurTotal += Info.Size;
        }
    }
    const double OurMillis = Millis(OurStart, Lumina::PlatformTime::Cycles());

    EXPECT_EQ(OurTotal, StdTotal);

    std::printf("[ STAT     ] %zu files\n", Files.size());
    std::printf("[ STAT     ] std::filesystem %8.3f ms\n", StdMillis);
    std::printf("[ STAT     ] Lumina          %8.3f ms  (%.2fx)\n", OurMillis, StdMillis / OurMillis);
}

#include "FileSystem/NativeFileSystem.h"

namespace
{
    double NanosPerOp(Lumina::uint64 Start, Lumina::uint64 End, size_t Ops)
    {
        return (Lumina::PlatformTime::ToSeconds(End - Start) * 1e9) / (double)Ops;
    }
}

TEST(FilesystemBench, ResolveVirtualPathCost)
{
    VFS::FNativeFileSystem Mount("/Game", "H:/LuminaTests/MyLuminaProject/Game");

    const FString Short = "/Game/Content/Meshes/Cube.lasset";

    FString Long = "/Game/Content";
    for (int32 Index = 0; Index < 10; ++Index)
    {
        Long.append("/SegmentOfMeaningfulLength");
    }
    Long.append("/Asset.lasset");

    constexpr size_t kIterations = 1000000;
    size_t Sink = 0;

    const Lumina::uint64 ShortStart = Lumina::PlatformTime::Cycles();
    for (size_t Index = 0; Index < kIterations; ++Index)
    {
        Sink += Mount.ResolveVirtualPath(FStringView(Short.data(), Short.size())).size();
    }
    const double ShortNanos = NanosPerOp(ShortStart, Lumina::PlatformTime::Cycles(), kIterations);

    const Lumina::uint64 LongStart = Lumina::PlatformTime::Cycles();
    for (size_t Index = 0; Index < kIterations; ++Index)
    {
        Sink += Mount.ResolveVirtualPath(FStringView(Long.data(), Long.size())).size();
    }
    const double LongNanos = NanosPerOp(LongStart, Lumina::PlatformTime::Cycles(), kIterations);

    EXPECT_GT(Sink, 0u);

    std::printf("[ RESOLVE  ] short path (%zu chars total) %7.2f ns/op\n",
        Mount.ResolveVirtualPath(FStringView(Short.data(), Short.size())).size(), ShortNanos);
    std::printf("[ RESOLVE  ] long path  (%zu chars total) %7.2f ns/op\n",
        Mount.ResolveVirtualPath(FStringView(Long.data(), Long.size())).size(), LongNanos);
}
