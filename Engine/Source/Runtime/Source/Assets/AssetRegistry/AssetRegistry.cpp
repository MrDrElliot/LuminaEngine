#include "RuntimePCH.h"
#include "AssetRegistry.h"

#include "TextAssetSidecar.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/Package/Package.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "Core/Serialization/Archiver.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Core/Serialization/Structured/JsonStructuredArchive.h"

#include "nlohmann/json.hpp"

#include <chrono>
#include <filesystem>
#include "Log/Log.h"

namespace Lumina
{
    // Tag for the cooked-runtime registry blob bundled into the PAK.
    static constexpr uint32 kAssetRegistryCacheTag     = 0xA55E1DB2; // 'AssetIDB2'

    // Schema version for the human-readable on-disk editor cache.
    static constexpr int32 kAssetRegistryJsonVersion   = 2;

    FAssetRegistry& FAssetRegistry::Get()
    {
        static FAssetRegistry Registry;
        return Registry;
    }

    namespace
    {
        FString AssetDbPath()
        {
            const FString& Install = Paths::GetEngineInstallDirectory();
            if (Install.empty()) return {};
            FString Out = Install;
            Out += "/Intermediates/AssetRegistry.json";
            return Out;
        }

        int64 FileMTimeNanos(FStringView VirtualPath)
        {
            // Resolve virtual -> absolute and stat via std::filesystem (VFS has no uniform mtime accessor).
            const FFixedString Resolved = VFS::ResolvePath(VirtualPath);
            if (Resolved.empty())
            {
                return 0;
            }
            std::error_code Ec;
            const auto Ftime = std::filesystem::last_write_time(
                std::filesystem::path(Resolved.c_str(), Resolved.c_str() + Resolved.size()), Ec);
            if (Ec)
            {
                return 0;
            }
            const auto Dur = Ftime.time_since_epoch();
            return std::chrono::duration_cast<std::chrono::nanoseconds>(Dur).count();
        }

        // Quick xxh64 of the on-disk asset bytes. Reads via VFS so it works
        // for any mounted alias (including plugin /<PluginName>/Content).
        uint64 ContentHashOf(FStringView VirtualPath, TVector<uint8>* OutBytes = nullptr)
        {
            TVector<uint8> Bytes;
            if (!VFS::ReadFile(Bytes, VirtualPath))
            {
                return 0;
            }
            const uint64 H = Hash::XXHash::GetHash64(Bytes.data(), Bytes.size());
            if (OutBytes)
            {
                *OutBytes = Move(Bytes);
            }
            return H;
        }

        // Recognize /<Alias>/... and check if Alias matches a known plugin
        // name. Returns the plugin name (without slash) or empty.
        FName ExtractOwningPlugin(FStringView VirtualPath)
        {
            if (VirtualPath.empty() || VirtualPath[0] != '/')
            {
                return FName();
            }
            size_t SecondSlash = VirtualPath.find('/', 1);
            if (SecondSlash == FStringView::npos)
            {
                return FName();
            }
            FStringView Alias = VirtualPath.substr(1, SecondSlash - 1);
            if (Alias == "Game" || Alias == "Engine" || Alias == "Config")
            {
                return FName();
            }
            if (FPluginManager::Get().FindPlugin(Alias) != nullptr)
            {
                return FName(Alias);
            }
            return FName();
        }
    }

    void FAssetRegistry::RunInitialDiscovery()
    {
        LUMINA_PROFILE_SCOPE();

        // Load cached registry first; the discovery pass below only
        // touches entries whose mtime/content changed.
        const bool bHadCache = LoadCache();
        if (!bHadCache)
        {
            ClearAssets();
        }

        TVector<FFixedString> PackagePaths;
        TVector<FFixedString> WalkedRoots;
        PackagePaths.reserve(256);
        WalkedRoots.reserve(8);

        auto Callback = [&](const VFS::FFileInfo& File)
        {
            if (File.IsDirectory())
            {
                return;
            }
            if (File.IsLAsset())
            {
                PackagePaths.emplace_back(File.VirtualPath);
            }
        };

        WalkedRoots.emplace_back(FFixedString("/Engine/Resources/Content"));
        VFS::RecursiveDirectoryIterator("/Engine/Resources/Content", Callback);

        WalkedRoots.emplace_back(FFixedString("/Game/Content"));
        VFS::RecursiveDirectoryIterator("/Game/Content", Callback);
        
        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (!Plugin->IsEnabled())
            {
                continue;
            }
            if (!Plugin->IsContentMounted())
            {
                continue;
            }
            const FString MountAlias = Plugin->GetMountAlias();
            WalkedRoots.emplace_back(FFixedString(MountAlias.c_str()));
            VFS::RecursiveDirectoryIterator(MountAlias, Callback);
        }

        // Snapshot so OnInitialDiscoveryCompleted can reap cache entries
        // whose files no longer exist under a walked root.
        LastDiscoveryWalkedRoots  = WalkedRoots;
        LastDiscoveryVisitedPaths = PackagePaths;
        eastl::sort(LastDiscoveryVisitedPaths.begin(), LastDiscoveryVisitedPaths.end());
        
        RunTextAssetDiscovery();

        const uint32 NumPackages = (uint32)PackagePaths.size();
        if (NumPackages == 0)
        {
            OnInitialDiscoveryCompleted();
            return;
        }

