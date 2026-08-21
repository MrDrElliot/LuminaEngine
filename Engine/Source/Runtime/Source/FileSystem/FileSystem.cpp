#include "RuntimePCH.h"
#include "FileSystem.h"

#include "Containers/Function.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Containers/StringFormat.h"


namespace Lumina::VFS
{
    bool IFileSystem::ReadFileRange(TVector<uint8>& Result, FStringView Path, uint64 Offset, uint64 Size)
    {
        Result.clear();

        TVector<uint8> Whole;
        if (!ReadFile(Whole, Path))
        {
            return false;
        }

        if (Offset >= Whole.size())
        {
            return true;
        }

        const uint64 Remaining = (uint64)Whole.size() - Offset;
        const uint64 Available = Size < Remaining ? Size : Remaining;
        Result.assign(Whole.begin() + (size_t)Offset, Whole.begin() + (size_t)(Offset + Available));
        return true;
    }

    namespace Detail
    {
        using FMountList = TVector<TUniquePtr<IFileSystem>>;
        static THashMap<FFixedString, FMountList> FileSystemStorage;

        static FStringView ExtractAliasPrefix(FStringView Path)
        {
            if (Path.empty() || Path[0] != '/')
            {
                return {};
            }

            size_t SecondSlash = Path.find('/', 1);
            return SecondSlash == FStringView::npos ? Path : Path.substr(0, SecondSlash);
        }

        static FMountList* FindMountList(FStringView Path)
        {
            FStringView Alias = ExtractAliasPrefix(Path);
            if (Alias.empty())
            {
                return nullptr;
            }

            FFixedString Key(Alias.data(), Alias.size());
            auto It = FileSystemStorage.find(Key);
            return It == FileSystemStorage.end() ? nullptr : &It->second;
        }

        template<typename TFunc>
        static auto VisitFileSystems(FStringView Path, TFunc&& Func) -> decltype(Func(std::declval<IFileSystem&>()))
        {
            using TResult = decltype(Func(std::declval<IFileSystem&>()));
            constexpr bool bIsVoid = std::is_same_v<TResult, void>;

            FMountList* List = FindMountList(Path);
            if (!List)
            {
                if constexpr (bIsVoid)
                {
                    return;
                }
                else
                {
                    return {};
                }
            }

            // Reversed, so the most recently mounted overlay wins.
            for (auto It = List->rbegin(); It != List->rend(); ++It)
            {
                IFileSystem& FS = **It;
                if constexpr (bIsVoid)
                {
                    Func(FS);
                }
                else
                {
                    if (auto Result = Func(FS))
                    {
                        return Result;
                    }
                }
            }

            if constexpr (!bIsVoid)
            {
                return {};
            }
        }

        IFileSystem& AddFileSystemImpl(const FFixedString& Alias, TUniquePtr<IFileSystem> System)
        {
            FFixedString Normalized = Paths::Normalize(Alias);
            FMountList& List = FileSystemStorage[Normalized];
            List.emplace_back(Move(System));
            return *List.back();
        }
    }

    void Unmount(FStringView Alias)
    {
        // Keyed by normalized FFixedString (e.g. "/Game"), same as AddFileSystemImpl.
        FFixedString Key = Paths::Normalize(Alias);
        Detail::FileSystemStorage.erase(Key);
    }

    bool DoesAliasExists(const FName& Alias)
    {
        // FileSystemStorage is keyed by normalized FFixedString (e.g. "/Game").
        FStringView View = Alias.ToString();
        FFixedString Key = Paths::Normalize(View);
        auto It = Detail::FileSystemStorage.find(Key);
        return It != Detail::FileSystemStorage.end() && !It->second.empty();
    }

    FStringView Extension(FStringView Path)
    {
        size_t Dot = Path.find_last_of('.');
        if (Dot == FString::npos)
        {
            return {};
        }

        return Path.substr(Dot);
    }

