#include "MCPAssetTools.h"

#include "Agent/AgentToolRegistry.h"
#include "MCPTextMatch.h"
#include "Asset/AssetOps.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/Algorithm.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"

namespace Lumina::MCP
{
    namespace
    {

        // Rejects the traversal and separator tricks that would take a write outside the mounted roots.
        bool IsWellFormedPath(const FString& Path, FString& OutError)
        {
            if (Path.empty() || Path[0] != '/')
            {
                OutError = "A path has to start at a mount root, such as /Game/Content.";
                return false;
            }

            if (Path.find("..") != FString::npos || Path.find("//") != FString::npos)
            {
                OutError = "A path cannot contain .. or an empty step.";
                return false;
            }

            return true;
        }

        bool IsWellFormedName(const FString& Name, FString& OutError)
        {
            if (Name.empty())
            {
                OutError = "A name cannot be empty.";
                return false;
            }

            for (char Character : Name)
            {
                if (Character == '/' || Character == (char)0x5C || Character == ':')
                {
                    OutError = "A name is just the name, so it cannot contain a path separator.";
                    return false;
                }
            }

            return true;
        }

        Agent::FToolResult ReportPathOp(const AssetOps::FPathOpResult& Result, SPathOpResult& Out,
            FStringView Destination, FStringView Verb)
        {
            if (!Result.bSucceeded)
            {
                return Agent::FToolResult::Error(Result.Error);
            }

            Out.Path      = FString(Destination.data(), Destination.size());
            Out.Relocated = (int32)Result.AssetsRelocated;

            return Agent::FToolResult::Ok(Lumina::Format("{} to {}.", Verb, Out.Path));
        }

        void RegisterListFolders(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListFoldersParams, SListFoldersResult>(
                Owner, "assets.list_folders",
                "List content folders, so an agent can see where assets live before moving them.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListFoldersParams& In, SListFoldersResult& Out)
                {
                    const FString Root = In.Under.empty() ? FString("/Game/Content") : In.Under;
                    const int32 Depth = In.Depth > 0 ? In.Depth : 2;

                    FString Error;
                    if (!IsWellFormedPath(Root, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (!VFS::Exists(Root))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Nothing exists at {}.", Root));
                    }

                    const size_t RootDepth = (size_t)Algo::Count(Root, '/');

                    THashMap<FString, int32> Counts;

                    VFS::RecursiveDirectoryIterator(Root, [&](const VFS::FFileInfo& FileInfo)
                    {
                        const FStringView Path(FileInfo.VirtualPath.data(), FileInfo.VirtualPath.size());

                        // The editor keeps its own bookkeeping in dot folders, which are not content.
                        if (Path.find("/.") != FStringView::npos)
                        {
                            return;
                        }

                        const FStringView Folder = FileInfo.IsDirectory() ? Path : VFS::Parent(Path, true);

                        const size_t FolderDepth = (size_t)Algo::Count(Folder, '/');
                        if (FolderDepth > RootDepth + (size_t)Depth)
                        {
                            return;
                        }

                        FString Key(Folder.data(), Folder.size());

                        auto It = Counts.find(Key);
                        if (It == Counts.end())
                        {
                            It = Counts.emplace(Move(Key), 0).first;
                        }

                        if (!FileInfo.IsDirectory() && FileInfo.IsLAsset())
                        {
                            ++It->second;
                        }
                    });

                    for (const auto& Pair : Counts)
                    {
                        SFolderInfo Info;
                        Info.Path       = Pair.first;
                        Info.AssetCount = Pair.second;

                        Out.Folders.push_back(Move(Info));
                    }

                    Algo::Sort(Out.Folders,
                        [](const SFolderInfo& A, const SFolderInfo& B) { return A.Path < B.Path; });

                    return Agent::FToolResult::Ok(Lumina::Format("{} folder(s) under {}.",
                        Out.Folders.size(), Root));
                });
        }

        void RegisterCreateFolder(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateFolderParams, SPathOpResult>(
                Owner, "assets.create_folder",
                "Create a content folder, including any missing folders above it.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateFolderParams& In, SPathOpResult& Out)
                {
                    FString Error;
                    if (!IsWellFormedPath(In.Path, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (!AssetOps::IsAssetLocation(FStringView(In.Path)))
                    {
                        return Agent::FToolResult::Error(
                            "Folders belong under /Game/Content, since nothing scans for assets elsewhere.");
                    }

                    // Each missing level is created in turn, because CreateDir does not build a chain.
                    TVector<FString> Missing;
                    FStringView Walk(In.Path);

                    while (!Walk.empty() && !VFS::Exists(Walk))
                    {
                        Missing.push_back(FString(Walk.data(), Walk.size()));

                        const FStringView Up = VFS::Parent(Walk, true);

                        // Parent hands back an unchanged view at the top, which would spin here forever.
                        if (Up.size() >= Walk.size())
                        {
                            break;
                        }

                        Walk = Up;
                    }

                    if (Missing.empty())
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} already exists.", In.Path));
                    }

                    for (auto It = Missing.rbegin(); It != Missing.rend(); ++It)
                    {
                        const AssetOps::FPathOpResult Result = AssetOps::CreateFolder(FStringView(*It));
                        if (!Result.bSucceeded)
                        {
                            return Agent::FToolResult::Error(Result.Error);
                        }
                    }

                    Out.Path = In.Path;

                    return Agent::FToolResult::Ok(Missing.size() == 1
                        ? Lumina::Format("Created {}.", In.Path)
                        : Lumina::Format("Created {}, along with {} folder(s) above it.",
                            In.Path, Missing.size() - 1));
                });
        }

        void RegisterRenameAsset(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SRenameAssetParams, SPathOpResult>(
                Owner, "assets.rename",
                "Rename an asset or folder in place. References survive, since they resolve by GUID.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SRenameAssetParams& In, SPathOpResult& Out)
                {
                    FString Error;
                    if (!IsWellFormedPath(In.Path, Error) || !IsWellFormedName(In.NewName, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    // The extension names the asset kind, so it is carried over rather than retyped.
                    const FStringView Extension = VFS::Extension(FStringView(In.Path));
                    const FStringView Parent    = VFS::Parent(VFS::RemoveExtension(FStringView(In.Path)), true);

                    FFixedString Destination = Paths::Combine(Parent, FStringView(In.NewName));
                    Destination.append(Extension.data(), Extension.size());

                    const FStringView Final(Destination.c_str(), Destination.size());

                    return ReportPathOp(AssetOps::MovePath(FStringView(In.Path), Final), Out, Final, "Renamed");
                });
        }

        void RegisterMoveAsset(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SMoveAssetParams, SPathOpResult>(
                Owner, "assets.move",
                "Move an asset or folder into another folder. References survive, since they resolve by GUID.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SMoveAssetParams& In, SPathOpResult& Out)
                {
                    FString Error;
                    if (!IsWellFormedPath(In.Path, Error) || !IsWellFormedPath(In.DestinationFolder, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (!VFS::IsDirectory(In.DestinationFolder))
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "{} is not a folder. Use assets.create_folder first.", In.DestinationFolder));
                    }

                    const FStringView Name = VFS::FileName(FStringView(In.Path), false);

                    const FFixedString Destination = Paths::Combine(FStringView(In.DestinationFolder), Name);
                    const FStringView Final(Destination.c_str(), Destination.size());

                    return ReportPathOp(AssetOps::MovePath(FStringView(In.Path), Final), Out, Final, "Moved");
                });
        }

        void RegisterSaveAsset(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSaveAssetParams, SSaveAssetResult>(
                Owner, "assets.save",
                "Write an asset's package to disk, so edits survive a restart.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSaveAssetParams& In, SSaveAssetResult& Out)
                {
                    const TOptional<FGuid> Parsed = FGuid::TryParse(FStringView(In.Asset));
                    if (!Parsed.IsSet())
                    {
                        return Agent::FToolResult::Error("That is not a GUID. Use assets.search to find an asset.");
                    }

                    CObject* Object = StaticLoadObject(*Parsed);
                    if (Object == nullptr)
                    {
                        return Agent::FToolResult::Error("That GUID names no asset that could be loaded.");
                    }

                    CPackage* Package = Object->GetPackage();
                    if (Package == nullptr)
                    {
                        return Agent::FToolResult::Error("That asset has no package to save.");
                    }

                    const FFixedString Path = Package->GetPackagePath();

                    if (!CPackage::SavePackage(Package, Path))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not save {}.", Path));
                    }

                    FAssetRegistry::Get().AssetSaved(Object);

                    Out.Path = FString(Path.c_str());

                    return Agent::FToolResult::Ok(Lumina::Format("Saved {}.", Out.Path));
                });
        }
    }

