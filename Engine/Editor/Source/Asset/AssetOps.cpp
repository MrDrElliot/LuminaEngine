#include "EditorPCH.h"
#include "Asset/AssetOps.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetRegistry/TextAssetTypes.h"
#include "Containers/StringFormat.h"
#include "Core/CoreEditorDelegates.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"

namespace Lumina::AssetOps
{
    namespace
    {
        // Case-insensitive compare of a view against a NUL-terminated literal.
        bool IEquals(FStringView A, const char* B)
        {
            const auto Lower = [](char Character)
            {
                return (Character >= 'A' && Character <= 'Z') ? (char)(Character + 32) : Character;
            };

            size_t Index = 0;
            for (; Index < A.size() && B[Index] != 0; ++Index)
            {
                if (Lower(A[Index]) != Lower(B[Index]))
                {
                    return false;
                }
            }

            return Index == A.size() && B[Index] == 0;
        }

        bool IsAtOrUnder(FStringView VirtualPath, const char* RootLiteral)
        {
            const FStringView Root(RootLiteral);
            if (VirtualPath.size() < Root.size() || !IEquals(VirtualPath.substr(0, Root.size()), RootLiteral))
            {
                return false;
            }

            // A prefix match is only real on a path boundary, or "/Game/ContentPacks" would pass.
            return VirtualPath.size() == Root.size() || VirtualPath[Root.size()] == '/';
        }

        FPathOpResult MoveAssetPackage(FStringView OldPath, FStringView NewPath)
        {
            // RenamePackage owns the disk move plus the in-memory rename, so the registry follows it.
            if (!CPackage::RenamePackage(OldPath, NewPath))
            {
                return FPathOpResult::Fail(Lumina::Format("Could not move the package at {}.", OldPath));
            }

            FAssetRegistry::Get().AssetRenamed(OldPath, NewPath);
            FCoreEditorDelegates::OnAssetRenamed.Broadcast(OldPath, NewPath);

            return FPathOpResult::Ok(1);
        }

        FPathOpResult MoveFolder(FStringView OldFolder, FStringView NewFolder)
        {
            struct FFolderEntry
            {
                FFixedString OldPath;
                FFixedString NewPath;
            };

            // Snapshotted before the filesystem moves, since the walk cannot find them afterwards.
            TVector<FFolderEntry> Entries;

            VFS::RecursiveDirectoryIterator(OldFolder, [&](const VFS::FFileInfo& FileInfo)
            {
                if (FileInfo.IsDirectory() || !FileInfo.IsLAsset())
                {
                    return;
                }

                FStringView Old(FileInfo.VirtualPath.data(), FileInfo.VirtualPath.size());
                if (!Old.starts_with(OldFolder))
                {
                    return;
                }

                FFixedString NewPath(NewFolder.data(), NewFolder.size());
                NewPath.append(Old.data() + OldFolder.size(), Old.size() - OldFolder.size());

                Entries.push_back({ FFixedString(Old.data(), Old.size()), Move(NewPath) });
            });

            if (!VFS::Rename(OldFolder, NewFolder))
            {
                return FPathOpResult::Fail(Lumina::Format("Could not move the folder at {}.", OldFolder));
            }

            // Only the directory portion changed, so no package content needs rewriting.
            for (const FFolderEntry& Entry : Entries)
            {
                CPackage::OnPackageMovedExternally(Entry.OldPath, Entry.NewPath);
                FAssetRegistry::Get().AssetRenamed(Entry.OldPath, Entry.NewPath);
                FCoreEditorDelegates::OnAssetRenamed.Broadcast(Entry.OldPath, Entry.NewPath);
            }

            FAssetRegistry::Get().TextAssetFolderRenamed(OldFolder, NewFolder);

            return FPathOpResult::Ok((uint32)Entries.size());
        }