    FStringView FileName(FStringView Path, bool bRemoveExtension)
    {
        size_t LastSlash = Path.find_last_of("/\\");
        if (LastSlash == FString::npos)
        {
            return {};
        }

        FStringView FilePart = Path.substr(LastSlash + 1);

        if (bRemoveExtension)
        {
            size_t DotPos = FilePart.find_last_of('.');
            if (DotPos != FString::npos)
            {
                return FilePart.substr(0, DotPos);
            }
        }

        return Move(FilePart);
    }

    bool Remove(FStringView Path)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            return FS.Remove(Path);
        });
    }

    size_t Size(FStringView Path)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            return FS.Size(Path);
        });
    }

    bool RemoveAll(FStringView Path)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            return FS.RemoveAll(Path);
        });
    }

    // Overlay mounts win; with nothing on disk the topmost candidate answers "where would this go".
    FPathString ResolvePath(FStringView Path)
    {
        Detail::FMountList* List = Detail::FindMountList(Path);
        if (List == nullptr)
        {
            return {};
        }

        FPathString Candidate;

        for (auto It = List->rbegin(); It != List->rend(); ++It)
        {
            FPathString Disk = (*It)->ResolveToDiskPath(Path);
            if (Disk.empty())
            {
                continue;
            }

            if (Filesystem::Exists(Disk))
            {
                return Disk;
            }

            if (Candidate.empty())
            {
                Candidate = Move(Disk);
            }
        }

        return Candidate;
    }

    bool CreateDir(FStringView Path)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            return FS.CreateDir(Path);
        });
    }

    bool IsUnderDirectory(FStringView Parent, FStringView Path)
    {
        if (!Path.starts_with(Parent))
        {
            return false;
        }

        return Path.length() == Parent.length() ||
               Path[Parent.length()] == '/' ||
               Path[Parent.length()] == '\\';
    }

    bool IsDirectory(FStringView Path)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            return FS.IsDirectory(Path);
        });
    }

    bool IsLuminaAsset(FStringView Path)
    {
        return Extension(Path) == ".lasset";
    }

    FStringView Parent(FStringView Path, bool bRemoveTrailingSlash)
    {
        size_t Pos = Path.find_last_of('/');
        if (Pos == FString::npos)
        {
            return {};
        }

        return Path.substr(0, bRemoveTrailingSlash ? Pos : Pos + 1);
    }

    bool ReadFile(TVector<uint8>& Result, FStringView Path)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            if (FS.Exists(Path))
            {
                if (FS.ReadFile(Result, Path))
                {
                    return true;
                }
            }

            return false;
        });
    }

    bool ReadFile(FString& OutString, FStringView Path)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            if (FS.Exists(Path))
            {
                if (FS.ReadFile(OutString, Path))
                {
                    return true;
                }
            }

            return false;
        });
    }

    bool ReadFileRange(TVector<uint8>& Result, FStringView Path, uint64 Offset, uint64 Size)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            if (FS.Exists(Path))
            {
                if (FS.ReadFileRange(Result, Path, Offset, Size))
                {
                    return true;
                }
            }

            return false;
        });
    }

    bool WriteFile(FStringView Path, FStringView Data)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            return FS.WriteFile(Path, Data);
        });
    }

    bool WriteFile(FStringView Path, TSpan<const uint8> Data)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            return FS.WriteFile(Path, Data);
        });
    }

    bool AtomicWriteFile(FStringView Path, TSpan<const uint8> Data)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            return FS.AtomicWriteFile(Path, Data);
        });
    }

    bool AtomicWriteFileSpliced(FStringView Path, TSpan<const uint8> Prefix,
                                FStringView SrcPath, uint64 SrcOffset, uint64 SrcSize,
                                TSpan<const uint8> Suffix)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            return FS.AtomicWriteFileSpliced(Path, Prefix, SrcPath, SrcOffset, SrcSize, Suffix);
        });
    }

    void PlatformOpen(FStringView Path)
    {
        Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            if (FS.Exists(Path))
            {
                FS.PlatformOpen(Path);
            }
        });
    }

    bool Exists(FStringView Path)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            return FS.Exists(Path);
        });
    }

    void DirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback)
    {
        Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            FS.DirectoryIterator(Path, Callback);
        });
    }

    void RecursiveDirectoryIterator(FStringView Path, const TFunction<void(const FFileInfo&)>& Callback)
    {
        Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            FS.RecursiveDirectoryIterator(Path, Callback);
        });
    }

    bool IsEmpty(FStringView Directory)
    {
        return Detail::VisitFileSystems(Directory, [&](IFileSystem& FS)
        {
            if (FS.Exists(Directory))
            {
                if (FS.IsEmpty(Directory))
                {
                    return true;
                }
            }

            return false;
        });
    }

    FStringView RemoveExtension(FStringView Path)
    {
        size_t Dot = Path.find_last_of(".");
        if (Dot != FString::npos)
        {
            return Path.substr(0, Dot);
        }

        return Path;
    }

    bool Rename(FStringView Old, FStringView New)
    {
        return Detail::VisitFileSystems(Old, [&](IFileSystem& FS)
        {
            if (FS.Exists(Old))
            {
                if (FS.Rename(Old, New))
                {
                    return true;
                }
            }

            return false;
        });
    }

    FFixedString MakeUniqueFilePath(FStringView BasePath)
    {
        FStringView Ext = Extension(BasePath);
        FStringView NoExtensionPath = RemoveExtension(BasePath);

        FFixedString ReturnPath(BasePath.begin(), BasePath.end());

        if (!Exists(ReturnPath))
        {
            return Move(ReturnPath);
        }

        int32 Counter = 1;
        while (Counter <= 25)
        {
            ReturnPath.clear();
            ReturnPath.append(NoExtensionPath.begin(), NoExtensionPath.end());
            ReturnPath.append("_");
            ReturnPath.append(Format("{}", Counter));

            if (!Ext.empty())
            {
                ReturnPath.append(Ext.begin(), Ext.end());
            }

            if (!Exists(ReturnPath))
            {
                return Move(ReturnPath);
            }

            Counter++;
        }

        return {};
    }

    FFixedString ResolveToVirtualPath(FStringView InputPath)
    {
        if (InputPath.empty())
        {
            return {};
        }

        // Normalize slashes once up front. Both branches need it.
        FFixedString Normalized = Paths::Normalize(InputPath);

        // Already a virtual path.
        if (!Normalized.empty() && Normalized.front() == '/')
        {
            return Normalized;
        }

        // Native mount base paths are absolute and normalized at construction time.
        FStringView NormalizedView(Normalized.data(), Normalized.size());

        FFixedString Best;
        size_t BestBaseLen = 0;

        for (const auto& [Alias, MountList] : Detail::FileSystemStorage)
        {
            for (const TUniquePtr<IFileSystem>& FS : MountList)
            {
                FStringView Base = FS->GetBasePath();
                if (Base.empty() || Base.size() > NormalizedView.size())
                {
                    continue;
                }
                // The longest match wins, so a more specific nested mount beats its parent.
                if (NormalizedView.starts_with(Base) && Base.size() > BestBaseLen)
                {
                    FStringView Tail = NormalizedView.substr(Base.size());
                    if (!Tail.empty() && Tail.front() != '/')
                    {
                        // The base is a prefix but not on a path boundary, so skip it.
                        continue;
                    }
                    Best.assign(Alias.data(), Alias.size());
                    Best.append(Tail.data(), Tail.size());
                    BestBaseLen = Base.size();
                }
            }
        }

        return BestBaseLen > 0 ? Best : Normalized;
    }

    bool HasExtension(FStringView Path, FStringView Ext)
    {
        Path = Paths::Normalize(Path);
        Ext = Paths::Normalize(Ext);
        size_t Dot = Path.find_last_of('.');
        if (Dot == FString::npos)
        {
            return false;
        }

        FStringView ActualExt = Path.substr(Dot);
        return ActualExt == Ext;
    }
}
