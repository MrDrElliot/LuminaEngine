#include "MCPAssetTools.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentObjectEdit.h"
#include "Agent/AgentPropertyPath.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "MCPServer.h"
#include "MCPTextMatch.h"
#include "Asset/AssetOps.h"
#include "Assets/AssetEvents.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/Factories/Factory.h"
#include "Containers/Algorithm.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Session/SessionOps.h"
#include "Tools/Import/Importer.h"
#include "Tools/Import/TextureImporter.h"
#include "UI/Tools/EditorAssetActions.h"

namespace Lumina::MCP
{
    namespace MCPAssetToolsInternal
    {
        constexpr const char* GAssetSaveHint = " Save with assets.save to persist.";

        // Rejects the traversal and separator tricks that would take a write outside the mounted roots.
        bool IsWellFormedAssetPath(const FString& Path, FString& OutError)
        {
            if (Path.IsEmpty() || Path[0] != '/')
            {
                OutError = "A path has to start at a mount root, such as /Game/Content.";
                return false;
            }

            if (Path.Find("..") != Constants::kIndexNone || Path.Find("//") != Constants::kIndexNone)
            {
                OutError = "A path cannot contain .. or an empty step.";
                return false;
            }

            return true;
        }

        bool IsWellFormedAssetName(const FString& Name, FString& OutError)
        {
            return AssetOps::IsValidLeafName(FStringView(*Name, Name.Len()), &OutError);
        }

        SAssetInfo DescribeAssetData(const FAssetData& Data)
        {
            SAssetInfo Info;
            Info.Name       = FString(*Data.AssetName.ToString());
            Info.Path       = FString(*Data.Path);
            Info.AssetClass = FString(*Data.AssetClass.ToString());
            Info.Guid       = FString(*Data.AssetGUID.ToString());
            return Info;
        }

        void CollectAssetReferencers(const FGuid& Guid, TVector<SAssetInfo>& Out)
        {
            for (const FAssetData* Data : FAssetRegistry::Get().GetReferencersOf(Guid))
            {
                if (Data != nullptr)
                {
                    Out.Add(DescribeAssetData(*Data));
                }
            }
        }

        // Each missing level is created in turn, because CreateDir does not build a chain.
        bool CreateAssetFolderChain(FStringView Path, size_t& OutCreated, FString& OutError)
        {
            TVector<FString> Missing;
            FStringView Walk(Path);

            while (!Walk.IsEmpty() && !VFS::Exists(Walk))
            {
                Missing.Emplace(Walk.GetData(), Walk.Len());

                const FStringView Up = VFS::Parent(Walk, true);

                // Parent hands back an unchanged view at the top, which would spin here forever.
                if (Up.Len() >= Walk.Len())
                {
                    break;
                }

                Walk = Up;
            }

            for (auto It = Missing.rbegin(); It != Missing.rend(); ++It)
            {
                const AssetOps::FPathOpResult Result = AssetOps::CreateFolder(FStringView(*It));
                if (!Result.bSucceeded)
                {
                    OutError = Result.Error;
                    return false;
                }
            }

            OutCreated = Missing.Num();
            return true;
        }

        // The extension names the asset kind, so a rename carries it over rather than taking it retyped.
        FFixedString AssetMoveDestination(const SMoveAssetParams& In)
        {
            const FStringView Source(In.Path);
            const FStringView Folder = In.DestinationFolder.IsEmpty()
                ? VFS::Parent(VFS::RemoveExtension(Source), true)
                : FStringView(In.DestinationFolder);

            if (In.NewName.IsEmpty())
            {
                return Paths::Combine(Folder, VFS::FileName(Source, false));
            }

            const FStringView Extension = VFS::Extension(Source);
            FFixedString Destination = Paths::Combine(Folder, FStringView(In.NewName));
            Destination.Append(Extension.GetData(), Extension.Len());
            return Destination;
        }

