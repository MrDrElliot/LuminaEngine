#include "RuntimePCH.h"
#include "NativeFileSystem.h"
#include <filesystem>
#include <fstream>

#include "FileInfo.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "Log/Log.h"


namespace Lumina::VFS
{
    namespace
    {
        // Create the parent directory of FullPath so an ofstream open can't fail
        // just because the folder was never committed (e.g. an empty Config dir).
        void EnsureParentDir(const FFixedString& FullPath)
        {
            std::error_code EC;
            std::filesystem::path Parent = std::filesystem::path(FullPath.c_str()).parent_path();
            if (!Parent.empty())
            {
                std::filesystem::create_directories(Parent, EC);
            }
        }
    }

    FNativeFileSystem::FNativeFileSystem(const FFixedString& InAliasPath, FStringView InBasePath)
        : AliasPath(Paths::Normalize(InAliasPath))
        , BasePath(Paths::Normalize(InBasePath))
    {
    }

    FFixedString FNativeFileSystem::ResolveVirtualPath(FStringView Path) const
    {
        if (!Path.starts_with(AliasPath))
        {
            return {};
        }
    
        FStringView RelativePath = Path.substr(AliasPath.length());
    
        FFixedString FullPath = BasePath;
        FullPath.append(RelativePath.begin(), RelativePath.end());
        
        return FullPath;
    }

    bool FNativeFileSystem::ReadFile(TVector<uint8>& Result, FStringView Path)
    {
        FFixedString FullPath = ResolveVirtualPath(Path);
        
        Result.clear();

        std::ifstream File(FullPath.data(), std::ios::binary | std::ios::ate);
        if (!File)
        {
            return false;
        }

        const std::streamsize Size = File.tellg();
        if (Size < 0)
        {
            return false;
        }

        if (Size == 0)
        {
            return true;
        }

        File.seekg(0, std::ios::beg);

        Result.resize(static_cast<size_t>(Size));

        if (!File.read(reinterpret_cast<char*>(Result.data()), Size))
        {
            Result.clear();
            return false;
        }

        return true;
    }


    bool FNativeFileSystem::ReadFile(FString& OutString, FStringView Path)
    {
        FFixedString FullPath = ResolveVirtualPath(Path);

        std::ifstream File(FullPath.data(), std::ios::binary);
        if (!File)
        {
            return false;
        }

        File.seekg(0, std::ios::end);
        std::streamsize Size = File.tellg();
        File.seekg(0, std::ios::beg);

        if (Size < 0)
        {
            return false;
        }

        OutString.resize(static_cast<size_t>(Size));

        if (!File.read(OutString.data(), Size))
        {
            return false;
        }

        return true;
    }

    bool FNativeFileSystem::WriteFile(FStringView Path, FStringView Data)
    {
        FFixedString FullPath = ResolveVirtualPath(Path);
        EnsureParentDir(FullPath);
        std::ofstream File(FullPath.data(), std::ios::binary | std::ios::trunc);
        if (!File)
        {
            return false;
        }

        File.write(Data.data(), static_cast<std::streamsize>(Data.size()));
        return File.good();
    }

    bool FNativeFileSystem::WriteFile(FStringView Path, TSpan<const uint8> Data)
    {
        FFixedString FullPath = ResolveVirtualPath(Path);
        EnsureParentDir(FullPath);
        std::ofstream OutFile(FullPath.data(), std::ios::binary | std::ios::trunc);
        if (!OutFile)
        {
            return false;
        }

        if (!OutFile.write(reinterpret_cast<const char*>(Data.data()), static_cast<int64>(Data.size())))
        {
            return false;
        }

        return true;
    }

    bool FNativeFileSystem::AtomicWriteFile(FStringView Path, TSpan<const uint8> Data)
    {
        FFixedString FullPath = ResolveVirtualPath(Path);
        if (FullPath.empty())
        {
            return false;
        }

        EnsureParentDir(FullPath);

        FFixedString TempPath = FullPath;
        TempPath.append(".tmp");

        // Make sure no orphan from a prior failed save sticks around.
        {
            std::error_code EC;
            std::filesystem::remove(TempPath.c_str(), EC);
        }

        {
            std::ofstream OutFile(TempPath.data(), std::ios::binary | std::ios::trunc);
            if (!OutFile)
            {
                return false;
            }

            if (!Data.empty())
            {
                OutFile.write(reinterpret_cast<const char*>(Data.data()), static_cast<std::streamsize>(Data.size()));
            }

            OutFile.flush();
            if (!OutFile.good())
            {
                std::error_code EC;
                std::filesystem::remove(TempPath.c_str(), EC);
                return false;
            }
        }
        
        std::error_code EC;
        std::filesystem::rename(TempPath.c_str(), FullPath.c_str(), EC);
        if (EC)
        {
            LOG_ERROR("AtomicWriteFile: rename of {0} -> {1} failed: {2}", TempPath, FullPath, EC.message());
            std::error_code RemoveEC;
            std::filesystem::remove(TempPath.c_str(), RemoveEC);
            return false;
        }

        return true;
    }

