#include "RuntimePCH.h"
#include "NativeFileSystem.h"

#include "FileInfo.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Platform/Process/PlatformProcess.h"
#include "Log/Log.h"


namespace Lumina::VFS
{
    namespace
    {
        EFileFlags TranslateFlags(Filesystem::EAttributes Attributes, FStringView Name)
        {
            EFileFlags Flags = EFileFlags::None;

            if (EnumHasAnyFlags(Attributes, Filesystem::EAttributes::Directory)) { Flags |= EFileFlags::Directory; }
            if (EnumHasAnyFlags(Attributes, Filesystem::EAttributes::Symlink))   { Flags |= EFileFlags::Symlink; }
            if (EnumHasAnyFlags(Attributes, Filesystem::EAttributes::Hidden))    { Flags |= EFileFlags::Hidden; }
            if (EnumHasAnyFlags(Attributes, Filesystem::EAttributes::ReadOnly))  { Flags |= EFileFlags::ReadOnly; }

            if (Name.ends_with(".lasset"))
            {
                Flags |= EFileFlags::LAssetFile;
            }

            return Flags;
        }
    }

    FNativeFileSystem::FNativeFileSystem(const FFixedString& InAliasPath, FStringView InBasePath)
        : AliasPath(Paths::Normalize(InAliasPath))
        , BasePath(Paths::Normalize(InBasePath))
    {
    }

    FPathString FNativeFileSystem::ResolveVirtualPath(FStringView Path) const
    {
        const size_t AliasLength = AliasPath.size();

        if (Path.size() < AliasLength || ::memcmp(Path.data(), AliasPath.data(), AliasLength) != 0)
        {
            return {};
        }

        // Without the boundary test "/GameData/x" would resolve under the "/Game" mount as "Data/x".
        if (Path.size() > AliasLength && Path[AliasLength] != '/')
        {
            return {};
        }

        const size_t BaseLength     = BasePath.size();
        const size_t RelativeLength = Path.size() - AliasLength;

        FPathString FullPath;
        FullPath.resize(BaseLength + RelativeLength);

        ::memcpy(FullPath.data(), BasePath.data(), BaseLength);
        ::memcpy(FullPath.data() + BaseLength, Path.data() + AliasLength, RelativeLength);

        return FullPath;
    }

    bool FNativeFileSystem::ReadFile(TVector<uint8>& Result, FStringView Path)
    {
        return Filesystem::ReadFile(Result, ResolveVirtualPath(Path));
    }

    bool FNativeFileSystem::ReadFile(FString& OutString, FStringView Path)
    {
        return Filesystem::ReadFile(OutString, ResolveVirtualPath(Path));
    }

    bool FNativeFileSystem::ReadFileRange(TVector<uint8>& Result, FStringView Path, uint64 Offset, uint64 Size)
    {
        return Filesystem::ReadFileRange(Result, ResolveVirtualPath(Path), Offset, Size);
    }

    bool FNativeFileSystem::WriteFile(FStringView Path, FStringView Data)
    {
        const TSpan<const uint8> Bytes(reinterpret_cast<const uint8*>(Data.data()), Data.size());
        return Filesystem::WriteFile(ResolveVirtualPath(Path), Bytes);
    }

    bool FNativeFileSystem::WriteFile(FStringView Path, TSpan<const uint8> Data)
    {
        return Filesystem::WriteFile(ResolveVirtualPath(Path), Data);
    }

    bool FNativeFileSystem::AtomicWriteFile(FStringView Path, TSpan<const uint8> Data)
    {
        return Filesystem::AtomicWriteFile(ResolveVirtualPath(Path), Data);
    }

    bool FNativeFileSystem::AtomicWriteFileSpliced(FStringView Path, TSpan<const uint8> Prefix,
                                                   FStringView SrcPath, uint64 SrcOffset, uint64 SrcSize,
                                                   TSpan<const uint8> Suffix)
    {
        const FPathString FullPath = ResolveVirtualPath(Path);
        const FPathString FullSrc  = ResolveVirtualPath(SrcPath);

        if (FullPath.empty() || FullSrc.empty())
        {
            return false;
        }

        return Filesystem::AtomicWriteFileSpliced(FullPath, Prefix, FullSrc, SrcOffset, SrcSize, Suffix);
    }

