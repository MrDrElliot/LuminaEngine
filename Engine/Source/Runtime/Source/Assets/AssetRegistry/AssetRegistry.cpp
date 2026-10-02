#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include <string>
#include "AssetRegistry.h"

#include "TextAssetSidecar.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Engine/Engine.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/Package/Package.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "FileSystem/FileSystem.h"
#include "Memory/MemoryTracking.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Tools/UI/ImGui/ImGuiX.h"


#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Log/Log.h"

namespace Lumina
{
    // Tag for the cooked-runtime registry blob bundled into the PAK.
    static constexpr uint32 kAssetRegistryCacheTag     = 0xA55E1DB2; // 'AssetIDB2'

    // Schema version for the human-readable on-disk editor cache.

    FAssetRegistry& FAssetRegistry::Get()
    {
        static FAssetRegistry Registry;
        return Registry;
    }

    namespace
    {
        // Per project, since one shared cache made every project switch reap and re-extract the other's entries.
        FString AssetDbPath()
        {
            FString Out;
            if (GEngine != nullptr && !GEngine->GetProjectPath().empty())
            {
                const FStringView Project = GEngine->GetProjectPath();
                Out.assign(Project.data(), Project.size());
            }
            else
            {
                Out = Paths::GetEngineInstallDirectory();
            }
            if (Out.empty()) return {};
            while (!Out.empty() && Out.back() == '/')
            {
                Out.pop_back();
            }
            Out += "/Intermediates/AssetRegistry.bin";
            return Out;
        }

        struct FDiscoveredPackage
        {
            FFixedString Path;
            int64        MTimeNs = 0;
            uint64       FileSize = 0;
        };

        int64 FileMTimeNanos(FStringView VirtualPath)
        {
            const FPathString Resolved = VFS::ResolvePath(VirtualPath);
            return Resolved.empty() ? 0 : Filesystem::LastWriteTime(Resolved);
        }

        // Read through the VFS so it works for any mounted alias, including a plugin's content.
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

        // Returns the plugin name without the slash, or empty when the alias is not a plugin.
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
        LUMINA_MEMORY_SCOPE("Asset Registry");
        LUMINA_PROFILE_SCOPE();

        DiscoveryStartCycles = PlatformTime::Cycles();
        DiscoveryUnchanged.store(0, std::memory_order_relaxed);
        DiscoveryRehashed.store(0, std::memory_order_relaxed);
        DiscoveryExtracted.store(0, std::memory_order_relaxed);

        // The discovery pass below only touches entries whose mtime or content changed.
        const bool bHadCache = LoadCache();
        bDiscoveryHadCache = bHadCache;
        if (!bHadCache)
        {
            ClearAssets();
        }
        DiscoveryCacheLoadMs = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - DiscoveryStartCycles);
        const uint64 WalkStart = PlatformTime::Cycles();

        TVector<FDiscoveredPackage> Packages;
        TVector<FFixedString> WalkedRoots;
        Packages.reserve(256);
        WalkedRoots.reserve(8);

        auto Callback = [&](const VFS::FFileInfo& File)
        {
            if (File.IsDirectory())
            {
                return;
            }
            if (File.IsLAsset())
            {
                Packages.push_back(FDiscoveredPackage{ FFixedString(File.VirtualPath.c_str(), File.VirtualPath.size()), File.LastModifyTime, File.Size });
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

        // Snapshotted so completion can reap cache entries whose files no longer exist under a walked root.
        LastDiscoveryWalkedRoots = WalkedRoots;
        LastDiscoveryVisitedPaths.clear();
        LastDiscoveryVisitedPaths.reserve(Packages.size());
        for (const FDiscoveredPackage& Package : Packages)
        {
            LastDiscoveryVisitedPaths.push_back(Package.Path);
        }
        Algo::Sort(LastDiscoveryVisitedPaths);

        RunTextAssetDiscovery();
        DiscoveryWalkMs = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - WalkStart);

        const uint32 NumPackages = (uint32)Packages.size();
        if (NumPackages == 0)
        {
            OnInitialDiscoveryCompleted();
            return;
        }

        Task::AsyncTask(NumPackages, NumPackages, [this, Packages = Move(Packages)] (uint32 Start, uint32 End, uint32)
        {
            for (uint32 i = Start; i < End; ++i)
            {
                const FDiscoveredPackage& Package = Packages[i];
                ProcessPackage(FStringView(Package.Path.c_str(), Package.Path.size()), Package.MTimeNs, Package.FileSize);
            }

            if (End == Packages.size())
            {
                OnInitialDiscoveryCompleted();
            }
        }, ETaskPriority::Background);
    }