    bool FNativeFileSystem::Exists(FStringView Path) const
    {
        // error_code overload: a transient lock/sharing-violation (e.g. another thread mid-rename) must
        // not throw and terminate the process; treat any error as "not present".
        std::error_code Ec;
        return std::filesystem::exists(ResolveVirtualPath(Path).c_str(), Ec);
    }

    bool FNativeFileSystem::IsDirectory(FStringView Path) const
    {
        std::error_code Ec;
        return std::filesystem::is_directory(ResolveVirtualPath(Path).c_str(), Ec);
    }

    size_t FNativeFileSystem::Size(FStringView Path) const
    {
        std::error_code Ec;
        const auto Sz = std::filesystem::file_size(ResolveVirtualPath(Path).c_str(), Ec);
        return Ec ? 0 : (size_t)Sz;
    }

    bool FNativeFileSystem::CreateDir(FStringView Path)
    {
        std::error_code Ec;
        return std::filesystem::create_directories(ResolveVirtualPath(Path).c_str(), Ec);
    }

    bool FNativeFileSystem::Remove(FStringView Path)
    {
        std::error_code Ec;
        return std::filesystem::remove(ResolveVirtualPath(Path).c_str(), Ec);
    }

    bool FNativeFileSystem::RemoveAll(FStringView Path)
    {
        std::error_code Ec;
        return std::filesystem::remove_all(ResolveVirtualPath(Path).c_str(), Ec) != static_cast<std::uintmax_t>(-1);
    }

    bool FNativeFileSystem::Rename(FStringView Old, FStringView New)
    {
        FFixedString OldResolvedPath = ResolveVirtualPath(Old);
        FFixedString NewResolvedPath = ResolveVirtualPath(New);
        
        std::error_code EC;
        std::filesystem::rename(OldResolvedPath.c_str(), NewResolvedPath.c_str(), EC);
        
        if (EC)
        {
            LOG_ERROR("File System Error! - Failed to rename: {0}", EC.message());
            return false;
        }
        
        return true;
    }

    bool FNativeFileSystem::IsEmpty(FStringView Path) const
    {
        return std::filesystem::is_empty(ResolveVirtualPath(Path).c_str());
    }

    void FNativeFileSystem::PlatformOpen(FStringView Path) const
    {
        Platform::LaunchURL(StringUtils::ToWideString(ResolveVirtualPath(Path)).c_str());
    }