        // Settings names only fields of the importer class, so one call can carry keys for several formats.
        bool ApplyAssetImporterSettings(CImporter* Importer, const FJsonValue& Settings, FString& OutError)
        {
            for (const FJsonMember& Setting : Settings.Members())
            {
                Agent::FResolvedProperty Target;
                FString Ignored;
                if (!Agent::ResolvePropertyPath(Importer->GetClass(), Importer, Setting.Key.View(), Target, Ignored))
                {
                    continue;
                }
                const Agent::FMarshalResult Result = Agent::ReadProperty(Setting.Value, Target.Property, Target.ValuePtr,
                    Setting.Key.View());
                if (!Result.IsValid())
                {
                    OutError = Result.Error;
                    return false;
                }
            }
            return true;
        }

        bool ReimportExistingAsset(CImporter* Importer, const FFixedString& Destination, const FString& SourcePath,
            SImportAssetsResult& Out, const FString& FileName)
        {
            const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(FStringView(*Destination, Destination.Len()));
            if (Data == nullptr)
            {
                return false;
            }
            CObject* Asset = LoadObject<CObject>(Data->AssetGUID);
            if (Asset == nullptr || !Importer->CanReimport(Asset->GetClass()))
            {
                return false;
            }
            // Reimport reads the importer's parsed source, as the content browser's reimport does after its parse step.
            const FImportRequest Request{ FFixedString(*SourcePath), FFixedString() };
            FString ParseError;
            const bool bReimported = Importer->ParseSource(Request, ParseError, nullptr) && Importer->ReimportAsset(Asset, Request, nullptr);
            Importer->ReleaseSourceData();
            if (!bReimported)
            {
                Out.Failed.Add(FileName + ": the reimport failed and the asset was left unchanged" + (ParseError.IsEmpty() ? FString() : FString(" (") + ParseError + ")"));
                return true;
            }
            CPackage* Package = Asset->GetPackage();
            if (Package == nullptr || !CPackage::SavePackage(Package, Package->GetPackagePath()))
            {
                Out.Failed.Add(FileName + ": reimported in memory but could not be saved");
                return true;
            }
            FAssetRegistry::Get().AssetSaved(Asset);
            AssetEvents::BroadcastAssetDataChanged(Asset);
            Out.Imported.Emplace(*Destination);
            return true;
        }

        void ImportAssetFile(FStringView File, const FString& Folder, const SImportAssetsParams& In,
            const FJsonValue& Settings, SImportAssetsResult& Out)
        {
            const FString SourcePath(File);
            const FString FileName(VFS::FileName(File));

            CImporter* ImporterCDO = CImporterRegistry::Get().FindImporterForExtension(VFS::Extension(File));
            if (ImporterCDO == nullptr)
            {
                return;
            }

            // Sanitized as CPackage::CreatePackage names it, so a non-ASCII file is found again on a rerun.
            const FFixedString Destination = SanitizeObjectName(Paths::Combine(FStringView(Folder), VFS::FileName(File, true)));
            FFixedString OnDisk = Destination;
            CPackage::AddPackageExt(OnDisk);
            if (!In.ReplaceExisting && (FindObject<CPackage>(Destination) != nullptr || VFS::Exists(OnDisk)))
            {
                Out.Skipped++;
                return;
            }

            CImporter* Importer = CImporterRegistry::CreateImporterOfClass(ImporterCDO->GetClass());
            if (CTextureImporter* TextureImporter = Cast<CTextureImporter>(Importer))
            {
                TextureImporter->Group = In.TextureGroup;
            }

            FString SettingsError;
            if (!ApplyAssetImporterSettings(Importer, Settings, SettingsError))
            {
                CImporterRegistry::DestroyImporter(Importer);
                Out.Failed.Add(FileName + ": " + SettingsError);
                return;
            }

            // Updating in place, as the content browser's reimport does, keeps the GUID and every reference to it.
            if (In.ReplaceExisting && ReimportExistingAsset(Importer, Destination, SourcePath, Out, FileName))
            {
                CImporterRegistry::DestroyImporter(Importer);
                return;
            }

            // Importers with a settings step read the source here, and building without it produces nothing.
            const FImportRequest Request{ FFixedString(*SourcePath), Destination, In.ReplaceExisting };
            FImportResult Result;
            FString ParseError;
            if (Importer->ParseSource(Request, ParseError, nullptr))
            {
                Importer->BuildAssets(Request, Result, nullptr);
            }
            else
            {
                Result.Error = ParseError.IsEmpty() ? FString("The source could not be read.") : ParseError;
            }
            Importer->ReleaseSourceData();

            // A file holding several meshes names each File_Mesh, so the caller is told the paths that really exist.
            TVector<FString> CreatedPaths;
            for (const TStrongObjectPtr<CObject>& Created : Result.CreatedObjects)
            {
                if (Created.IsValid() && Created->GetPackage() != nullptr)
                {
                    CreatedPaths.Emplace(*Created->GetPackage()->GetPackagePath());
                }
            }

            Result.Finish(Request);
            CImporterRegistry::DestroyImporter(Importer);

            if (Result.Succeeded())
            {
                if (CreatedPaths.IsEmpty())
                {
                    Out.Imported.Emplace(*Destination);
                }
                for (FString& Path : CreatedPaths)
                {
                    Out.Imported.Add(Move(Path));
                }
            }
            else
            {
                Out.Failed.Add(FileName + ": " + Result.Error);
            }
        }