    bool FNativeFileSystem::Exists(FStringView Path) const
    {
        return Filesystem::Exists(ResolveVirtualPath(Path));
    }

    bool FNativeFileSystem::IsDirectory(FStringView Path) const
    {
        return Filesystem::IsDirectory(ResolveVirtualPath(Path));
    }

    size_t FNativeFileSystem::Size(FStringView Path) const
    {
        return static_cast<size_t>(Filesystem::FileSize(ResolveVirtualPath(Path)));
    }

    bool FNativeFileSystem::CreateDir(FStringView Path)
    {
        return Filesystem::MakeDirectoryTree(ResolveVirtualPath(Path));
    }

    bool FNativeFileSystem::Remove(FStringView Path)
    {
        const FPathString FullPath = ResolveVirtualPath(Path);
        return Filesystem::IsDirectory(FullPath) ? Filesystem::DeleteDirectory(FullPath) : Filesystem::RemoveFile(FullPath);
    }

    bool FNativeFileSystem::RemoveAll(FStringView Path)
    {
        return Filesystem::RemoveTree(ResolveVirtualPath(Path));
    }

    bool FNativeFileSystem::Rename(FStringView Old, FStringView New)
    {
        const FPathString OldResolvedPath = ResolveVirtualPath(Old);
        const FPathString NewResolvedPath = ResolveVirtualPath(New);

        if (!Filesystem::Move(OldResolvedPath, NewResolvedPath, true))
        {
            LOG_ERROR("File System Error! - Failed to rename '{0}': {1}", OldResolvedPath, Filesystem::ToString(Filesystem::GetLastResult()));
            return false;
        }

        return true;
    }

    bool FNativeFileSystem::IsEmpty(FStringView Path) const
    {
        return Filesystem::IsDirectoryEmpty(ResolveVirtualPath(Path));
    }

    void FNativeFileSystem::PlatformOpen(FStringView Path) const
    {
        Platform::LaunchURL(UTF8_TO_TCHAR(ResolveVirtualPath(Path).c_str()));
    }

    void FNativeFileSystem::Iterate(FStringView Path, bool bRecursive, const TFunction<void(const FFileInfo&)>& Callback) const
    {
        const FPathString ResolvedPath = ResolveVirtualPath(Path);
        if (ResolvedPath.empty())
        {
            LOG_WARN("DirectoryIterator: path '{0}' does not resolve under alias '{1}'", Path, AliasPath);
            return;
        }

        const size_t BaseLength = BasePath.size();

        auto Visit = [this, &Callback, BaseLength](const Filesystem::FDirectoryEntry& Entry)
        {
            const FStringView Relative = Entry.FullPath.size() >= BaseLength
                ? Entry.FullPath.substr(BaseLength)
                : Entry.FullPath;

            FString VirtualPath;
            VirtualPath.reserve(AliasPath.size() + Relative.size());
            VirtualPath.append(AliasPath.data(), AliasPath.size());
            VirtualPath.append(Relative.data(), Relative.size());

            FFileInfo FileInfo
            {
                .Name           = FString(Entry.Name.data(), Entry.Name.size()),
                .VirtualPath    = Move(VirtualPath),
                .PathSource     = FString(Entry.FullPath.data(), Entry.FullPath.size()),
                .LastModifyTime = Entry.LastModifyTime,
                .Flags          = TranslateFlags(Entry.Attributes, Entry.Name)
            };

            Callback(FileInfo);
        };

        const bool bWalked = Filesystem::IterateDirectoryRaw(ResolvedPath, bRecursive,
            &Filesystem::Detail::Trampoline<decltype(Visit)>, &Visit);

        if (!bWalked)
        {
            LOG_WARN("DirectoryIterator: skipping '{0}' (resolved '{1}'): {2}",
                Path, ResolvedPath, Filesystem::ToString(Filesystem::GetLastResult()));
        }
    }

    void FNativeFileSystem::DirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const
    {
        Iterate(Path, false, Callback);
    }

    void FNativeFileSystem::RecursiveDirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback) const
    {
        Iterate(Path, true, Callback);
    }
}