        FPathOpResult MoveLooseFile(FStringView OldPath, FStringView NewPath)
        {
            if (!VFS::Rename(OldPath, NewPath))
            {
                return FPathOpResult::Fail(Lumina::Format("Could not move the file at {}.", OldPath));
            }

            // The sidecar carries a text asset's identity, so references survive only if it comes along.
            if (TextAsset::IsTextAssetPath(OldPath) || TextAsset::IsTextAssetPath(NewPath))
            {
                FAssetRegistry::Get().TextAssetRenamed(OldPath, NewPath);
                FCoreEditorDelegates::OnAssetRenamed.Broadcast(OldPath, NewPath);
                return FPathOpResult::Ok(1);
            }

            return FPathOpResult::Ok();
        }
    }

    bool IsProtectedRoot(FStringView VirtualPath)
    {
        return IEquals(VirtualPath, "/Game")
            || IEquals(VirtualPath, "/Game/Content")
            || IEquals(VirtualPath, "/Game/Scripts")
            || IEquals(VirtualPath, "/Engine/Resources")
            || IEquals(VirtualPath, "/Engine/Resources/Content")
            || IEquals(VirtualPath, "/Engine/Resources/Scripts");
    }

    bool IsAssetLocation(FStringView VirtualPath)
    {
        return IsAtOrUnder(VirtualPath, "/Game/Content") || IsAtOrUnder(VirtualPath, "/Engine/Resources/Content");
    }

    FPathOpResult MovePath(FStringView OldPath, FStringView NewPath)
    {
        if (OldPath.empty() || NewPath.empty())
        {
            return FPathOpResult::Fail("A move needs both a source and a destination.");
        }

        if (OldPath == NewPath)
        {
            return FPathOpResult::Fail("The source and the destination are the same path.");
        }

        if (!VFS::Exists(OldPath))
        {
            return FPathOpResult::Fail(Lumina::Format("Nothing exists at {}.", OldPath));
        }

        if (IsProtectedRoot(OldPath))
        {
            return FPathOpResult::Fail(Lumina::Format("{} is a mount root and cannot be moved.", OldPath));
        }

        if (VFS::Exists(NewPath))
        {
            return FPathOpResult::Fail(Lumina::Format("{} already exists.", NewPath));
        }

        // Moving a folder into itself would recurse forever, and the walk below cannot detect it after the fact.
        if (VFS::IsDirectory(OldPath) && IsAtOrUnder(NewPath, FString(OldPath.data(), OldPath.size()).c_str()))
        {
            return FPathOpResult::Fail("A folder cannot be moved inside itself.");
        }

        const FStringView Parent = VFS::Parent(NewPath, true);
        if (!Parent.empty() && !VFS::Exists(Parent))
        {
            return FPathOpResult::Fail(Lumina::Format("{} does not exist, so nothing can be moved into it.", Parent));
        }

        const FStringView Extension = VFS::Extension(OldPath);

        if (Extension == ".lasset")
        {
            return MoveAssetPackage(OldPath, NewPath);
        }

        if (Extension.empty())
        {
            return MoveFolder(OldPath, NewPath);
        }

        return MoveLooseFile(OldPath, NewPath);
    }

    FPathOpResult CreateFolder(FStringView VirtualPath)
    {
        if (VirtualPath.empty())
        {
            return FPathOpResult::Fail("A folder needs a path.");
        }

        if (VFS::Exists(VirtualPath))
        {
            return FPathOpResult::Fail(Lumina::Format("{} already exists.", VirtualPath));
        }

        const FStringView Parent = VFS::Parent(VirtualPath, true);
        if (!Parent.empty() && !VFS::Exists(Parent))
        {
            return FPathOpResult::Fail(Lumina::Format("{} does not exist, so nothing can be created in it.", Parent));
        }

        if (!VFS::CreateDir(VirtualPath))
        {
            return FPathOpResult::Fail(Lumina::Format("Could not create {}.", VirtualPath));
        }

        return FPathOpResult::Ok();
    }
}