        Task::AsyncTask(NumPackages, NumPackages, [this, PackagePaths = Move(PackagePaths)] (uint32 Start, uint32 End, uint32)
        {
            for (uint32 i = Start; i < End; ++i)
            {
                ProcessPackagePath(PackagePaths[i]);
            }

            if (End == PackagePaths.size())
            {
                OnInitialDiscoveryCompleted();
            }
        });
    }

    void FAssetRegistry::OnInitialDiscoveryCompleted()
    {
        // Drop cache entries whose underlying files vanished between sessions
        // before we hand the registry out or persist it.
        ReapStaleEntries();

        ImGuiX::Notifications::NotifySuccess("Asset Registry Finished Initial Discovery: Num [{}]", Assets.size());
        LOG_INFO("Asset Registry Finished Initial Discovery: Num [{}]", Assets.size());

        // Persist the cache so next launch only re-parses changed assets; skipped if the path won't resolve.
        SaveCache();

        // Reverse map gets built lazily on first GetReferencersOf().
        {
            FWriteScopeLock Lock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        DispatchRegistryChanged();
    }

    bool FAssetRegistry::NeedsReextract(FStringView Path, int64 MTimeNs, uint64 ContentHash) const
    {
        FReadScopeLock Lock(AssetsMutex);
        for (const TUniquePtr<FAssetData>& Data : Assets)
        {
            if (Data->Path == Path)
            {
                // Mtime is the cheap predicate; content hash is the truth.
                // Pass either-or as 0 to force re-extract.
                if (MTimeNs == 0 || ContentHash == 0) return true;
                return Data->SourceMTimeNs != MTimeNs || Data->ContentHash != ContentHash;
            }
        }
        return true; // not yet in registry
    }

    void FAssetRegistry::SuspendBroadcasts()
    {
        BroadcastSuspendCount.fetch_add(1, std::memory_order_relaxed);
    }

    void FAssetRegistry::ResumeBroadcasts()
    {
        // Last suspender out fires the single coalesced broadcast iff something flagged a change while suspended.
        if (BroadcastSuspendCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            if (bBroadcastPending.exchange(false, std::memory_order_relaxed))
            {
                DispatchRegistryChanged();
            }
        }
    }

    void FAssetRegistry::DispatchRegistryChanged()
    {
        // Every subscriber to this is main-thread-only: the content browser rebuilds its tile tree, the
        // editor reopens asset tabs and loads worlds, the thumbnail manager queues renders. Asset creation
        // runs on a task fiber during import, so broadcasting inline ran all of that concurrently with the
        // main thread drawing the very structures it rebuilds.
        if (Threading::IsMainThread())
        {
            OnAssetRegistryUpdated.Broadcast();
            return;
        }

        MainThread::Enqueue([]
        {
            FAssetRegistry::Get().OnAssetRegistryUpdated.Broadcast();
        });
    }

    void FAssetRegistry::NotifyRegistryChanged()
    {
        if (BroadcastSuspendCount.load(std::memory_order_relaxed) > 0)
        {
            bBroadcastPending.store(true, std::memory_order_relaxed);
            return;
        }

        DispatchRegistryChanged();
    }

    FScopedAssetRegistryBatch::FScopedAssetRegistryBatch()
    {
        FAssetRegistry::Get().SuspendBroadcasts();
    }

    FScopedAssetRegistryBatch::~FScopedAssetRegistryBatch()
    {
        FAssetRegistry::Get().ResumeBroadcasts();
    }

    void FAssetRegistry::AssetCreated(const CObject* Asset)
    {
        FFixedString FilePath = Asset->GetPackage()->GetPackagePath();

        auto AssetData = MakeUnique<FAssetData>();
        AssetData->AssetClass    = Asset->GetClass()->GetName();
        AssetData->AssetGUID     = Asset->GetGUID();
        AssetData->AssetName     = Asset->GetName();
        AssetData->Path          = Move(FilePath);
        AssetData->OwningPlugin  = ExtractOwningPlugin(AssetData->Path);

        {
            FWriteScopeLock Lock(AssetsMutex);
            Assets.emplace(Move(AssetData));
        }

        {
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        NotifyRegistryChanged();
    }

    void FAssetRegistry::AssetDeleted(const FGuid& GUID)
    {
        {
            FWriteScopeLock Lock(AssetsMutex);

            auto It = Assets.find_as(GUID, FGuidHash(), FAssetDataGuidEqual());
            if (It == Assets.end())
            {
                LOG_WARN("AssetRegistry::AssetDeleted: GUID not present in registry; ignoring");
                return;
            }

            Assets.erase(It);
        }

        {
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        NotifyRegistryChanged();
    }

    void FAssetRegistry::AssetRenamed(FStringView OldPath, FStringView NewPath)
    {
        {
            FWriteScopeLock Lock(AssetsMutex);

            auto It = eastl::find_if(Assets.begin(), Assets.end(), [&OldPath](const TUniquePtr<FAssetData>& Asset)
            {
                return Asset->Path == OldPath;
            });

            if (It == Assets.end())
            {
                LOG_WARN("AssetRegistry::AssetRenamed: no entry for {}; rename of {} -> {} not reflected in registry until next discovery", OldPath, OldPath, NewPath);
                return;
            }

            // Drop any stale entry already at NewPath (different GUID), else GetAssetByPath is non-deterministic.
            const FGuid RenamedGuid = (*It)->AssetGUID;
            auto Colliding = eastl::find_if(Assets.begin(), Assets.end(), [&](const TUniquePtr<FAssetData>& Asset)
            {
                return Asset->AssetGUID != RenamedGuid && Asset->Path == NewPath;
            });
            if (Colliding != Assets.end())
            {
                LOG_WARN("AssetRegistry::AssetRenamed: dropping stale entry at {} colliding with rename {} -> {}", NewPath, OldPath, NewPath);
                Assets.erase(Colliding);
                // hash_set::erase can invalidate other iterators; re-find.
                It = eastl::find_if(Assets.begin(), Assets.end(), [&OldPath](const TUniquePtr<FAssetData>& Asset)
                {
                    return Asset->Path == OldPath;
                });
                if (It == Assets.end()) return;
            }

            const TUniquePtr<FAssetData>& Data = *It;
            Data->Path.assign_convert(NewPath);
            Data->AssetName    = VFS::FileName(NewPath, true);
            Data->OwningPlugin = ExtractOwningPlugin(NewPath);
        }

        NotifyRegistryChanged();
    }

    void FAssetRegistry::AssetSaved(CObject* Asset)
    {
        // Re-extract the saved package: its ImportTable + content hash may
        // have changed. Reverse map invalidated.
        FFixedString Path = Asset->GetPackage()->GetPackagePath();
        ProcessPackagePath(Path);

        {
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        NotifyRegistryChanged();
    }

    FAssetData* FAssetRegistry::GetAssetByGUID(const FGuid& GUID) const
    {
        FReadScopeLock Lock(AssetsMutex);

        auto It = eastl::find_if(Assets.begin(), Assets.end(), [&](const auto& Data)
        {
            return Data->AssetGUID == GUID;
        });

        return It == Assets.end() ? nullptr : It->get();
    }

    FAssetData* FAssetRegistry::GetAssetByPath(FStringView Path) const
    {
        FReadScopeLock Lock(AssetsMutex);

        FStringView PathNoExt = VFS::RemoveExtension(Path);
        auto It = eastl::find_if(Assets.begin(), Assets.end(), [&](const TUniquePtr<FAssetData>& Data)
        {
            return VFS::RemoveExtension(Data->Path) == PathNoExt;
        });

        return It == Assets.end() ? nullptr : It->get();
    }

    TVector<FAssetData*> FAssetRegistry::FindByPredicate(const TFunction<bool(const FAssetData&)>& Predicate)
    {
        FReadScopeLock Lock(AssetsMutex);

        TVector<FAssetData*> Datas;
        Datas.reserve(Assets.size() / 2);
        for (const TUniquePtr<FAssetData>& Data : Assets)
        {
            if (Predicate(*Data))
            {
                Datas.emplace_back(Data.get());
            }
        }

        return Datas;
    }

    // --- Text assets -------------------------------------------------------------------------------

    void FAssetRegistry::RunTextAssetDiscovery()
    {
        LUMINA_PROFILE_SCOPE();

        TVector<FFixedString> Roots;
        Roots.emplace_back(FFixedString("/Engine/Resources/Content"));
        Roots.emplace_back(FFixedString("/Game/Content"));
        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (!Plugin->IsEnabled())        continue;
            if (!Plugin->IsContentMounted()) continue;
            Roots.emplace_back(FFixedString(Plugin->GetMountAlias().c_str()));
        }

        FTextAssetMap Rebuilt;

        auto Callback = [&](const VFS::FFileInfo& File)
        {
            if (File.IsDirectory()) return;
            const FStringView Vp(File.VirtualPath.c_str(), File.VirtualPath.size());
            if (TextAssetSidecar::IsSidecarPath(Vp)) return;

            const ETextAssetKind Kind = TextAsset::KindFromPath(Vp);
            if (Kind == ETextAssetKind::None) return;

            const FGuid Guid = TextAssetSidecar::ReadOrMint(Vp, Kind);
            if (!Guid.IsValid()) return;

            auto Data = MakeUnique<FTextAssetData>();
            Data->Guid          = Guid;
            Data->Path          .assign_convert(Vp);
            Data->Name          = VFS::FileName(Vp, true);
            Data->Kind          = Kind;
            Data->OwningPlugin  = ExtractOwningPlugin(Vp);
            Data->SourceMTimeNs = FileMTimeNanos(Vp);

            // Two files sharing a stale GUID (e.g. a copy-paste before re-discovery) would collide; the
            // first one wins and the duplicate gets re-minted next pass once its sidecar is regenerated.
            if (Rebuilt.find_as(Guid, FGuidHash(), FTextAssetGuidEqual()) == Rebuilt.end())
            {
                Rebuilt.emplace(Move(Data));
            }
        };

        for (const FFixedString& Root : Roots)
        {
            VFS::RecursiveDirectoryIterator(FStringView(Root.c_str(), Root.size()), Callback);
        }

        FWriteScopeLock Lock(TextAssetsMutex);
        TextAssets = Move(Rebuilt);
    }

    FGuid FAssetRegistry::EnsureTextAsset(FStringView Path)
    {
        if (FTextAssetData* Existing = GetTextAssetByPath(Path))
        {
            return Existing->Guid;
        }

        const ETextAssetKind Kind = TextAsset::KindFromPath(Path);
        if (Kind == ETextAssetKind::None)
        {
            return FGuid();
        }

        const FGuid Guid = TextAssetSidecar::ReadOrMint(Path, Kind);
        if (!Guid.IsValid())
        {
            return FGuid();
        }

        auto Data = MakeUnique<FTextAssetData>();
        Data->Guid          = Guid;
        Data->Path          .assign_convert(Path);
        Data->Name          = VFS::FileName(Path, true);
        Data->Kind          = Kind;
        Data->OwningPlugin  = ExtractOwningPlugin(Path);
        Data->SourceMTimeNs = FileMTimeNanos(Path);

        FWriteScopeLock Lock(TextAssetsMutex);
        if (TextAssets.find_as(Guid, FGuidHash(), FTextAssetGuidEqual()) == TextAssets.end())
        {
            TextAssets.emplace(Move(Data));
        }
        return Guid;
    }

    FTextAssetData* FAssetRegistry::GetTextAssetByGUID(const FGuid& GUID) const
    {
        FReadScopeLock Lock(TextAssetsMutex);
        auto It = TextAssets.find_as(GUID, FGuidHash(), FTextAssetGuidEqual());
        return It == TextAssets.end() ? nullptr : It->get();
    }

    FTextAssetData* FAssetRegistry::GetTextAssetByPath(FStringView Path) const
    {
        FReadScopeLock Lock(TextAssetsMutex);
        const FStringView PathNoExt = VFS::RemoveExtension(Path);
        auto It = eastl::find_if(TextAssets.begin(), TextAssets.end(), [&](const TUniquePtr<FTextAssetData>& Data)
        {
            return VFS::RemoveExtension(FStringView(Data->Path.c_str(), Data->Path.size())) == PathNoExt;
        });
        return It == TextAssets.end() ? nullptr : It->get();
    }

    TVector<FTextAssetData*> FAssetRegistry::GetTextAssetsOfKind(ETextAssetKind Kind) const
    {
        FReadScopeLock Lock(TextAssetsMutex);
        TVector<FTextAssetData*> Out;
        for (const TUniquePtr<FTextAssetData>& Data : TextAssets)
        {
            if (Kind == ETextAssetKind::None || Data->Kind == Kind)
            {
                Out.push_back(Data.get());
            }
        }
        return Out;
    }

    void FAssetRegistry::TextAssetCreated(FStringView Path)
    {
        EnsureTextAsset(Path);
        NotifyRegistryChanged();
    }

    void FAssetRegistry::TextAssetRenamed(FStringView OldPath, FStringView NewPath)
    {
        // Relocate the sidecar first so the GUID travels with the file.
        TextAssetSidecar::Move(OldPath, NewPath);

        bool bRenamed = false;
        {
            FWriteScopeLock Lock(TextAssetsMutex);

            auto It = eastl::find_if(TextAssets.begin(), TextAssets.end(), [&](const TUniquePtr<FTextAssetData>& Data)
            {
                return FStringView(Data->Path.c_str(), Data->Path.size()) == OldPath;
            });
            if (It == TextAssets.end())
            {
                return;
            }

            // Drop a stale entry already sitting at NewPath with a different GUID.
            const FGuid RenamedGuid = (*It)->Guid;
            auto Colliding = eastl::find_if(TextAssets.begin(), TextAssets.end(), [&](const TUniquePtr<FTextAssetData>& Data)
            {
                return Data->Guid != RenamedGuid && FStringView(Data->Path.c_str(), Data->Path.size()) == NewPath;
            });
            if (Colliding != TextAssets.end())
            {
                TextAssets.erase(Colliding);
                It = eastl::find_if(TextAssets.begin(), TextAssets.end(), [&](const TUniquePtr<FTextAssetData>& Data)
                {
                    return FStringView(Data->Path.c_str(), Data->Path.size()) == OldPath;
                });
                if (It == TextAssets.end()) return;
            }

            const TUniquePtr<FTextAssetData>& Data = *It;
            Data->Path.assign_convert(NewPath);
            Data->Name         = VFS::FileName(NewPath, true);
            Data->Kind         = TextAsset::KindFromPath(NewPath);
            Data->OwningPlugin = ExtractOwningPlugin(NewPath);
            bRenamed = true;
        }

        if (bRenamed)
        {
            // Outside the lock: subscribers (open file editors) may read the registry back.
            FCoreDelegates::OnContentFileRenamed.Broadcast(OldPath, NewPath);
            NotifyRegistryChanged();
        }
    }

    void FAssetRegistry::TextAssetDeleted(FStringView Path)
    {
        TextAssetSidecar::Delete(Path);

        {
            FWriteScopeLock Lock(TextAssetsMutex);
            auto It = eastl::find_if(TextAssets.begin(), TextAssets.end(), [&](const TUniquePtr<FTextAssetData>& Data)
            {
                return FStringView(Data->Path.c_str(), Data->Path.size()) == Path;
            });
            if (It != TextAssets.end())
            {
                TextAssets.erase(It);
            }
        }

        NotifyRegistryChanged();
    }

    void FAssetRegistry::TextAssetFolderRenamed(FStringView OldDir, FStringView NewDir)
    {
        // Snapshot the affected (old) paths first; mutate sidecars + entries outside the iteration.
        TVector<FFixedString> OldPaths;
        {
            FReadScopeLock Lock(TextAssetsMutex);
            for (const TUniquePtr<FTextAssetData>& Data : TextAssets)
            {
                const FStringView P(Data->Path.c_str(), Data->Path.size());
                if (VFS::IsUnderDirectory(OldDir, P))
                {
                    OldPaths.emplace_back(Data->Path);
                }
            }
        }

        for (const FFixedString& Old : OldPaths)
        {
            const FStringView OldView(Old.c_str(), Old.size());
            // new = NewDir + (Old - OldDir)
            FStringView Tail = OldView.substr(OldDir.size());
            FFixedString NewPath(NewDir.data(), NewDir.size());
            NewPath.append(Tail.data(), Tail.size());
            TextAssetRenamed(OldView, FStringView(NewPath.c_str(), NewPath.size()));
        }
    }

    void FAssetRegistry::ReapStaleEntries()
    {
        if (LastDiscoveryWalkedRoots.empty())
        {
            return;
        }

        FWriteScopeLock Lock(AssetsMutex);

        size_t Reaped = 0;
        for (auto It = Assets.begin(); It != Assets.end(); )
        {
            const FFixedString& Path = (*It)->Path;
            const FStringView PathView(Path.c_str(), Path.size());

            bool bUnderWalkedRoot = false;
            for (const FFixedString& Root : LastDiscoveryWalkedRoots)
            {
                const FStringView RootView(Root.c_str(), Root.size());
                if (PathView.starts_with(RootView))
                {
                    bUnderWalkedRoot = true;
                    break;
                }
            }
            if (!bUnderWalkedRoot)
            {
                ++It;
                continue;
            }

            const bool bVisited = eastl::binary_search(
                LastDiscoveryVisitedPaths.begin(),
                LastDiscoveryVisitedPaths.end(),
                Path);

            if (!bVisited)
            {
                It = Assets.erase(It);
                ++Reaped;
            }
            else
            {
                ++It;
            }
        }

        if (Reaped > 0)
        {
            LOG_INFO("AssetRegistry: reaped {} cached entries whose files no longer exist", Reaped);
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        LastDiscoveryWalkedRoots.clear();
        LastDiscoveryVisitedPaths.clear();
    }

    void FAssetRegistry::RebuildReverseMap()
    {
        // Caller holds ReverseMapMutex write lock.
        ReverseDepMap.clear();
        FReadScopeLock AssetsLock(AssetsMutex);
        for (const TUniquePtr<FAssetData>& Data : Assets)
        {
            for (const FAssetDependency& Dep : Data->Dependencies)
            {
                ReverseDepMap[Dep.TargetGUID].push_back(Data->AssetGUID);
            }
        }
        bReverseMapDirty = false;
    }

    TVector<FAssetData*> FAssetRegistry::GetReferencersOf(const FGuid& GUID) const
    {
        // Two-phase: rebuild map under write lock if dirty, then read under
        // shared lock. Const-cast is fine, map cache is mutable state.
        {
            FWriteScopeLock RLock(ReverseMapMutex);
            if (bReverseMapDirty)
            {
                const_cast<FAssetRegistry*>(this)->RebuildReverseMap();
            }
        }

        TVector<FAssetData*> Result;
        {
            FReadScopeLock RLock(ReverseMapMutex);
            auto It = ReverseDepMap.find(GUID);
            if (It == ReverseDepMap.end()) return Result;

            FReadScopeLock ALock(AssetsMutex);
            Result.reserve(It->second.size());
            for (const FGuid& ReferrerGuid : It->second)
            {
                auto AIt = Assets.find_as(ReferrerGuid, FGuidHash(), FAssetDataGuidEqual());
                if (AIt != Assets.end())
                {
                    Result.push_back(AIt->get());
                }
            }
        }
        return Result;
    }

    void FAssetRegistry::ProcessPackagePath(FStringView Path)
    {
        // Pre-check mtime + content hash vs cache; hash is over raw compressed bytes, so source or
        // compression-setting changes invalidate it.
        const int64 MTime = FileMTimeNanos(Path);
        TVector<uint8> RawBytes;
        const uint64 Hash = ContentHashOf(Path, &RawBytes);

        if (Hash != 0 && !NeedsReextract(Path, MTime, Hash))
        {
            return; // cache hit
        }

        if (RawBytes.empty())
        {
            LOG_ERROR("AssetRegistry: failed to read {}", Path);
            RecordFailedAsset(Path);
            return;
        }

        // ReadPackageFile decompresses the deflate-with-FCompressedPackageHeader file into Bytes,
        // which holds the FPackageHeader and import/export tables.
        TVector<uint8> Bytes;
        if (!CPackage::ReadPackageFile(Path, Bytes))
        {
            LOG_ERROR("AssetRegistry: failed to decompress {}", Path);
            RecordFailedAsset(Path);
            return;
        }

        if (Bytes.size() < sizeof(FPackageHeader))
        {
            LOG_ERROR("AssetRegistry: {} is too small to be a valid package", Path);
            RecordFailedAsset(Path);
            return;
        }

        FName PackageFileName = VFS::FileName(Path, true);

        FPackageHeader Header;
        FMemoryReader Reader(Bytes);
        Reader << Header;

        if (Header.Tag != PACKAGE_FILE_TAG)
        {
            LOG_ERROR("AssetRegistry: {} is not a valid Lumina package (tag mismatch)", Path);
            RecordFailedAsset(Path);
            return;
        }

        if (Header.Version > GPackageFileLuminaVersion.FileVersion)
        {
            LOG_ERROR("AssetRegistry: {} was saved with engine version {} (current {}); cannot register files from a newer engine", Path, Header.Version, GPackageFileLuminaVersion.FileVersion);
            RecordFailedAsset(Path);
            return;
        }

        Reader.SetFileVersion(Header.Version);

        if (Header.ExportTableOffset < 0 || static_cast<size_t>(Header.ExportTableOffset) > Bytes.size())
        {
            LOG_ERROR("AssetRegistry: {} has out-of-range export table offset", Path);
            RecordFailedAsset(Path);
            return;
        }

        Reader.Seek(Header.ExportTableOffset);

        TVector<FObjectExport> Exports;
        Reader << Exports;

        FObjectExport* Export = eastl::find_if(Exports.begin(), Exports.end(), [&](const FObjectExport& E)
        {
            return E.ObjectName == PackageFileName;
        });

        if (Export == Exports.end())
        {
            LOG_ERROR("AssetRegistry: {} contains no export matching its file name; refusing to register", Path);
            RecordFailedAsset(Path);
            return;
        }

        // Each ImportTable entry is one outbound edge with the type the saver recorded (Hard for direct
        // CObject* refs, Soft for FSoftObjectPath).
        TVector<FAssetDependency> Dependencies;
        if (Header.ImportTableOffset >= 0
            && static_cast<size_t>(Header.ImportTableOffset) <= Bytes.size())
        {
            Reader.Seek(Header.ImportTableOffset);
            TVector<FObjectImport> Imports;
            Reader << Imports;
            Dependencies.reserve(Imports.size());
            for (const FObjectImport& Import : Imports)
            {
                FAssetDependency Dep;
                Dep.TargetGUID = Import.ObjectGUID;
                Dep.Type       = Import.Type;
                Dependencies.emplace_back(Dep);
            }
        }

        auto AssetData = MakeUnique<FAssetData>();
        AssetData->AssetClass     = Export->ClassName;
        AssetData->AssetGUID      = Export->ObjectGUID;
        AssetData->AssetName      = Export->ObjectName;
        AssetData->Path           .assign_convert(Path);
        AssetData->ContentHash    = Hash;
        AssetData->SourceMTimeNs  = MTime;
        AssetData->Dependencies   = Move(Dependencies);
        AssetData->OwningPlugin   = ExtractOwningPlugin(Path);

        FWriteScopeLock Lock(AssetsMutex);
        // Drop any pre-existing entry with this GUID (external move/rename: path changed, GUID stable);
        // else the dup-GUID collision rejects the new entry and leaves a dangling old path.
        auto ExistingByGuid = Assets.find_as(AssetData->AssetGUID, FGuidHash(), FAssetDataGuidEqual());
        if (ExistingByGuid != Assets.end())
        {
            Assets.erase(ExistingByGuid);
        }
        // Then drop any stale entry at this path with a different GUID (rare:
        // user dropped a .lasset with a fresh GUID over an old one).
        auto ExistingByPath = eastl::find_if(Assets.begin(), Assets.end(), [&](const TUniquePtr<FAssetData>& D)
        {
            return D->Path == AssetData->Path;
        });
        if (ExistingByPath != Assets.end())
        {
            Assets.erase(ExistingByPath);
        }

        Assets.emplace(Move(AssetData));
    }

    void FAssetRegistry::RecordFailedAsset(FStringView Path)
    {
        FWriteScopeLock Lock(FailedAssetsMutex);
        FailedAssets.emplace_back(Path.data(), Path.size());
    }

    void FAssetRegistry::ClearAssets()
    {
        // Scoped so every lock is released before the broadcast. Listeners read the registry straight back
        // (the content browser rebuilds from it), and AssetsMutex is not recursive.
        {
            FWriteScopeLock Lock(AssetsMutex);
            Assets.clear();

            {
                FWriteScopeLock TextLock(TextAssetsMutex);
                TextAssets.clear();
            }

            {
                FWriteScopeLock RLock(ReverseMapMutex);
                ReverseDepMap.clear();
                bReverseMapDirty = false;
            }
        }

        DispatchRegistryChanged();
    }

    void FAssetRegistry::WriteToArchive(FArchive& Ar) const
    {
        uint32 Tag = kAssetRegistryCacheTag;
        Ar << Tag;

        FReadScopeLock Lock(AssetsMutex);
        uint32 Count = (uint32)Assets.size();
        Ar << Count;

        for (const TUniquePtr<FAssetData>& Data : Assets)
        {
            Ar << const_cast<FGuid&>(Data->AssetGUID);
            Ar << const_cast<FFixedString&>(Data->Path);
            Ar << const_cast<FName&>(Data->AssetName);
            Ar << const_cast<FName&>(Data->AssetClass);
            Ar << const_cast<uint64&>(Data->ContentHash);
            Ar << const_cast<int64&>(Data->SourceMTimeNs);
            uint32 Flags = (uint32)Data->Flags;
            Ar << Flags;

            uint32 DepCount = (uint32)Data->Dependencies.size();
            Ar << DepCount;
            for (const FAssetDependency& Dep : Data->Dependencies)
            {
                Ar << const_cast<FGuid&>(Dep.TargetGUID);
                uint8 T = (uint8)Dep.Type;
                Ar << T;
            }

            Ar << const_cast<FName&>(Data->OwnerChunk);
            Ar << const_cast<FName&>(Data->OwningPlugin);
        }

        // Text-asset identity table (v2+).
        FReadScopeLock TextLock(TextAssetsMutex);
        uint32 TextCount = (uint32)TextAssets.size();
        Ar << TextCount;
        for (const TUniquePtr<FTextAssetData>& Data : TextAssets)
        {
            Ar << const_cast<FGuid&>(Data->Guid);
            Ar << const_cast<FFixedString&>(Data->Path);
            Ar << const_cast<FName&>(Data->Name);
            uint8 Kind = (uint8)Data->Kind;
            Ar << Kind;
            Ar << const_cast<FName&>(Data->OwningPlugin);
            Ar << const_cast<int64&>(Data->SourceMTimeNs);
        }
    }

    bool FAssetRegistry::LoadFromArchive(FArchive& Ar)
    {
        uint32 Tag = 0;
        Ar << Tag;
        if (Tag != kAssetRegistryCacheTag)
        {
            return false;
        }

        uint32 Count = 0;
        Ar << Count;

        FWriteScopeLock Lock(AssetsMutex);
        Assets.clear();
        Assets.reserve(Count);

        for (uint32 i = 0; i < Count; ++i)
        {
            auto Data = MakeUnique<FAssetData>();
            Ar << Data->AssetGUID;
            Ar << Data->Path;
            Ar << Data->AssetName;
            Ar << Data->AssetClass;
            Ar << Data->ContentHash;
            Ar << Data->SourceMTimeNs;
            uint32 Flags = 0;
            Ar << Flags;
            Data->Flags = (EAssetFlags)Flags;

            uint32 DepCount = 0;
            Ar << DepCount;
            Data->Dependencies.resize(DepCount);
            for (uint32 d = 0; d < DepCount; ++d)
            {
                Ar << Data->Dependencies[d].TargetGUID;
                uint8 T = 0;
                Ar << T;
                Data->Dependencies[d].Type = (EDependencyType)T;
            }

            Ar << Data->OwnerChunk;
            Ar << Data->OwningPlugin;

            Assets.emplace(Move(Data));
        }

        // Text-asset identity table (v2+).
        {
            FWriteScopeLock TextLock(TextAssetsMutex);
            TextAssets.clear();

            uint32 TextCount = 0;
            Ar << TextCount;
            TextAssets.reserve(TextCount);
            for (uint32 i = 0; i < TextCount; ++i)
            {
                auto Data = MakeUnique<FTextAssetData>();
                Ar << Data->Guid;
                Ar << Data->Path;
                Ar << Data->Name;
                uint8 Kind = 0;
                Ar << Kind;
                Data->Kind = (ETextAssetKind)Kind;
                Ar << Data->OwningPlugin;
                Ar << Data->SourceMTimeNs;

                if (TextAssets.find_as(Data->Guid, FGuidHash(), FTextAssetGuidEqual()) == TextAssets.end())
                {
                    TextAssets.emplace(Move(Data));
                }
            }
        }

        {
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        return true;
    }

    // The editor cache is JSON (inspectable/diffable by hand) driven through the engine's
    // FJsonStructuredArchive; the cooked PAK registry stays compact binary (see WriteToArchive).
    // The leaf archive handles primitives/FName/FString directly; FGuid and FFixedString have no
    // leaf overload, so they round-trip through an FString (GUID as text, path as a plain string).
    namespace
    {
        void SerializeGuidField(FArchiveRecord& Rec, FName Field, FGuid& Guid, bool bLoading)
        {
            FArchiveSlot Slot = Rec.EnterField(Field);
            if (bLoading)
            {
                FString S;
                Slot.Serialize(S);
                Guid = FGuid();
                if (auto Parsed = FGuid::TryParse(FStringView(S.c_str(), S.size())))
                {
                    Guid = *Parsed;
                }
            }
            else
            {
                FString S = Guid.ToString();
                Slot.Serialize(S);
            }
        }

        void SerializePathField(FArchiveRecord& Rec, FName Field, FFixedString& Path, bool bLoading)
        {
            FArchiveSlot Slot = Rec.EnterField(Field);
            if (bLoading)
            {
                FString S;
                Slot.Serialize(S);
                Path.assign_convert(FStringView(S.c_str(), S.size()));
            }
            else
            {
                FString S(Path.c_str());
                Slot.Serialize(S);
            }
        }

        void SerializeAssetEntry(FArchiveRecord& Rec, FAssetData& Data, bool bLoading)
        {
            SerializeGuidField(Rec, "guid", Data.AssetGUID, bLoading);
            SerializePathField(Rec, "path", Data.Path, bLoading);
            Rec << StructuredArchive::TNamedValue<FName>("name", Data.AssetName);
            Rec << StructuredArchive::TNamedValue<FName>("class", Data.AssetClass);
            Rec << StructuredArchive::TNamedValue<uint64>("contentHash", Data.ContentHash);
            Rec << StructuredArchive::TNamedValue<int64>("mtime", Data.SourceMTimeNs);

            uint32 Flags = (uint32)Data.Flags;
            Rec << StructuredArchive::TNamedValue<uint32>("flags", Flags);
            if (bLoading) Data.Flags = (EAssetFlags)Flags;

            Rec << StructuredArchive::TNamedValue<FName>("ownerChunk", Data.OwnerChunk);
            Rec << StructuredArchive::TNamedValue<FName>("owningPlugin", Data.OwningPlugin);

            FArchiveSlot DepsSlot = Rec.EnterField("dependencies");
            int32 DepCount = (int32)Data.Dependencies.size();
            FArchiveArray DepArray = DepsSlot.EnterArray(DepCount);
            if (bLoading) Data.Dependencies.resize(DepCount);
            for (int32 i = 0; i < DepCount; ++i)
            {
                FArchiveSlot ElementSlot = DepArray.EnterElement();
                FArchiveRecord DepRec = ElementSlot.EnterRecord();
                SerializeGuidField(DepRec, "guid", Data.Dependencies[i].TargetGUID, bLoading);
                uint8 Type = (uint8)Data.Dependencies[i].Type;
                DepRec << StructuredArchive::TNamedValue<uint8>("type", Type);
                if (bLoading) Data.Dependencies[i].Type = (EDependencyType)Type;
            }
        }

        void SerializeTextEntry(FArchiveRecord& Rec, FTextAssetData& Data, bool bLoading)
        {
            SerializeGuidField(Rec, "guid", Data.Guid, bLoading);
            SerializePathField(Rec, "path", Data.Path, bLoading);
            Rec << StructuredArchive::TNamedValue<FName>("name", Data.Name);

            uint8 Kind = (uint8)Data.Kind;
            Rec << StructuredArchive::TNamedValue<uint8>("kind", Kind);
            if (bLoading) Data.Kind = (ETextAssetKind)Kind;

            Rec << StructuredArchive::TNamedValue<FName>("owningPlugin", Data.OwningPlugin);
            Rec << StructuredArchive::TNamedValue<int64>("mtime", Data.SourceMTimeNs);
        }
    }

    void FAssetRegistry::SaveCache() const
    {
        const FString CachePath = AssetDbPath();
        if (CachePath.empty()) return;

        std::error_code Ec;
        std::filesystem::create_directories(
            std::filesystem::path(CachePath.c_str()).parent_path(), Ec);

        nlohmann::json Root = nlohmann::json::object();
        {
            FJsonStructuredArchive Archive(Root, /*bLoading*/ false);
            FArchiveRecord RootRecord = Archive.Open().EnterRecord();

            int32 Version = kAssetRegistryJsonVersion;
            RootRecord << StructuredArchive::TNamedValue<int32>("version", Version);

            {
                FReadScopeLock Lock(AssetsMutex);
                FArchiveSlot AssetsSlot = RootRecord.EnterField("assets");
                int32 Count = (int32)Assets.size();
                FArchiveArray AssetsArray = AssetsSlot.EnterArray(Count);
                for (const TUniquePtr<FAssetData>& Data : Assets)
                {
                    FArchiveSlot ElementSlot = AssetsArray.EnterElement();
                    FArchiveRecord EntryRecord = ElementSlot.EnterRecord();
                    SerializeAssetEntry(EntryRecord, *Data, /*bLoading*/ false);
                }
            }

            {
                FReadScopeLock TextLock(TextAssetsMutex);
                FArchiveSlot TextSlot = RootRecord.EnterField("textAssets");
                int32 Count = (int32)TextAssets.size();
                FArchiveArray TextArray = TextSlot.EnterArray(Count);
                for (const TUniquePtr<FTextAssetData>& Data : TextAssets)
                {
                    FArchiveSlot ElementSlot = TextArray.EnterElement();
                    FArchiveRecord EntryRecord = ElementSlot.EnterRecord();
                    SerializeTextEntry(EntryRecord, *Data, /*bLoading*/ false);
                }
            }
        }

        const std::string Dumped = Root.dump(2);
        TVector<uint8> Bytes;
        Bytes.assign(
            reinterpret_cast<const uint8*>(Dumped.data()),
            reinterpret_cast<const uint8*>(Dumped.data() + Dumped.size()));

        if (!FileHelper::SaveArrayToFile(Bytes, CachePath))
        {
            LOG_WARN("AssetRegistry: failed to write cache to {}", CachePath);
        }
    }

    bool FAssetRegistry::LoadCache()
    {
        const FString CachePath = AssetDbPath();
        if (CachePath.empty()) return false;

        // First-launch / fresh-clone: no cache yet. Quiet exit; full rescan is the correct fallback.
        std::error_code Ec;
        if (!std::filesystem::exists(std::filesystem::path(CachePath.c_str()), Ec))
        {
            return false;
        }

        TVector<uint8> Bytes;
        if (!FileHelper::LoadFileToArray(Bytes, CachePath)) return false;
        if (Bytes.empty()) return false;

        const char* Begin = reinterpret_cast<const char*>(Bytes.data());
        nlohmann::json Root = nlohmann::json::parse(Begin, Begin + Bytes.size(), nullptr, /*allow_exceptions*/ false);
        if (Root.is_discarded() || !Root.is_object())
        {
            LOG_INFO("AssetRegistry: cache at {} is malformed JSON; rebuilding from scratch", CachePath);
            return false;
        }

        FJsonStructuredArchive Archive(Root, /*bLoading*/ true);
        FArchiveRecord RootRecord = Archive.Open().EnterRecord();

        int32 Version = 0;
        RootRecord << StructuredArchive::TNamedValue<int32>("version", Version);
        if (Version != kAssetRegistryJsonVersion)
        {
            LOG_INFO("AssetRegistry: cache at {} is stale (version {} != {}); rebuilding from scratch", CachePath, Version, kAssetRegistryJsonVersion);
            return false;
        }

        {
            FWriteScopeLock Lock(AssetsMutex);
            Assets.clear();

            FArchiveSlot AssetsSlot = RootRecord.EnterField("assets");
            int32 Count = 0;
            FArchiveArray AssetsArray = AssetsSlot.EnterArray(Count);
            Assets.reserve(Count);
            for (int32 i = 0; i < Count; ++i)
            {
                FArchiveSlot ElementSlot = AssetsArray.EnterElement();
                FArchiveRecord EntryRecord = ElementSlot.EnterRecord();
                auto Data = MakeUnique<FAssetData>();
                SerializeAssetEntry(EntryRecord, *Data, /*bLoading*/ true);
                Assets.emplace(Move(Data));
            }
        }

        {
            FWriteScopeLock TextLock(TextAssetsMutex);
            TextAssets.clear();

            FArchiveSlot TextSlot = RootRecord.EnterField("textAssets");
            int32 Count = 0;
            FArchiveArray TextArray = TextSlot.EnterArray(Count);
            TextAssets.reserve(Count);
            for (int32 i = 0; i < Count; ++i)
            {
                FArchiveSlot ElementSlot = TextArray.EnterElement();
                FArchiveRecord EntryRecord = ElementSlot.EnterRecord();
                auto Data = MakeUnique<FTextAssetData>();
                SerializeTextEntry(EntryRecord, *Data, /*bLoading*/ true);

                if (TextAssets.find_as(Data->Guid, FGuidHash(), FTextAssetGuidEqual()) == TextAssets.end())
                {
                    TextAssets.emplace(Move(Data));
                }
            }
        }

        {
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        LOG_INFO("AssetRegistry: loaded {} entries from cache {}", Assets.size(), CachePath);
        return true;
    }
}