    void FAssetRegistry::OnInitialDiscoveryCompleted()
    {
        // Dropped before the registry is handed out or persisted.
        const size_t Reaped = ReapStaleEntries();

        const uint32 Unchanged = DiscoveryUnchanged.load(std::memory_order_relaxed);
        const uint32 Rehashed  = DiscoveryRehashed.load(std::memory_order_relaxed);
        const uint32 Extracted = DiscoveryExtracted.load(std::memory_order_relaxed);
        const double ProcessMs = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - DiscoveryStartCycles) - DiscoveryCacheLoadMs - DiscoveryWalkMs;

        // Persist the cache so next launch only re-parses changed assets, unless this pass changed nothing in it.
        const uint64 SaveStart = PlatformTime::Cycles();
        const bool bCacheChanged = !bDiscoveryHadCache || Rehashed > 0 || Extracted > 0 || Reaped > 0;
        if (bCacheChanged)
        {
            SaveCache();
        }
        const double SaveMs = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - SaveStart);

        ImGuiX::Notifications::NotifySuccess("Asset Registry Finished Initial Discovery: Num [{}]", Assets.size());
        LOG_INFO("Asset Registry Finished Initial Discovery: Num [{}] in {:.0f} ms (cache load {:.0f} ms, walk {:.0f} ms, "
                 "process {:.0f} ms with {} unchanged, {} rehashed, {} extracted, {} reaped, save {:.0f} ms{})",
                 Assets.size(), PlatformTime::ToMilliseconds(PlatformTime::Cycles() - DiscoveryStartCycles),
                 DiscoveryCacheLoadMs, DiscoveryWalkMs, ProcessMs, Unchanged, Rehashed, Extracted, Reaped, SaveMs,
                 bCacheChanged ? "" : ", skipped");

        // Reverse map gets built lazily on first GetReferencersOf().
        {
            FWriteScopeLock Lock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        DispatchRegistryChanged();
    }

    void FAssetRegistry::EnsurePathIndex() const
    {
        {
            FReadScopeLock Lock(AssetsMutex);
            if (bPathIndexValid)
            {
                return;
            }
        }
        FWriteScopeLock Lock(AssetsMutex);
        if (!bPathIndexValid)
        {
            RebuildPathIndex();
        }
    }

    bool FAssetRegistry::MatchesCachedStamp(FStringView Path, int64 MTimeNs, uint64 FileSize) const
    {
        // A zero stamp means the backing store could not say, so only the bytes can answer.
        if (MTimeNs == 0 || FileSize == 0)
        {
            return false;
        }

        EnsurePathIndex();
        const FString Key(VFS::RemoveExtension(Path));

        FReadScopeLock Lock(AssetsMutex);
        if (!bPathIndexValid)
        {
            return false;
        }
        auto Found = PathIndex.find(Key);
        if (Found == PathIndex.end())
        {
            return false;
        }

        // A classless entry was cached before its header was read; the UI font resolver keys on the class.
        const FAssetData* Data = Found->second;
        return !Data->AssetClass.IsNone() && Data->SourceMTimeNs == MTimeNs && Data->FileSize == FileSize;
    }

    bool FAssetRegistry::RefreshStampIfContentUnchanged(FStringView Path, uint64 ContentHash, int64 MTimeNs, uint64 FileSize)
    {
        FWriteScopeLock Lock(AssetsMutex);
        if (!bPathIndexValid)
        {
            RebuildPathIndex();
        }
        auto Found = PathIndex.find(FString(VFS::RemoveExtension(Path)));
        if (Found == PathIndex.end())
        {
            return false;
        }

        FAssetData* Data = Found->second;
        if (Data->AssetClass.IsNone() || Data->ContentHash != ContentHash)
        {
            return false;
        }
        Data->SourceMTimeNs = MTimeNs;
        Data->FileSize      = FileSize;
        return true;
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
        // Asset creation runs on a task fiber, so broadcasting inline raced the main thread's redraw.
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
            InsertAssetLocked(Move(AssetData));
        }

        {
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        NotifyRegistryChanged();
    }

    void FAssetRegistry::InsertAssetLocked(TUniquePtr<FAssetData>&& AssetData)
    {
        if (!bPathIndexValid)
        {
            RebuildPathIndex();
        }

        // A replace import or a file dropped over an old one leaves a new GUID at a known path, and two entries for one path make lookups pick either.
        FString PathKey(VFS::RemoveExtension(AssetData->Path));
        auto Stale = PathIndex.find(PathKey);
        if (Stale != PathIndex.end())
        {
            auto Entry = Assets.find_as(Stale->second->AssetGUID, FGuidHash(), FAssetDataGuidEqual());
            if (Entry != Assets.end())
            {
                Assets.erase(Entry);
            }
            PathIndex.erase(Stale);
        }

        // An external move keeps the GUID, so a stale entry would otherwise leave a dangling old path.
        auto SameGuid = Assets.find_as(AssetData->AssetGUID, FGuidHash(), FAssetDataGuidEqual());
        if (SameGuid != Assets.end())
        {
            auto OldKey = PathIndex.find(FString(VFS::RemoveExtension((*SameGuid)->Path)));
            if (OldKey != PathIndex.end() && OldKey->second == SameGuid->get())
            {
                PathIndex.erase(OldKey);
            }
            Assets.erase(SameGuid);
        }

        FAssetData* Added = AssetData.get();
        Assets.emplace(Move(AssetData));
        PathIndex[Move(PathKey)] = Added;
    }

    void FAssetRegistry::AssetDeleted(const FGuid& GUID)
    {
        {
            FWriteScopeLock Lock(AssetsMutex);
            InvalidatePathIndex();

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
            InvalidatePathIndex();

            auto It = Algo::FindIf(Assets, [&OldPath](const TUniquePtr<FAssetData>& Asset)
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
            auto Colliding = Algo::FindIf(Assets, [&](const TUniquePtr<FAssetData>& Asset)
            {
                return Asset->AssetGUID != RenamedGuid && Asset->Path == NewPath;
            });
            if (Colliding != Assets.end())
            {
                LOG_WARN("AssetRegistry::AssetRenamed: dropping stale entry at {} colliding with rename {} -> {}", NewPath, OldPath, NewPath);
                Assets.erase(Colliding);
                // hash_set::erase can invalidate other iterators; re-find.
                It = Algo::FindIf(Assets, [&OldPath](const TUniquePtr<FAssetData>& Asset)
                {
                    return Asset->Path == OldPath;
                });
                if (It == Assets.end()) return;
            }

            const TUniquePtr<FAssetData>& Data = *It;
            Data->Path.assign(NewPath);
            Data->AssetName    = VFS::FileName(NewPath, true);
            Data->OwningPlugin = ExtractOwningPlugin(NewPath);
        }

        NotifyRegistryChanged();
    }

    void FAssetRegistry::AssetSaved(CObject* Asset)
    {
        // The saved package's import table and content hash may have changed, so the reverse map dies.
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

        auto It = Assets.find_as(GUID, FGuidHash(), FAssetDataGuidEqual());
        return It == Assets.end() ? nullptr : It->get();
    }

    void FAssetRegistry::RebuildPathIndex() const
    {
        PathIndex.clear();
        PathIndex.reserve(Assets.size());

        for (const TUniquePtr<FAssetData>& Data : Assets)
        {
            PathIndex.emplace(FString(VFS::RemoveExtension(Data->Path)), Data.get());
        }
        bPathIndexValid = true;
    }

    FAssetData* FAssetRegistry::GetAssetByPath(FStringView Path) const
    {
        LUMINA_PROFILE_SCOPE();
        // The index hashes strings and views alike, so the lookup needs no copy of the key.
        const FStringView Key = VFS::RemoveExtension(Path);

        {
            FReadScopeLock Lock(AssetsMutex);
            if (bPathIndexValid)
            {
                auto Found = PathIndex.find(Key);
                return Found == PathIndex.end() ? nullptr : Found->second;
            }
        }

        FWriteScopeLock Lock(AssetsMutex);
        if (!bPathIndexValid)
        {
            RebuildPathIndex();
        }

        auto It = PathIndex.find(Key);
        return It == PathIndex.end() ? nullptr : It->second;
    }

    TVector<FAssetData*> FAssetRegistry::FindByPredicate(const TFunction<bool(const FAssetData&)>& Predicate) const
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
        LUMINA_MEMORY_SCOPE("Asset Registry");
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
            Data->Path          .assign(Vp);
            Data->Name          = VFS::FileName(Vp, true);
            Data->Kind          = Kind;
            Data->OwningPlugin  = ExtractOwningPlugin(Vp);
            Data->SourceMTimeNs = FileMTimeNanos(Vp);

            // The first of two files sharing a stale GUID wins, and the duplicate re-mints next pass.
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
        Data->Path          .assign(Path);
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
        auto It = Algo::FindIf(TextAssets, [&](const TUniquePtr<FTextAssetData>& Data)
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

            auto It = Algo::FindIf(TextAssets, [&](const TUniquePtr<FTextAssetData>& Data)
            {
                return FStringView(Data->Path.c_str(), Data->Path.size()) == OldPath;
            });
            if (It == TextAssets.end())
            {
                return;
            }

            // Drop a stale entry already sitting at NewPath with a different GUID.
            const FGuid RenamedGuid = (*It)->Guid;
            auto Colliding = Algo::FindIf(TextAssets, [&](const TUniquePtr<FTextAssetData>& Data)
            {
                return Data->Guid != RenamedGuid && FStringView(Data->Path.c_str(), Data->Path.size()) == NewPath;
            });
            if (Colliding != TextAssets.end())
            {
                TextAssets.erase(Colliding);
                It = Algo::FindIf(TextAssets, [&](const TUniquePtr<FTextAssetData>& Data)
                {
                    return FStringView(Data->Path.c_str(), Data->Path.size()) == OldPath;
                });
                if (It == TextAssets.end()) return;
            }

            const TUniquePtr<FTextAssetData>& Data = *It;
            Data->Path.assign(NewPath);
            Data->Name         = VFS::FileName(NewPath, true);
            Data->Kind         = TextAsset::KindFromPath(NewPath);
            Data->OwningPlugin = ExtractOwningPlugin(NewPath);
            bRenamed = true;
        }

        if (bRenamed)
        {
            // Outside the lock, since subscribers may read the registry back.
            FCoreDelegates::OnContentFileRenamed.Broadcast(OldPath, NewPath);
            NotifyRegistryChanged();
        }
    }

    void FAssetRegistry::TextAssetDeleted(FStringView Path)
    {
        TextAssetSidecar::Delete(Path);

        {
            FWriteScopeLock Lock(TextAssetsMutex);
            auto It = Algo::FindIf(TextAssets, [&](const TUniquePtr<FTextAssetData>& Data)
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

    size_t FAssetRegistry::ReapStaleEntries()
    {
        if (LastDiscoveryWalkedRoots.empty())
        {
            return 0;
        }

        FWriteScopeLock Lock(AssetsMutex);
        InvalidatePathIndex();

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

            const bool bVisited = Algo::BinarySearch(
                LastDiscoveryVisitedPaths,
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
        return Reaped;
    }

    void FAssetRegistry::RebuildReverseMap()
    {
        LUMINA_MEMORY_SCOPE("Asset Registry");
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
        // The const-cast is fine, since the map cache is mutable state.
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
        const FPathString Resolved = VFS::ResolvePath(Path);
        const Filesystem::FFileStat Stat = Resolved.empty() ? Filesystem::FFileStat{} : Filesystem::Stat(Resolved);
        ProcessPackage(Path, Stat.bValid ? Stat.LastModifyTime : 0, Stat.bValid ? Stat.Size : 0);
    }

    void FAssetRegistry::ProcessPackage(FStringView Path, int64 MTimeNs, uint64 FileSize)
    {
        LUMINA_MEMORY_SCOPE("Asset Registry");
        if (MatchesCachedStamp(Path, MTimeNs, FileSize))
        {
            DiscoveryUnchanged.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // The hash covers raw compressed bytes, so a source or compression change invalidates it.
        TVector<uint8> RawBytes;
        const uint64 Hash = ContentHashOf(Path, &RawBytes);
        if (Hash != 0 && RefreshStampIfContentUnchanged(Path, Hash, MTimeNs, FileSize))
        {
            DiscoveryRehashed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        DiscoveryExtracted.fetch_add(1, std::memory_order_relaxed);

        if (RawBytes.empty())
        {
            LOG_ERROR("AssetRegistry: failed to read {}", Path);
            RecordFailedAsset(Path);
            return;
        }

        // ReadPackageFile decompresses into Bytes, which holds the header and import and export tables.
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
        FPackageContainerReader Reader(Bytes);
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

        // the export and import tables name things through slots, so the table comes first
        FPackageNameTable Names;
        if (!CPackage::ReadNameTable(Reader, Header, Names))
        {
            LOG_ERROR("AssetRegistry: {} has an unreadable name table", Path);
            RecordFailedAsset(Path);
            return;
        }
        Reader.SetNameTable(&Names);

        if (Header.ExportTableOffset < 0 || static_cast<size_t>(Header.ExportTableOffset) > Bytes.size())
        {
            LOG_ERROR("AssetRegistry: {} has out-of-range export table offset", Path);
            RecordFailedAsset(Path);
            return;
        }

        Reader.Seek(Header.ExportTableOffset);

        TVector<FObjectExport> Exports;
        Reader << Exports;

        FObjectExport* Export = Algo::FindIf(Exports, [&](const FObjectExport& E)
        {
            return E.ObjectName == PackageFileName;
        });

        if (Export == Exports.end())
        {
            LOG_ERROR("AssetRegistry: {} contains no export matching its file name; refusing to register", Path);
            RecordFailedAsset(Path);
            return;
        }

        // Hard for a direct CObject reference and Soft for an FSoftObjectPath, as the saver recorded.
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
        AssetData->Path           .assign(Path);
        AssetData->ContentHash    = Hash;
        AssetData->SourceMTimeNs  = MTimeNs;
        AssetData->FileSize       = FileSize;
        AssetData->Dependencies   = Move(Dependencies);
        AssetData->OwningPlugin   = ExtractOwningPlugin(Path);

        FWriteScopeLock Lock(AssetsMutex);
        InsertAssetLocked(Move(AssetData));
    }

    void FAssetRegistry::RecordFailedAsset(FStringView Path)
    {
        FWriteScopeLock Lock(FailedAssetsMutex);
        FailedAssets.emplace_back(Path.data(), Path.size());
    }

    void FAssetRegistry::ClearAssets()
    {
        // Listeners read the registry straight back, and AssetsMutex is not recursive.
        {
            FWriteScopeLock Lock(AssetsMutex);
            InvalidatePathIndex();
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
        LUMINA_MEMORY_SCOPE("Asset Registry");
        uint32 Tag = 0;
        Ar << Tag;
        if (Tag != kAssetRegistryCacheTag)
        {
            return false;
        }

        uint32 Count = 0;
        Ar << Count;

        FWriteScopeLock Lock(AssetsMutex);
        InvalidatePathIndex();
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

    // FGuid and FFixedString have no leaf overload, so they round-trip through an FString.
    namespace
    {
        // A JSON cache cost 1.8 s to parse and 2 s to write at 109k entries, so the cache is a flat binary stream.
        constexpr uint32 kAssetRegistryCacheMagic   = 0x4352414C;
        constexpr uint32 kAssetRegistryCacheVersion = 1;

        // A damaged count is refused before it sizes an allocation.
        constexpr uint32 kMaxCachedDependencies = 1u << 16;

        void SerializeName(FArchive& Ar, FName& Name)
        {
            FString Text;
            if (Ar.IsWriting() && !Name.IsNone())
            {
                Text = Name.c_str();
            }
            Ar << Text;
            if (Ar.IsReading())
            {
                Name = Text.empty() ? FName() : FName(Text);
            }
        }

        void SerializeAssetEntry(FArchive& Ar, FAssetData& Data)
        {
            Ar << Data.AssetGUID;
            Ar << Data.Path;
            SerializeName(Ar, Data.AssetName);
            SerializeName(Ar, Data.AssetClass);
            Ar << Data.ContentHash;
            Ar << Data.SourceMTimeNs;
            Ar << Data.FileSize;

            uint32 Flags = (uint32)Data.Flags;
            Ar << Flags;
            Data.Flags = (EAssetFlags)Flags;

            SerializeName(Ar, Data.OwnerChunk);
            SerializeName(Ar, Data.OwningPlugin);

            uint32 DependencyCount = (uint32)Data.Dependencies.size();
            Ar << DependencyCount;
            if (Ar.IsReading())
            {
                if (DependencyCount > kMaxCachedDependencies)
                {
                    Ar.SetHasError(true);
                    return;
                }
                Data.Dependencies.resize(DependencyCount);
            }
            for (FAssetDependency& Dependency : Data.Dependencies)
            {
                Ar << Dependency.TargetGUID;
                uint8 Type = (uint8)Dependency.Type;
                Ar << Type;
                Dependency.Type = (EDependencyType)Type;
            }
        }

        void SerializeTextEntry(FArchive& Ar, FTextAssetData& Data)
        {
            Ar << Data.Guid;
            Ar << Data.Path;
            SerializeName(Ar, Data.Name);

            uint8 Kind = (uint8)Data.Kind;
            Ar << Kind;
            Data.Kind = (ETextAssetKind)Kind;

            SerializeName(Ar, Data.OwningPlugin);
            Ar << Data.SourceMTimeNs;
        }
    }

    void FAssetRegistry::SaveCache() const
    {
        const FString CachePath = AssetDbPath();
        if (CachePath.empty()) return;

        Filesystem::MakeParentDirectoryTree(CachePath);

        TVector<uint8> Bytes;
        FMemoryWriter Writer(Bytes);

        uint32 Magic   = kAssetRegistryCacheMagic;
        uint32 Version = kAssetRegistryCacheVersion;
        Writer << Magic;
        Writer << Version;

        {
            FReadScopeLock Lock(AssetsMutex);
            uint32 Count = (uint32)Assets.size();
            Writer << Count;
            for (const TUniquePtr<FAssetData>& Data : Assets)
            {
                SerializeAssetEntry(Writer, *Data);
            }
        }

        {
            FReadScopeLock TextLock(TextAssetsMutex);
            uint32 Count = (uint32)TextAssets.size();
            Writer << Count;
            for (const TUniquePtr<FTextAssetData>& Data : TextAssets)
            {
                SerializeTextEntry(Writer, *Data);
            }
        }

        if (!FileHelper::SaveArrayToFile(Bytes, CachePath))
        {
            LOG_WARN("AssetRegistry: failed to write cache to {}", CachePath);
        }
    }

    bool FAssetRegistry::LoadCache()
    {
        LUMINA_MEMORY_SCOPE("Asset Registry");
        const FString CachePath = AssetDbPath();
        if (CachePath.empty()) return false;

        // A first launch has no cache, so exit quietly and let the full rescan handle it.
        if (!Filesystem::Exists(CachePath))
        {
            return false;
        }

        TVector<uint8> Bytes;
        if (!FileHelper::LoadFileToArray(Bytes, CachePath) || Bytes.empty())
        {
            return false;
        }

        FMemoryReader Reader(Bytes);
        uint32 Magic   = 0;
        uint32 Version = 0;
        Reader << Magic;
        Reader << Version;
        if (Reader.HasError() || Magic != kAssetRegistryCacheMagic || Version != kAssetRegistryCacheVersion)
        {
            LOG_INFO("AssetRegistry: cache at {} is from another build; rebuilding from scratch", CachePath);
            return false;
        }

        // Read whole before anything is replaced, so a damaged file leaves the registry as it was.
        uint32 AssetCount = 0;
        Reader << AssetCount;
        TVector<TUniquePtr<FAssetData>> LoadedAssets;
        LoadedAssets.reserve(Reader.HasError() ? 0 : Math::Min<uint32>(AssetCount, (uint32)(Bytes.size() / 32)));
        for (uint32 i = 0; i < AssetCount && !Reader.HasError(); ++i)
        {
            auto Data = MakeUnique<FAssetData>();
            SerializeAssetEntry(Reader, *Data);
            LoadedAssets.push_back(Move(Data));
        }

        uint32 TextCount = 0;
        Reader << TextCount;
        TVector<TUniquePtr<FTextAssetData>> LoadedText;
        for (uint32 i = 0; i < TextCount && !Reader.HasError(); ++i)
        {
            auto Data = MakeUnique<FTextAssetData>();
            SerializeTextEntry(Reader, *Data);
            LoadedText.push_back(Move(Data));
        }

        if (Reader.HasError())
        {
            LOG_WARN("AssetRegistry: cache at {} is damaged; rebuilding from scratch", CachePath);
            return false;
        }

        {
            FWriteScopeLock Lock(AssetsMutex);
            InvalidatePathIndex();
            Assets.clear();
            Assets.reserve(LoadedAssets.size());
            for (TUniquePtr<FAssetData>& Data : LoadedAssets)
            {
                Assets.emplace(Move(Data));
            }
        }

        {
            FWriteScopeLock TextLock(TextAssetsMutex);
            TextAssets.clear();
            TextAssets.reserve(LoadedText.size());
            for (TUniquePtr<FTextAssetData>& Data : LoadedText)
            {
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
