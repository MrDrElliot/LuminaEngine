#include "RuntimePCH.h"
#include "FileSystem.h"

#include "Containers/Function.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Containers/StringFormat.h"


namespace Lumina::VFS
{
    bool IFileSystem::ReadFileRange(TVector<uint8>& Result, FStringView Path, int64 Offset, int64 Size)
    {
        Result.Reset();

        TVector<uint8> Whole;
        if (!ReadFile(Whole, Path))
        {
            return false;
        }

        if (Offset < 0 || Size <= 0 || Offset >= Whole.Num())
        {
            return true;
        }

        const int64 Remaining = Whole.Num() - Offset;
        const int64 Available = Size < Remaining ? Size : Remaining;
        Result.Append(Whole.GetData() + Offset, static_cast<int32>(Available));
        return true;
    }

    namespace Detail
    {
        using FMountList = TVector<TUniquePtr<IFileSystem>>;
        static THashMap<FFixedString, FMountList> FileSystemStorage;

        static FStringView ExtractAliasPrefix(FStringView Path)
        {
            if (Path.IsEmpty() || Path[0] != '/')
            {
                return {};
            }

            int32 SecondSlash = Path.Find('/', 1);
            return SecondSlash == Constants::kIndexNone ? Path : Path.Mid(0, SecondSlash);
        }

        static FMountList* FindMountList(FStringView Path)
        {
            FStringView Alias = ExtractAliasPrefix(Path);
            if (Alias.IsEmpty())
            {
                return nullptr;
            }

            FFixedString Key(Alias.GetData(), Alias.Len());
            auto It = FileSystemStorage.Find(Key);
            return It == nullptr ? nullptr : &(*It);
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
            List.Emplace(Move(System));
            return *List.Last();
        }
    }

    void Unmount(FStringView Alias)
    {
        // Keyed by normalized FFixedString (e.g. "/Game"), same as AddFileSystemImpl.
        FFixedString Key = Paths::Normalize(Alias);
        Detail::FileSystemStorage.Remove(Key);
    }

    bool DoesAliasExists(const FName& Alias)
    {
        // FileSystemStorage is keyed by normalized FFixedString (e.g. "/Game").
        FStringView View = Alias.ToString();
        FFixedString Key = Paths::Normalize(View);
        auto It = Detail::FileSystemStorage.Find(Key);
        return It != nullptr && !It->IsEmpty();
    }

    FStringView Extension(FStringView Path)
    {
        int32 Dot = Path.FindLastOf('.');
        if (Dot == Constants::kIndexNone)
        {
            return {};
        }

        return Path.Mid(Dot);
    }

    FStringView FileName(FStringView Path, bool bRemoveExtension)
    {
        int32 LastSlash = Path.FindLastOf("/\\");
        if (LastSlash == Constants::kIndexNone)
        {
            return {};
        }

        FStringView FilePart = Path.Mid(LastSlash + 1);

        if (bRemoveExtension)
        {
            int32 DotPos = FilePart.FindLastOf('.');
            if (DotPos != Constants::kIndexNone)
            {
                return FilePart.Mid(0, DotPos);
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

    int64 Size(FStringView Path)
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
    bool IsReadOnlyOnDisk(FStringView Path)
    {
        const FPathString Disk = ResolvePath(Path);
        return !Disk.IsEmpty() && Filesystem::Stat(FStringView(*Disk, Disk.Len())).IsReadOnly();
    }

    bool MakeWritable(FStringView Path)
    {
        const FPathString Disk = ResolvePath(Path);
        return !Disk.IsEmpty() && Filesystem::SetReadOnly(FStringView(*Disk, Disk.Len()), false);
    }

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
            if (Disk.IsEmpty())
            {
                continue;
            }

            if (Filesystem::Exists(Disk))
            {
                return Disk;
            }

            if (Candidate.IsEmpty())
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
        if (!Path.StartsWith(Parent))
        {
            return false;
        }

        return Path.Len() == Parent.Len() ||
               Path[Parent.Len()] == '/' ||
               Path[Parent.Len()] == '\\';
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
        int32 Pos = Path.FindLastOf('/');
        if (Pos == Constants::kIndexNone)
        {
            return {};
        }

        return Path.Mid(0, bRemoveTrailingSlash ? Pos : Pos + 1);
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

    bool ReadFile(TVector64<uint8>& Result, FStringView Path)
    {
        return Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            return FS.Exists(Path) && FS.ReadFile(Result, Path);
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

    bool ReadFileRange(TVector<uint8>& Result, FStringView Path, int64 Offset, int64 Size)
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
                                FStringView SrcPath, int64 SrcOffset, int64 SrcSize,
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

    void DirectoryIterator(FStringView Path, const TFunction<void(FFileInfo&)>& Callback)
    {
        Detail::VisitFileSystems(Path, [&](IFileSystem& FS)
        {
            FS.DirectoryIterator(Path, Callback);
        });
    }

    void RecursiveDirectoryIterator(FStringView Path, const TFunction<void(FFileInfo&)>& Callback)
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
        int32 Dot = Path.FindLastOf(".");
        if (Dot != Constants::kIndexNone)
        {
            return Path.Mid(0, Dot);
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
            ReturnPath.Reset();
            ReturnPath.Append(NoExtensionPath.begin(), NoExtensionPath.end());
            ReturnPath.Append("_");
            ReturnPath.Append(Format("{}", Counter));

            if (!Ext.IsEmpty())
            {
                ReturnPath.Append(Ext.begin(), Ext.end());
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
        if (InputPath.IsEmpty())
        {
            return {};
        }

        // Normalize slashes once up front. Both branches need it.
        FFixedString Normalized = Paths::Normalize(InputPath);

        // Already a virtual path.
        if (!Normalized.IsEmpty() && Normalized[0] == '/')
        {
            return Normalized;
        }

        // Native mount base paths are absolute and normalized at construction time.
        FStringView NormalizedView(Normalized.GetData(), Normalized.Len());

        FFixedString Best;
        int32 BestBaseLen = 0;

        for (const auto& [Alias, MountList] : Detail::FileSystemStorage)
        {
            for (const TUniquePtr<IFileSystem>& FS : MountList)
            {
                FStringView Base = FS->GetBasePath();
                if (Base.IsEmpty() || Base.Len() > NormalizedView.Len())
                {
                    continue;
                }
                // The longest match wins, so a more specific nested mount beats its parent.
                if (NormalizedView.StartsWith(Base) && Base.Len() > BestBaseLen)
                {
                    FStringView Tail = NormalizedView.Mid(Base.Len());
                    if (!Tail.IsEmpty() && Tail[0] != '/')
                    {
                        // The base is a prefix but not on a path boundary, so skip it.
                        continue;
                    }
                    Best = FStringView(Alias.GetData(), Alias.Len());
                    Best.Append(Tail.GetData(), Tail.Len());
                    BestBaseLen = Base.Len();
                }
            }
        }

        return BestBaseLen > 0 ? Best : Normalized;
    }

    bool HasExtension(FStringView Path, FStringView Ext)
    {
        // Held by value, since a view of the normalized temporary would dangle.
        const FFixedString NormalPath = Paths::Normalize(Path);
        const FFixedString NormalExt  = Paths::Normalize(Ext);
        const int32 Dot = NormalPath.FindLastOf('.');
        if (Dot == Constants::kIndexNone)
        {
            return false;
        }

        return FStringView(*NormalPath + Dot, NormalPath.Len() - Dot) == FStringView(*NormalExt, NormalExt.Len());
    }
}