    void RegisterAssetTools(FStringView Owner)
    {
        RegisterSaveAsset(Owner);
        RegisterListFolders(Owner);
        RegisterCreateFolder(Owner);
        RegisterRenameAsset(Owner);
        RegisterMoveAsset(Owner);

        Agent::FToolRegistry::Get().Register<SSearchAssetsParams, SSearchAssetsResult>(
            Owner,
            "assets.search",
            "Find assets by name, path or class, and report the GUID that component fields take.",
            Agent::EToolEffect::ReadOnly,
            Agent::EToolThread::GameThread,
            [](const SSearchAssetsParams& In, SSearchAssetsResult& Out)
            {
                const int32 Limit = In.Limit > 0 ? In.Limit : 50;

                // Matching runs inside the predicate so a huge project is walked once, not collected twice.
                FAssetRegistry::Get().FindByPredicate([&](const FAssetData& Data)
                {
                    const FString Name(Data.AssetName.ToString().c_str());
                    const FString Path(Data.Path.c_str());
                    const FString Class(Data.AssetClass.ToString().c_str());

                    if (!ContainsTextFold(FStringView(Class), In.AssetClass))
                    {
                        return false;
                    }

                    if (!ContainsTextFold(FStringView(Name), In.Contains) && !ContainsTextFold(FStringView(Path), In.Contains))
                    {
                        return false;
                    }

                    ++Out.Matched;

                    if (static_cast<int32>(Out.Results.size()) < Limit)
                    {
                        SAssetInfo Info;
                        Info.Name  = Name;
                        Info.Path  = Path;
                        Info.AssetClass = Class;
                        Info.Guid  = FString(Data.AssetGUID.ToString().c_str());

                        Out.Results.push_back(Move(Info));
                    }

                    return false;
                });

                if (Out.Matched == 0)
                {
                    return Agent::FToolResult::Ok("No assets matched.");
                }

                return Agent::FToolResult::Ok(Lumina::Format("{} of {} matching asset(s).",
                    Out.Results.size(), Out.Matched));
            });
    }
}
