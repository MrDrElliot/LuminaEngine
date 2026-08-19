#include "RuntimePCH.h"
#include "FileHelper.h"

#include "Containers/Array.h"
#include "Log/Log.h"
#include "PlatformFilesystem.h"

namespace Lumina::FileHelper
{
    bool SaveArrayToFile(const TVector<uint8>& Array, FStringView Path, uint32 WriteFlags)
    {
        const TSpan<const uint8> Data(Array.data(), Array.size());

        if (!Filesystem::WriteFile(Path, Data))
        {
            LOG_ERROR("Failed to write data to file: {0}", Path);
            return false;
        }

        return true;
    }

    bool LoadFileToArray(TVector<uint8>& Result, FStringView Path)
    {
        if (!Filesystem::ReadFile(Result, Path))
        {
            LOG_ERROR("Failed to read data from file: {0}", Path);
            return false;
        }

        return true;
    }

    FString FileFinder(const FString& FileName, FStringView IteratorPath, bool bRecursive)
    {
        FString Result;

        auto Visit = [&FileName, &Result](const Filesystem::FDirectoryEntry& Entry) -> Filesystem::EVisit
        {
            if (Entry.IsDirectory() || Entry.Name != FStringView(FileName.data(), FileName.size()))
            {
                return Filesystem::EVisit::Continue;
            }

            Result.assign(Entry.FullPath.data(), Entry.FullPath.size());
            return Filesystem::EVisit::Stop;
        };

        if (bRecursive)
        {
            Filesystem::IterateDirectoryRecursive(IteratorPath, Visit);
        }
        else
        {
            Filesystem::IterateDirectory(IteratorPath, Visit);
        }

        return Result;
    }

    bool LoadFileIntoString(FString& OutString, FStringView Path, uint32 ReadFlags)
    {
        if (!Filesystem::ReadFile(OutString, Path))
        {
            LOG_ERROR("Failed to open file: {0}", Path);
            return false;
        }

        return !OutString.empty();
    }

    bool SaveStringToFile(FStringView String, FStringView Path, uint32 WriteFlags)
    {
        const TSpan<const uint8> Data(reinterpret_cast<const uint8*>(String.data()), String.size());

        if (!Filesystem::WriteFile(Path, Data))
        {
            LOG_ERROR("Failed to open file for writing: {0}", Path);
            return false;
        }

        return true;
    }

    bool DoesDirectoryExist(FStringView FilePath)
    {
        return Filesystem::Exists(FilePath);
    }

    bool CreateNewFile(FStringView FilePath, bool bBinary)
    {
        Filesystem::MakeParentDirectoryTree(FilePath);

        const Filesystem::FFileHandle Handle = Filesystem::Open(FilePath, Filesystem::EAccess::Write,
                                                                Filesystem::ECreateMode::CreateNew);
        if (!Handle.IsValid())
        {
            LOG_ERROR("Failed to create file: {0}", FilePath);
            return false;
        }

        return true;
    }

    uint64 GetFileSize(FStringView FilePath)
    {
        return Filesystem::FileSize(FilePath);
    }
}