        void ImportAssetDirectory(FStringView Directory, const FString& Folder, const SImportAssetsParams& In,
            const FJsonValue& Settings, SImportAssetsResult& Out)
        {
            Filesystem::IterateDirectory(Directory, [&](const Filesystem::FDirectoryEntry& Entry)
            {
                if (!Entry.IsDirectory())
                {
                    ImportAssetFile(Entry.FullPath, Folder, In, Settings, Out);
                    return;
                }

                if (!In.Recursive)
                {
                    return;
                }

                const FString SubFolder = Folder + "/" + FString(Entry.Name);
                if (!VFS::Exists(FStringView(SubFolder)) && !AssetOps::CreateFolder(FStringView(SubFolder)).bSucceeded)
                {
                    Out.Failed.Add(SubFolder + ": could not create folder");
                    return;
                }

                ImportAssetDirectory(Entry.FullPath, SubFolder, In, Settings, Out);
            });
        }

        void RegisterAssetsSearch(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSearchAssetsParams, SSearchAssetsResult>(
                Owner, "assets.search",
                "Find assets by name, path or class, and report the GUID that component fields take.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SSearchAssetsParams& In, SSearchAssetsResult& Out)
                {
                    const int32 Limit = In.Limit > 0 ? In.Limit : 50;
                    const int32 Offset = In.Offset > 0 ? In.Offset : 0;

                    // Matching runs inside the predicate so a huge project is walked once, not collected twice.
                    FAssetRegistry::Get().FindByPredicate([&](const FAssetData& Data)
                    {
                        const FString Name(*Data.AssetName.ToString());
                        const FString Path(*Data.Path);
                        const FString Class(*Data.AssetClass.ToString());

                        if (!ContainsTextFold(FStringView(Class), In.AssetClass))
                        {
                            return false;
                        }

                        if (!ContainsTextFold(FStringView(Name), In.Contains) && !ContainsTextFold(FStringView(Path), In.Contains))
                        {
                            return false;
                        }

                        ++Out.Matched;

                        if (Out.Matched > Offset && Out.Results.Num() < Limit)
                        {
                            Out.Results.Add(DescribeAssetData(Data));
                        }

                        return false;
                    });

                    if (Out.Matched == 0)
                    {
                        return Agent::FToolResult::Ok("No assets matched.");
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} of {} matching asset(s).",
                        Out.Results.Num(), Out.Matched));
                });
        }

        void RegisterAssetsListFolders(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListFoldersParams, SListFoldersResult>(
                Owner, "assets.list_folders",
                "List content folders and how many assets each holds directly.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListFoldersParams& In, SListFoldersResult& Out)
                {
                    const FString Root = In.Under.IsEmpty() ? FString("/Game/Content") : In.Under;
                    const int32 Depth = In.Depth > 0 ? In.Depth : 2;

                    FString Error;
                    if (!IsWellFormedAssetPath(Root, Error))
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
                        const FStringView Path(FileInfo.VirtualPath.GetData(), FileInfo.VirtualPath.Len());

                        // The editor keeps its own bookkeeping in dot folders, which are not content.
                        if (Path.Find("/.") != Constants::kIndexNone)
                        {
                            return;
                        }

                        const FStringView Folder = FileInfo.IsDirectory() ? Path : VFS::Parent(Path, true);

                        const size_t FolderDepth = (size_t)Algo::Count(Folder, '/');
                        if (FolderDepth > RootDepth + (size_t)Depth)
                        {
                            return;
                        }

                        FString Key(Folder.GetData(), Folder.Len());

                        auto It = Counts.Find(Key);
                        if (It == nullptr)
                        {
                            It = &Counts.FindOrAdd(Move(Key), 0);
                        }

                        if (!FileInfo.IsDirectory() && FileInfo.IsLAsset())
                        {
                            ++(*It);
                        }
                    });

                    for (const auto& [Path, AssetCount] : Counts)
                    {
                        SFolderInfo Info;
                        Info.Path       = Path;
                        Info.AssetCount = AssetCount;

                        Out.Folders.Add(Move(Info));
                    }

                    Algo::Sort(Out.Folders,
                        [](const SFolderInfo& A, const SFolderInfo& B) { return A.Path < B.Path; });

                    return Agent::FToolResult::Ok(Lumina::Format("{} folder(s) under {}.",
                        Out.Folders.Num(), Root));
                });
        }

        void RegisterAssetsCreateFolder(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAssetPathParams, SPathOpResult>(
                Owner, "assets.create_folder",
                "Create a content folder under /Game/Content, including any missing folders above it.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAssetPathParams& In, SPathOpResult& Out)
                {
                    FString Error;
                    if (!IsWellFormedAssetPath(In.Path, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (!AssetOps::IsAssetLocation(FStringView(In.Path)))
                    {
                        return Agent::FToolResult::Error(
                            "Folders belong under /Game/Content, since nothing scans for assets elsewhere.");
                    }

                    size_t Missing = 0;
                    if (!CreateAssetFolderChain(FStringView(In.Path), Missing, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (Missing == 0)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} already exists.", In.Path));
                    }

                    Out.Path = In.Path;

                    return Agent::FToolResult::Ok(Missing == 1
                        ? Lumina::Format("Created {}.", In.Path)
                        : Lumina::Format("Created {}, along with {} folder(s) above it.",
                            In.Path, Missing - 1));
                });
        }

        void RegisterAssetsMove(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SMoveAssetParams, SPathOpResult>(
                Owner, "assets.move",
                "Move an asset or folder into DestinationFolder, rename it to NewName, or both. "
                "References survive, since they resolve by GUID.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SMoveAssetParams& In, SPathOpResult& Out)
                {
                    if (In.DestinationFolder.IsEmpty() && In.NewName.IsEmpty())
                    {
                        return Agent::FToolResult::Error("Set DestinationFolder, NewName or both.");
                    }

                    FString Error;
                    if (!IsWellFormedAssetPath(In.Path, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (!In.NewName.IsEmpty() && !IsWellFormedAssetName(In.NewName, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (!In.DestinationFolder.IsEmpty())
                    {
                        if (!IsWellFormedAssetPath(In.DestinationFolder, Error))
                        {
                            return Agent::FToolResult::Error(Error);
                        }

                        if (!VFS::IsDirectory(In.DestinationFolder))
                        {
                            return Agent::FToolResult::Error(Lumina::Format(
                                "{} is not a folder. Use assets.create_folder first.", In.DestinationFolder));
                        }
                    }

                    const FFixedString Destination = AssetMoveDestination(In);
                    const FStringView Final(*Destination, Destination.Len());

                    const AssetOps::FPathOpResult Result = AssetOps::MovePath(FStringView(In.Path), Final);
                    if (!Result.bSucceeded)
                    {
                        return Agent::FToolResult::Error(Result.Error);
                    }

                    Out.Path      = FString(Final.GetData(), Final.Len());
                    Out.Relocated = (int32)Result.AssetsRelocated;

                    return Agent::FToolResult::Ok(Lumina::Format("{} to {}.",
                        In.DestinationFolder.IsEmpty() ? "Renamed" : "Moved", Out.Path));
                });
        }

        void RegisterAssetsDuplicate(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAssetPathParams, SPathOpResult>(
                Owner, "assets.duplicate",
                "Copy an asset beside itself as Name_Copy, as the Content Browser's Duplicate does, and save the copy.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAssetPathParams& In, SPathOpResult& Out)
                {
                    FString Error;
                    if (!IsWellFormedAssetPath(In.Path, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FFixedString NewPath;
                    if (DuplicateAsset(FStringView(In.Path), NewPath) == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not duplicate {}.", In.Path));
                    }

                    Out.Path = *NewPath;
                    return Agent::FToolResult::Ok(Lumina::Format("Duplicated to {}.", Out.Path));
                });
        }

        void RegisterAssetsSave(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAssetRefParams, SSaveAssetResult>(
                Owner, "assets.save",
                "Write an asset's package to disk, so edits survive a restart.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAssetRefParams& In, SSaveAssetResult& Out)
                {
                    CObject* Object = nullptr;
                    FString Error;
                    if (!Agent::ResolveAssetObject(FStringView(In.Asset), Object, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (!SessionOps::SaveAsset(Object, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Path = FString(*Object->GetPackage()->GetPackagePath());

                    return Agent::FToolResult::Ok(Lumina::Format("Saved {}.", Out.Path));
                });
        }

        void RegisterAssetsImport(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SImportAssetsParams, SImportAssetsResult>(
                Owner, "assets.import",
                "Import a source file, or every importable file in a directory, into a content folder. "
                "Existing assets are skipped unless ReplaceExisting, so a rerun resumes an import that timed out.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SImportAssetsParams& In, SImportAssetsResult& Out)
                {
                    FString Error;
                    if (!IsWellFormedAssetPath(In.DestinationFolder, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (!AssetOps::IsAssetLocation(FStringView(In.DestinationFolder)))
                    {
                        return Agent::FToolResult::Error(
                            "Assets belong under /Game/Content, since nothing scans for them elsewhere.");
                    }

                    const FFixedString Source = Paths::Normalize(FStringView(In.Source));
                    if (!Filesystem::Exists(Source))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} does not exist.", In.Source));
                    }

                    FJsonValue Settings = FJsonValue::MakeObject();
                    if (!In.Settings.IsEmpty())
                    {
                        if (!FJsonValue::Parse(In.Settings, Settings) || !Settings.IsObject())
                        {
                            return Agent::FToolResult::Error("Settings must be a JSON object of importer properties.");
                        }
                    }

                    size_t Created = 0;
                    if (!CreateAssetFolderChain(FStringView(In.DestinationFolder), Created, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (Filesystem::IsDirectory(Source))
                    {
                        ImportAssetDirectory(Source, In.DestinationFolder, In, Settings, Out);
                    }
                    else
                    {
                        ImportAssetFile(Source, In.DestinationFolder, In, Settings, Out);
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Imported {}, skipped {}, failed {}.",
                        Out.Imported.Num(), Out.Skipped, Out.Failed.Num()));
                });
        }

        // A class is creatable when a factory claims it or an ancestor of it; there is no abstract flag to ask.
        void RegisterAssetsListClasses(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListAssetClassesParams, SListAssetClassesResult>(
                Owner, "assets.list_classes",
                "List every asset class assets.create can make.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListAssetClassesParams& In, SListAssetClassesResult& Out)
                {
                    auto List = [&In, &Out](CClass* Class, CFactory* Factory)
                    {
                        SAssetClassInfo Info;
                        Info.Name                 = FString(*Class->GetName().ToString());
                        Info.Category             = Factory->GetCategory();
                        Info.bHasCreationDialogue = Factory->HasCreationDialogue();

                        if (ContainsTextFold(FStringView(Info.Name), In.Contains)
                            || ContainsTextFold(FStringView(Info.Category), In.Contains))
                        {
                            Out.Classes.Add(Move(Info));
                        }
                    };

                    for (CFactory* Factory : CFactoryRegistry::Get().GetFactories())
                    {
                        CClass* Class = Factory != nullptr ? Factory->GetAssetClass() : nullptr;
                        if (Class == nullptr)
                        {
                            continue;
                        }
                        List(Class, Factory);

                        // A factory for a base such as CDataAsset also makes every subclass, script ones included.
                        for (TObjectIterator<CClass> It; It; ++It)
                        {
                            if (*It != Class && (*It)->IsChildOf(Class) && CFactoryRegistry::Get().FindFactory(*It) == Factory)
                            {
                                List(*It, Factory);
                            }
                        }
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} creatable asset class(es).", Out.Classes.Num()));
                });
        }

        void RegisterAssetsCreate(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateAssetParams, SAssetInfo>(
                Owner, "assets.create",
                "Create an empty asset of any creatable class and save it to disk.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateAssetParams& In, SAssetInfo& Out)
                {
                    if (In.Name.IsEmpty())
                    {
                        return Agent::FToolResult::Error("An asset needs a name.");
                    }

                    FString Error;
                    CClass* Class = Agent::FindClassByName(FStringView(In.ClassName), nullptr, Error);
                    if (Class == nullptr)
                    {
                        return Agent::FToolResult::Error(Error + " Call assets.list_classes.");
                    }

                    CFactory* Factory = CFactoryRegistry::Get().FindFactory(Class);
                    if (Factory == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "{} is not a creatable asset class. Call assets.list_classes.", In.ClassName));
                    }

                    if (!AssetOps::IsAssetLocation(FStringView(In.Folder)))
                    {
                        return Agent::FToolResult::Error(
                            "Assets belong under /Game/Content, since nothing scans for assets elsewhere.");
                    }

                    if (!VFS::IsDirectory(In.Folder))
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "{} is not a folder. Use assets.create_folder first.", In.Folder));
                    }

                    FFixedString Path = Paths::Combine(FStringView(In.Folder), FStringView(In.Name));
                    CPackage::AddPackageExt(Path);

                    if (VFS::Exists(Path))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} already exists.", Path));
                    }

                    FString CreateError;
                    const TStrongObjectPtr<CObject> Asset = CFactory::CreateAsset(Factory, Class, FStringView(*Path, Path.Len()), CreateError);
                    if (!Asset)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} at {}: {}.", In.ClassName, Path, CreateError));
                    }

                    Out.Name       = FString(*Asset->GetName().ToString());
                    Out.Path       = FString(*Path);
                    Out.AssetClass = In.ClassName;
                    Out.Guid       = FString(*Asset->GetGUID().ToString());

                    return Agent::FToolResult::Ok(Lumina::Format("Created {}.", Out.Path));
                });
        }

        void RegisterAssetsDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAssetRefParams, SDescribeAssetResult>(
                Owner, "assets.describe",
                "Report an asset's class, every reflected field, and what references it.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SAssetRefParams& In, SDescribeAssetResult& Out)
                {
                    CObject* Asset = nullptr;
                    FString Error;
                    if (!Agent::ResolveAssetObject(FStringView(In.Asset), Asset, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FJsonValue Properties;
                    if (const Agent::FMarshalResult Written = Agent::WriteStruct(Asset->GetClass(), Asset, Properties);
                        !Written.IsValid())
                    {
                        return Agent::FToolResult::Error(Written.Error);
                    }

                    Out.Name       = FString(*Asset->GetName().ToString());
                    Out.ClassName  = FString(*Asset->GetClass()->GetName().ToString());
                    Out.Guid       = FString(*Asset->GetGUID().ToString());
                    Out.Properties = Properties.ToString(2);

                    if (CPackage* Package = Asset->GetPackage())
                    {
                        Out.Path = FString(*Package->GetPackagePath());
                    }

                    Agent::CollectUnwritableFields(Asset->GetClass(), FStringView(), Out.UnwritableFields);
                    CollectAssetReferencers(Asset->GetGUID(), Out.Referencers);

                    return Agent::FToolResult::Ok(Lumina::Format("{} ({})\n{}", Out.Name, Out.ClassName, Out.Properties));
                });
        }

        void RegisterAssetsSetProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetAssetPropertyParams, SAssetPropertyResult>(
                Owner, "assets.set_property",
                "Set one field on an asset. Undoable only while the asset is open in an editor.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetAssetPropertyParams& In, SAssetPropertyResult& Out)
                {
                    CObject* Asset = nullptr;
                    FString Error;
                    if (!ResolveEditableAsset(FStringView(In.Asset), Asset, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FJsonValue Value;
                    if (!FJsonValue::Parse(In.Value, Value))
                    {
                        return Agent::FToolResult::Error(
                            "Value is not JSON. Strings need quotes, so \"Torch\" rather than Torch.");
                    }

                    const Agent::FObjectEditResult Edit = Agent::SetObjectProperty(
                        Asset->GetClass(), Asset, Asset, FStringView(In.Path), Value, "Set Asset Property (agent)");

                    if (!Edit.IsValid())
                    {
                        return Agent::FToolResult::Error(Edit.Error);
                    }

                    Out.Previous  = Edit.Previous;
                    Out.Current   = Edit.Current;
                    Out.bUndoable = Edit.bUndoable;

                    return Agent::FToolResult::Ok(Lumina::Format("{}.{} is now {}.{}{}",
                        Asset->GetName(), In.Path, Out.Current, GAssetSaveHint,
                        Edit.bUndoable ? "" : " Applied without undo (no editor open for this asset)."));
                });
        }

        void RegisterAssetsDelete(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDeleteAssetParams, SDeleteAssetResult>(
                Owner, "assets.delete",
                "Delete an asset from disk. Not undoable. Refuses while other assets reference it unless bForce.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SDeleteAssetParams& In, SDeleteAssetResult& Out)
                {
                    const TOptional<FGuid> Guid = Agent::ParseAssetGuid(FStringView(In.Asset));
                    if (!Guid.IsSet())
                    {
                        return Agent::FToolResult::Error(Lumina::Format("'{}' is neither a GUID nor an asset path.", In.Asset));
                    }

                    // The registry knows the path without loading, which a file about to go should not need.
                    const FAssetData* Data = FAssetRegistry::Get().GetAssetByGUID(*Guid);
                    if (Data == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No asset has GUID {}.", In.Asset));
                    }

                    Out.Path = FString(*Data->Path);
                    CollectAssetReferencers(*Guid, Out.Referencers);

                    if (!Out.Referencers.IsEmpty() && !In.bForce)
                    {
                        return Agent::FToolResult::Ok(Lumina::Format(
                            "{} asset(s) reference {}. Pass bForce to delete anyway.", Out.Referencers.Num(), Out.Path));
                    }

                    const AssetOps::FDeleteAssetResult Deleted =
                        AssetOps::DeleteAsset(FStringView(Out.Path), SessionOps::GetToolContext());
                    if (!Deleted.bDeleted)
                    {
                        return Agent::FToolResult::Error(Deleted.Error);
                    }

                    Out.bDeleted = true;
                    return Agent::FToolResult::Ok(Lumina::Format("Deleted {}.", Out.Path));
                });
        }
    }

    using namespace MCPAssetToolsInternal;

    void RegisterAssetTools(FStringView Owner)
    {
        RegisterAssetsSearch(Owner);
        RegisterAssetsListFolders(Owner);
        RegisterAssetsCreateFolder(Owner);
        RegisterAssetsMove(Owner);
        RegisterAssetsDuplicate(Owner);
        RegisterAssetsSave(Owner);
        RegisterAssetsImport(Owner);
        RegisterAssetsListClasses(Owner);
        RegisterAssetsCreate(Owner);
        RegisterAssetsDescribe(Owner);
        RegisterAssetsSetProperty(Owner);
        RegisterAssetsDelete(Owner);
    }
}