    void FNativeFileSystem::DirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const
    {
        FFixedString ResolvedPath = ResolveVirtualPath(Path);
        if (ResolvedPath.empty())
        {
            LOG_WARN("DirectoryIterator: path '{0}' does not resolve under alias '{1}'", Path, AliasPath);
            return;
        }

        std::error_code EC;
        if (!std::filesystem::is_directory(ResolvedPath.c_str(), EC) || EC)
        {
            LOG_WARN("DirectoryIterator: skipping '{0}' (resolved '{1}'): {2}",
                FString(Path.data(), Path.size()), ResolvedPath,
                EC ? EC.message() : std::string("not a directory"));
            return;
        }

        std::filesystem::directory_iterator Begin(ResolvedPath.c_str(), std::filesystem::directory_options::skip_permission_denied, EC);
        if (EC)
        {
            LOG_WARN("DirectoryIterator: failed to open '{0}': {1}", ResolvedPath, EC.message());
            return;
        }
        
        const std::filesystem::directory_iterator End{};
        while (Begin != End)
        {
            const std::filesystem::directory_entry& Entry = *Begin;

            std::string FilePath        = Entry.path().generic_string();
            std::string RelativeStr     = FilePath.substr(BasePath.size());
            FFixedString VirtualPath    { RelativeStr.data(), RelativeStr.size() };

            VirtualPath.insert(0, AliasPath);

            std::error_code EntryEC;
            auto FileTime           = std::filesystem::last_write_time(Entry, EntryEC);
            int64 LastModifyTime    = 0;
            if (!EntryEC)
            {
                auto SysTime        = std::chrono::clock_cast<std::chrono::system_clock>(FileTime);
                LastModifyTime      = std::chrono::duration_cast<std::chrono::nanoseconds>(SysTime.time_since_epoch()).count();
            }
            bool bHidden            = Entry.path().filename().generic_string().starts_with(".");

            EFileFlags Flags = EFileFlags::None;

            if (Entry.path().extension() == ".lasset")
            {
                Flags |= EFileFlags::LAssetFile;
            }

            if (bHidden)
            {
                Flags |= EFileFlags::Hidden;
            }

            if (Entry.is_directory(EntryEC))
            {
                Flags |= EFileFlags::Directory;
            }

            if (Entry.is_symlink(EntryEC))
            {
                Flags |= EFileFlags::Symlink;
            }

            auto Status = Entry.status(EntryEC);
            if (!EntryEC)
            {
                auto Perms = Status.permissions();
                if ((Perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none)
                {
                    Flags |= EFileFlags::ReadOnly;
                }
            }

            FFileInfo FileInfo
            {
                .Name               = Entry.path().filename().generic_string().c_str(),
                .VirtualPath        = FFixedString{VirtualPath.data(),VirtualPath.size()},
                .PathSource         = FFixedString{FilePath.data(), FilePath.size()},
                .LastModifyTime     = LastModifyTime,
                .Flags              = Flags
            };

            Callback(Move(FileInfo));

            Begin.increment(EC);
            if (EC)
            {
                LOG_WARN("DirectoryIterator: iteration error under '{0}': {1}", ResolvedPath, EC.message());
                break;
            }
        }
    }

    void FNativeFileSystem::RecursiveDirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const
    {
        FFixedString BaseResolvedPath = ResolveVirtualPath(Path);
        if (BaseResolvedPath.empty())
        {
            LOG_WARN("RecursiveDirectoryIterator: path '{0}' does not resolve under alias '{1}'", Path, AliasPath);
            return;
        }

        std::error_code EC;
        if (!std::filesystem::is_directory(BaseResolvedPath.c_str(), EC) || EC)
        {
            LOG_WARN("RecursiveDirectoryIterator: skipping '{0}' (resolved '{1}'): {2}",
                FString(Path.data(), Path.size()), BaseResolvedPath,
                EC ? EC.message() : std::string("not a directory"));
            return;
        }

        std::filesystem::recursive_directory_iterator Itr(BaseResolvedPath.c_str(), std::filesystem::directory_options::skip_permission_denied, EC);
        if (EC)
        {
            LOG_WARN("RecursiveDirectoryIterator: failed to open '{0}': {1}", BaseResolvedPath, EC.message());
            return;
        }

        const std::filesystem::recursive_directory_iterator End{};
        while (Itr != End)
        {
            const auto& Entry = *Itr;

            std::string FilePath        = Entry.path().generic_string();
            std::string RelativeStr     = FilePath.substr(BasePath.size());
            FFixedString VirtualPath    { RelativeStr.data(), RelativeStr.size() };

            VirtualPath.insert(0, AliasPath);

            std::error_code TimeEC;
            auto FileTime           = std::filesystem::last_write_time(Entry, TimeEC);
            int64 LastModifyTime    = 0;
            if (!TimeEC)
            {
                auto SysTime        = std::chrono::clock_cast<std::chrono::system_clock>(FileTime);
                LastModifyTime      = std::chrono::duration_cast<std::chrono::nanoseconds>(SysTime.time_since_epoch()).count();
            }
            bool bHidden            = Entry.path().filename().generic_string().starts_with(".");


            EFileFlags Flags = EFileFlags::None;

            if (Entry.path().extension() == ".lasset")
            {
                Flags |= EFileFlags::LAssetFile;
            }

            if (bHidden)
            {
                Flags |= EFileFlags::Hidden;
            }

            if (Entry.is_directory(EC))
            {
                Flags |= EFileFlags::Directory;
            }

            if (Entry.is_symlink(EC))
            {
                Flags |= EFileFlags::Symlink;
            }

            auto Status = Entry.status(EC);
            if (!EC)
            {
                auto Perms = Status.permissions();
                if ((Perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none)
                {
                    Flags |= EFileFlags::ReadOnly;
                }
            }

            FFileInfo FileInfo
            {
                .Name               = Entry.path().filename().generic_string().c_str(),
                .VirtualPath        = FFixedString{VirtualPath.data(),VirtualPath.size()},
                .PathSource         = FFixedString{FilePath.data(), FilePath.size()},
                .LastModifyTime     = LastModifyTime,
                .Flags              = Flags
            };

            Callback(Move(FileInfo));

            Itr.increment(EC);
            if (EC)
            {
                LOG_WARN("RecursiveDirectoryIterator: iteration error under '{0}': {1}", BaseResolvedPath, EC.message());
                break;
            }
        }
    }
}
